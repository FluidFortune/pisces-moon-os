// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <NimBLEAdvertising.h>

#include "ble_keymote.h"
#include "apps.h"
#include "theme.h"
#include "keyboard.h"
#include "pm_input.h"
#include "touch.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
static constexpr int DISP_W = 480;
static constexpr int DISP_H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 240;
static constexpr int DISP_H = 135;
#else
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 240;
#endif

bool kmAdvertising = false;
bool kmConnected   = false;

static NimBLEServer*         kmServer = nullptr;
static NimBLECharacteristic* kmInput  = nullptr;

#define KM_BUF_MAX 128
static char    kmBuf[KM_BUF_MAX + 1];
static uint8_t kmBufLen = 0;
static bool    kmBufferMode = false;
static bool    kmDirty = true;

static const uint8_t KM_HID_DESCRIPTOR[] = {
    0x05, 0x01,  0x09, 0x06,  0xA1, 0x01,  0x85, 0x01,
    0x05, 0x07,  0x19, 0xE0,  0x29, 0xE7,  0x15, 0x00,
    0x25, 0x01,  0x75, 0x01,  0x95, 0x08,  0x81, 0x02,
    0x95, 0x01,  0x75, 0x08,  0x81, 0x01,
    0x95, 0x05,  0x75, 0x01,  0x05, 0x08,  0x19, 0x01,
    0x29, 0x05,  0x91, 0x02,  0x95, 0x01,  0x75, 0x03,
    0x91, 0x01,
    0x95, 0x06,  0x75, 0x08,  0x15, 0x00,  0x25, 0x65,
    0x05, 0x07,  0x19, 0x00,  0x29, 0x65,  0x81, 0x00,
    0xC0
};

#define KEY_NONE       0x00
#define KEY_A          0x04
#define KEY_ENTER      0x28
#define KEY_BACKSPACE  0x2A
#define KEY_TAB        0x2B
#define KEY_SPACE      0x2C
#define MOD_NONE       0x00
#define MOD_SHIFT      0x02

struct HIDKey { uint8_t mod; uint8_t code; };

static HIDKey kmAsciiToHID(char c) {
    if (c >= 'a' && c <= 'z') return {MOD_NONE,  (uint8_t)(KEY_A + c - 'a')};
    if (c >= 'A' && c <= 'Z') return {MOD_SHIFT, (uint8_t)(KEY_A + c - 'A')};
    if (c >= '1' && c <= '9') return {MOD_NONE,  (uint8_t)(0x1E + c - '1')};
    if (c == '0')             return {MOD_NONE,  0x27};
    switch (c) {
        case ' ':  return {MOD_NONE,  KEY_SPACE};
        case '\n': case '\r': return {MOD_NONE, KEY_ENTER};
        case '\b': return {MOD_NONE,  KEY_BACKSPACE};
        case '\t': return {MOD_NONE,  KEY_TAB};
        case '!':  return {MOD_SHIFT, 0x1E};
        case '@':  return {MOD_SHIFT, 0x1F};
        case '#':  return {MOD_SHIFT, 0x20};
        case '$':  return {MOD_SHIFT, 0x21};
        case '%':  return {MOD_SHIFT, 0x22};
        case '^':  return {MOD_SHIFT, 0x23};
        case '&':  return {MOD_SHIFT, 0x24};
        case '*':  return {MOD_SHIFT, 0x25};
        case '(':  return {MOD_SHIFT, 0x26};
        case ')':  return {MOD_SHIFT, 0x27};
        case '-':  return {MOD_NONE,  0x2D};
        case '_':  return {MOD_SHIFT, 0x2D};
        case '=':  return {MOD_NONE,  0x2E};
        case '+':  return {MOD_SHIFT, 0x2E};
        case '[':  return {MOD_NONE,  0x2F};
        case '{':  return {MOD_SHIFT, 0x2F};
        case ']':  return {MOD_NONE,  0x30};
        case '}':  return {MOD_SHIFT, 0x30};
        case '\\': return {MOD_NONE,  0x31};
        case '|':  return {MOD_SHIFT, 0x31};
        case ';':  return {MOD_NONE,  0x33};
        case ':':  return {MOD_SHIFT, 0x33};
        case '\'': return {MOD_NONE,  0x34};
        case '"':  return {MOD_SHIFT, 0x34};
        case '`':  return {MOD_NONE,  0x35};
        case '~':  return {MOD_SHIFT, 0x35};
        case ',':  return {MOD_NONE,  0x36};
        case '<':  return {MOD_SHIFT, 0x36};
        case '.':  return {MOD_NONE,  0x37};
        case '>':  return {MOD_SHIFT, 0x37};
        case '/':  return {MOD_NONE,  0x38};
        case '?':  return {MOD_SHIFT, 0x38};
        default:   return {MOD_NONE,  KEY_NONE};
    }
}

#define KM_HID_SERVICE_UUID    "1812"
#define KM_HID_INFO_UUID       "2A4A"
#define KM_HID_REPORT_MAP_UUID "2A4B"
#define KM_HID_CONTROL_UUID    "2A4C"
#define KM_HID_REPORT_UUID     "2A4D"
#define KM_DEV_INFO_UUID       "180A"
#define KM_PNP_ID_UUID         "2A50"
#define KM_BATTERY_UUID        "180F"
#define KM_BAT_LEVEL_UUID      "2A19"

class KMServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s) override {
        kmConnected = true; kmDirty = true;
        Serial.println("[KEYMOTE] Host connected");
    }
    void onDisconnect(NimBLEServer* s) override {
        kmConnected = false; kmDirty = true;
        Serial.println("[KEYMOTE] Host disconnected");
        NimBLEDevice::startAdvertising();
    }
};
static KMServerCB kmCB;

static void kmSendReport(uint8_t mod, uint8_t key) {
    if (!kmConnected || !kmInput) return;
    uint8_t report[8] = {mod, 0x00, key, 0,0,0,0,0};
    kmInput->setValue(report, sizeof(report)); kmInput->notify();
    delay(8);
    memset(report, 0, sizeof(report));
    kmInput->setValue(report, sizeof(report)); kmInput->notify();
    delay(5);
}

static void kmSendChar(char c) {
    HIDKey k = kmAsciiToHID(c);
    if (k.code != KEY_NONE) kmSendReport(k.mod, k.code);
}

static void kmSendString(const char* s) {
    while (*s) { kmSendChar(*s++); delay(10); }
}

static void kmInitBLE() {
    NimBLEDevice::init("Pisces Keymote");
    NimBLEDevice::setSecurityAuth(false, false, true);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    kmServer = NimBLEDevice::createServer();
    kmServer->setCallbacks(&kmCB);

    NimBLEService* devSvc = kmServer->createService(KM_DEV_INFO_UUID);
    NimBLECharacteristic* pnp = devSvc->createCharacteristic(KM_PNP_ID_UUID, NIMBLE_PROPERTY::READ);
    uint8_t pnpData[] = {0x02, 0x5E, 0x04, 0x00, 0x08, 0x00, 0x01};
    pnp->setValue(pnpData, sizeof(pnpData));
    devSvc->start();

    NimBLEService* batSvc = kmServer->createService(KM_BATTERY_UUID);
    NimBLECharacteristic* bat = batSvc->createCharacteristic(KM_BAT_LEVEL_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    uint8_t batLevel = 100; bat->setValue(&batLevel, 1);
    batSvc->start();

    NimBLEService* hid = kmServer->createService(KM_HID_SERVICE_UUID);
    NimBLECharacteristic* hidInfo = hid->createCharacteristic(KM_HID_INFO_UUID, NIMBLE_PROPERTY::READ);
    uint8_t hidInfoData[] = {0x11, 0x01, 0x00, 0x02};
    hidInfo->setValue(hidInfoData, sizeof(hidInfoData));

    NimBLECharacteristic* reportMap = hid->createCharacteristic(KM_HID_REPORT_MAP_UUID, NIMBLE_PROPERTY::READ);
    reportMap->setValue((uint8_t*)KM_HID_DESCRIPTOR, sizeof(KM_HID_DESCRIPTOR));

    hid->createCharacteristic(KM_HID_CONTROL_UUID, NIMBLE_PROPERTY::WRITE_NR);

    kmInput = hid->createCharacteristic(KM_HID_REPORT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    NimBLEDescriptor* refDesc = kmInput->createDescriptor("2908", NIMBLE_PROPERTY::READ, 2);
    uint8_t refData[] = {0x01, 0x01};
    refDesc->setValue(refData, sizeof(refData));

    hid->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(0x03C1);
    adv->addServiceUUID(KM_HID_SERVICE_UUID);
    adv->setScanResponse(false);
    adv->start();
    kmAdvertising = true;
    Serial.println("[KEYMOTE] Advertising as 'Pisces Keymote'");
}

static void kmDraw() {
    if (!kmDirty) return;
    kmDirty = false;
    gfx->fillScreen(C_BLACK);

    // Header
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->drawFastHLine(0, 24, DISP_W, C_GREEN);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
    gfx->print("BLE KEYMOTE | " PM_EXIT_COPY);

    // Status
    gfx->setCursor(10, 40); gfx->setTextColor(C_WHITE);
    gfx->print("Status: ");
    if (kmConnected)       { gfx->setTextColor(C_GREEN); gfx->print("PAIRED"); }
    else if (kmAdvertising){ gfx->setTextColor(C_CYAN);  gfx->print("Advertising..."); }
    else                   { gfx->setTextColor(C_RED);   gfx->print("Off"); }

    gfx->setTextColor(C_GREY); gfx->setCursor(10, 60);
    gfx->print("Pair from host as:");
    gfx->setTextColor(C_CYAN); gfx->setCursor(10, 75);
    gfx->print("'Pisces Keymote'");

    gfx->setTextColor(C_WHITE); gfx->setCursor(10, 100);
    gfx->print("Mode: ");
    gfx->setTextColor(C_GREEN);
    gfx->print(kmBufferMode ? "BUFFER (Enter=send)" : "LIVE");

    if (kmBufferMode) {
        gfx->setTextColor(C_GREY); gfx->setCursor(10, 120);
        gfx->print("Buffer:");
        gfx->setTextColor(C_WHITE); gfx->setCursor(10, 135);
        int previewMax = (DISP_W - 30) / 6;
        char preview[80];
        int pl = kmBufLen > previewMax ? previewMax : kmBufLen;
        if (pl > 75) pl = 75;
        memcpy(preview, kmBuf, pl);
        preview[pl] = 0;
        gfx->print(preview);
        gfx->print("_");
    }

    gfx->setTextColor(C_GREY);
    gfx->setCursor(10, DISP_H - 30);
    gfx->print("Press ` to toggle mode");
    gfx->setCursor(10, DISP_H - 15);
    gfx->print(PM_EXIT_COPY);
}

void keymoteEnter() {
    kmBufLen = 0; kmBuf[0] = 0; kmBufferMode = false; kmDirty = true;
    if (!kmServer) kmInitBLE();
    else if (!kmAdvertising) { NimBLEDevice::startAdvertising(); kmAdvertising = true; }
    kmDraw();
}

bool keymoteLoopOnce() {
    int16_t tx, ty;
    if (get_touch(&tx, &ty)) {
        if (ty < 30) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            return true;
        }
    }
    char c = get_keypress();
    if (c != 0) {
        if (pm_is_exit_key(c)) return true;
        if (c == '`') {
            kmBufferMode = !kmBufferMode;
            kmBufLen = 0; kmBuf[0] = 0;
            kmDirty = true; kmDraw();
            return false;
        }
        if (!kmConnected) { kmDirty = true; kmDraw(); return false; }
        if (kmBufferMode) {
            if (c == 13 || c == 10) {
                if (kmBufLen > 0) {
                    kmBuf[kmBufLen] = 0;
                    kmSendString(kmBuf);
                    kmSendChar('\n');
                    kmBufLen = 0; kmBuf[0] = 0;
                }
                kmDirty = true; kmDraw();
            } else if (c == 8 || c == 127) {
                if (kmBufLen > 0) {
                    kmBufLen--; kmBuf[kmBufLen] = 0;
                    kmDirty = true; kmDraw();
                }
            } else if (kmBufLen < KM_BUF_MAX) {
                kmBuf[kmBufLen++] = c;
                kmBuf[kmBufLen]   = 0;
                kmDirty = true; kmDraw();
            }
        } else {
            kmSendChar(c);
        }
    }
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 500) {
        lastDraw = millis();
        if (kmDirty) kmDraw();
    }
    return false;
}

void keymoteExit() {
    if (kmAdvertising) {
        NimBLEDevice::getAdvertising()->stop();
        kmAdvertising = false;
    }
    Serial.println("[KEYMOTE] Exited");
}
