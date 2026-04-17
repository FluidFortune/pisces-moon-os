// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// This program is free software: you can redistribute it
// and/or modify it under the terms of the GNU Affero General
// Public License as published by the Free Software Foundation,
// either version 3 of the License, or any later version.
//
// fluidfortune.com

/**
 * PISCES MOON OS — BLE DUCKY v1.0
 * Wireless rubber ducky via BLE HID keyboard.
 *
 * Architecture:
 *   NimBLE HID server advertises as "PM-Keyboard" (or custom name from payload header).
 *   Target pairs via normal Bluetooth settings — appears as standard keyboard.
 *   Once connected, executes DuckyScript payload file from SD card.
 *   Payload files live in /payloads/ (Ghost Partition in Tactical Mode).
 *
 * DuckyScript subset supported:
 *   REM <text>          Comment — ignored
 *   DELAY <ms>          Wait N milliseconds
 *   STRING <text>       Type text verbatim
 *   ENTER               Press Enter
 *   GUI [key]           Windows/Super key, optionally with another key (GUI r = Win+R)
 *   ALT [key]           Alt key combo
 *   CTRL [key]          Ctrl key combo
 *   SHIFT [key]         Shift key combo
 *   TAB                 Tab key
 *   ESCAPE / ESC        Escape key
 *   BACKSPACE           Backspace
 *   DELETE              Delete key
 *   INSERT              Insert key
 *   HOME / END          Home/End keys
 *   UPARROW / DOWNARROW / LEFTARROW / RIGHTARROW
 *   F1..F12             Function keys
 *   CAPSLOCK / NUMLOCK / SCROLLLOCK
 *   PRINTSCREEN
 *   PAUSE / BREAK
 *   REPEAT <n>          Repeat previous command N times
 *   DEFAULT_DELAY <ms>  Set delay between each line
 *   DEFAULTDELAY <ms>   Alias for DEFAULT_DELAY
 *   NAME <text>         Set BLE device name (must be first line if used)
 *   WAIT_FOR_CONNECT    Pause until host connects (implicit at payload start)
 *
 * NimBLE coexistence:
 *   wardrive_ble_stop() called before HID server starts.
 *   wardrive_ble_resume() called on exit.
 *   Do NOT run wardrive and BLE Ducky simultaneously — same NimBLE singleton.
 *
 * USE ONLY ON SYSTEMS YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLECharacteristic.h>
#include <NimBLEDescriptor.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "wardrive.h"
#include "ble_ducky.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define BD_BG     0x0000
#define BD_HDR    0x2000   // Dark red
#define BD_RED    0xF800
#define BD_ORANGE 0xFD20
#define BD_GREEN  0x07E0
#define BD_CYAN   0x07FF
#define BD_WHITE  0xFFFF
#define BD_DIM    0x4208
#define BD_SEL    0x2800

#define PAYLOAD_DIR "/payloads"

// ─────────────────────────────────────────────
//  HID REPORT DESCRIPTOR — BOOT KEYBOARD
//  Standard 8-byte keyboard HID descriptor.
//  Accepted by every major OS without driver install.
// ─────────────────────────────────────────────
static const uint8_t BD_HID_DESCRIPTOR[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    // Modifier keys (1 byte)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Minimum (224)
    0x29, 0xE7,  //   Usage Maximum (231)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Variable, Absolute)
    // Reserved byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant)
    // Keycode array (6 keys)
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x73,  //   Logical Maximum (115)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x73,  //   Usage Maximum (115)
    0x81, 0x00,  //   Input (Data, Array)
    0xC0         // End Collection
};

// ─────────────────────────────────────────────
//  HID KEY CODES (USB HID 1.12 keyboard page)
// ─────────────────────────────────────────────
// Modifier bitmask
#define MOD_NONE   0x00
#define MOD_LCTRL  0x01
#define MOD_LSHIFT 0x02
#define MOD_LALT   0x04
#define MOD_LGUI   0x08
#define MOD_RCTRL  0x10
#define MOD_RSHIFT 0x20
#define MOD_RALT   0x40
#define MOD_RGUI   0x80

// Key codes
#define KEY_NONE   0x00
#define KEY_A      0x04
#define KEY_Z      0x1D
#define KEY_1      0x1E
#define KEY_0      0x27
#define KEY_ENTER  0x28
#define KEY_ESC    0x29
#define KEY_BKSP   0x2A
#define KEY_TAB    0x2B
#define KEY_SPACE  0x2C
#define KEY_MINUS  0x2D
#define KEY_EQUAL  0x2E
#define KEY_LBRK   0x2F
#define KEY_RBRK   0x30
#define KEY_BKSL   0x31
#define KEY_SEMI   0x33
#define KEY_QUOTE  0x34
#define KEY_GRAVE  0x35
#define KEY_COMMA  0x36
#define KEY_DOT    0x37
#define KEY_SLASH  0x38
#define KEY_CAPS   0x39
#define KEY_F1     0x3A
#define KEY_F12    0x45
#define KEY_PRTSC  0x46
#define KEY_SCRLK  0x47
#define KEY_PAUSE  0x48
#define KEY_INS    0x49
#define KEY_HOME   0x4A
#define KEY_PGUP   0x4B
#define KEY_DEL    0x4C
#define KEY_END    0x4D
#define KEY_PGDN   0x4E
#define KEY_RIGHT  0x4F
#define KEY_LEFT   0x50
#define KEY_DOWN   0x51
#define KEY_UP     0x52
#define KEY_NUMLK  0x53
#define KEY_GUI    0x08   // Left GUI (as modifier bit)

// ─────────────────────────────────────────────
//  ASCII → HID KEYCODE TRANSLATION
// ─────────────────────────────────────────────
struct HIDKey { uint8_t mod; uint8_t code; };

static HIDKey bdAsciiToHID(char c) {
    // Lowercase a-z
    if (c >= 'a' && c <= 'z') return {MOD_NONE, (uint8_t)(KEY_A + (c - 'a'))};
    // Uppercase A-Z
    if (c >= 'A' && c <= 'Z') return {MOD_LSHIFT, (uint8_t)(KEY_A + (c - 'A'))};
    // Digits 1-9, 0
    if (c >= '1' && c <= '9') return {MOD_NONE, (uint8_t)(KEY_1 + (c - '1'))};
    if (c == '0') return {MOD_NONE, KEY_0};

    // Special characters
    switch(c) {
        case ' ':  return {MOD_NONE,   KEY_SPACE};
        case '\n': return {MOD_NONE,   KEY_ENTER};
        case '\t': return {MOD_NONE,   KEY_TAB};
        case '-':  return {MOD_NONE,   KEY_MINUS};
        case '_':  return {MOD_LSHIFT, KEY_MINUS};
        case '=':  return {MOD_NONE,   KEY_EQUAL};
        case '+':  return {MOD_LSHIFT, KEY_EQUAL};
        case '[':  return {MOD_NONE,   KEY_LBRK};
        case '{':  return {MOD_LSHIFT, KEY_LBRK};
        case ']':  return {MOD_NONE,   KEY_RBRK};
        case '}':  return {MOD_LSHIFT, KEY_RBRK};
        case '\\': return {MOD_NONE,   KEY_BKSL};
        case '|':  return {MOD_LSHIFT, KEY_BKSL};
        case ';':  return {MOD_NONE,   KEY_SEMI};
        case ':':  return {MOD_LSHIFT, KEY_SEMI};
        case '\'': return {MOD_NONE,   KEY_QUOTE};
        case '"':  return {MOD_LSHIFT, KEY_QUOTE};
        case '`':  return {MOD_NONE,   KEY_GRAVE};
        case '~':  return {MOD_LSHIFT, KEY_GRAVE};
        case ',':  return {MOD_NONE,   KEY_COMMA};
        case '<':  return {MOD_LSHIFT, KEY_COMMA};
        case '.':  return {MOD_NONE,   KEY_DOT};
        case '>':  return {MOD_LSHIFT, KEY_DOT};
        case '/':  return {MOD_NONE,   KEY_SLASH};
        case '?':  return {MOD_LSHIFT, KEY_SLASH};
        case '!':  return {MOD_LSHIFT, KEY_1};
        case '@':  return {MOD_LSHIFT, (uint8_t)(KEY_1+1)};
        case '#':  return {MOD_LSHIFT, (uint8_t)(KEY_1+2)};
        case '$':  return {MOD_LSHIFT, (uint8_t)(KEY_1+3)};
        case '%':  return {MOD_LSHIFT, (uint8_t)(KEY_1+4)};
        case '^':  return {MOD_LSHIFT, (uint8_t)(KEY_1+5)};
        case '&':  return {MOD_LSHIFT, (uint8_t)(KEY_1+6)};
        case '*':  return {MOD_LSHIFT, (uint8_t)(KEY_1+7)};
        case '(':  return {MOD_LSHIFT, (uint8_t)(KEY_1+8)};
        case ')':  return {MOD_LSHIFT, KEY_0};
        default:   return {MOD_NONE,   KEY_NONE};
    }
}

// ─────────────────────────────────────────────
//  NimBLE HID SERVER STATE
// ─────────────────────────────────────────────
static NimBLEServer*         bdServer     = nullptr;
static NimBLECharacteristic* bdInput      = nullptr;
static volatile bool         bdConnected  = false;
static volatile bool         bdWasConnected = false;

// UUIDs for HID over GATT
#define HID_SERVICE_UUID         "1812"
#define HID_REPORT_MAP_UUID      "2A4B"
#define HID_REPORT_UUID          "2A4D"
#define HID_INFO_UUID            "2A4A"
#define HID_CONTROL_POINT_UUID   "2A4C"
#define BATTERY_SERVICE_UUID     "180F"
#define BATTERY_LEVEL_UUID       "2A19"
#define DEVICE_INFO_UUID         "180A"
#define PNP_ID_UUID              "2A50"

class BDServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s) override {
        bdConnected    = true;
        bdWasConnected = true;
        Serial.println("[BLE DUCKY] Host connected");
    }
    void onDisconnect(NimBLEServer* s) override {
        bdConnected = false;
        Serial.println("[BLE DUCKY] Host disconnected");
        NimBLEDevice::startAdvertising();
    }
};
static BDServerCallbacks bdServerCB;

// ─────────────────────────────────────────────
//  HID REPORT SENDER
// ─────────────────────────────────────────────
static void bdSendReport(uint8_t mod, uint8_t key) {
    if (!bdConnected || !bdInput) return;
    uint8_t report[8] = {mod, 0x00, key, 0,0,0,0,0};
    bdInput->setValue(report, sizeof(report));
    bdInput->notify();
    delay(8);
    // Key release
    memset(report, 0, sizeof(report));
    bdInput->setValue(report, sizeof(report));
    bdInput->notify();
    delay(5);
}

static void bdTypeString(const char* s) {
    while (*s) {
        HIDKey k = bdAsciiToHID(*s++);
        if (k.code != KEY_NONE) bdSendReport(k.mod, k.code);
        delay(10);
    }
}

// ─────────────────────────────────────────────
//  DUCKYSCRIPT KEYWORD → KEY CODE
// ─────────────────────────────────────────────
static uint8_t bdKeywordCode(const char* word) {
    if (strcmp(word,"ENTER")==0||strcmp(word,"RETURN")==0) return KEY_ENTER;
    if (strcmp(word,"TAB")==0)       return KEY_TAB;
    if (strcmp(word,"ESCAPE")==0||strcmp(word,"ESC")==0) return KEY_ESC;
    if (strcmp(word,"BACKSPACE")==0) return KEY_BKSP;
    if (strcmp(word,"DELETE")==0||strcmp(word,"DEL")==0) return KEY_DEL;
    if (strcmp(word,"INSERT")==0)    return KEY_INS;
    if (strcmp(word,"HOME")==0)      return KEY_HOME;
    if (strcmp(word,"END")==0)       return KEY_END;
    if (strcmp(word,"UPARROW")==0||strcmp(word,"UP")==0)     return KEY_UP;
    if (strcmp(word,"DOWNARROW")==0||strcmp(word,"DOWN")==0) return KEY_DOWN;
    if (strcmp(word,"LEFTARROW")==0||strcmp(word,"LEFT")==0) return KEY_LEFT;
    if (strcmp(word,"RIGHTARROW")==0||strcmp(word,"RIGHT")==0) return KEY_RIGHT;
    if (strcmp(word,"SPACE")==0)     return KEY_SPACE;
    if (strcmp(word,"CAPSLOCK")==0)  return KEY_CAPS;
    if (strcmp(word,"NUMLOCK")==0)   return KEY_NUMLK;
    if (strcmp(word,"SCROLLLOCK")==0)return KEY_SCRLK;
    if (strcmp(word,"PRINTSCREEN")==0)return KEY_PRTSC;
    if (strcmp(word,"PAUSE")==0||strcmp(word,"BREAK")==0) return KEY_PAUSE;
    if (strncmp(word,"F",1)==0) {
        int n = atoi(word+1);
        if (n>=1&&n<=12) return (uint8_t)(KEY_F1 + n - 1);
    }
    // Single char shortcut (for combos like GUI r)
    if (strlen(word)==1) {
        HIDKey k = bdAsciiToHID(tolower(word[0]));
        return k.code;
    }
    return KEY_NONE;
}

static uint8_t bdModifierForKeyword(const char* word) {
    if (strcmp(word,"CTRL")==0||strcmp(word,"CONTROL")==0) return MOD_LCTRL;
    if (strcmp(word,"SHIFT")==0) return MOD_LSHIFT;
    if (strcmp(word,"ALT")==0)   return MOD_LALT;
    if (strcmp(word,"GUI")==0||strcmp(word,"WINDOWS")==0||strcmp(word,"COMMAND")==0) return MOD_LGUI;
    return MOD_NONE;
}

// ─────────────────────────────────────────────
//  PAYLOAD EXECUTOR
// ─────────────────────────────────────────────
static volatile bool bdAbort = false;

static void bdExecutePayload(const char* path,
                              void (*statusCb)(const char*, int, int)) {
    FsFile f = sd.open(path, O_RDONLY);
    if (!f) { statusCb("Cannot open payload file", 0, 0); return; }

    uint32_t totalLines = 0;
    // Count lines for progress
    {
        char tmp[256];
        while (f.fgets(tmp, sizeof(tmp))) totalLines++;
        f.rewindDirectory(); // reset won't work on file — reopen
        f.seek(0);
    }

    int    defaultDelay = 0;
    int    lineNum      = 0;
    char   lastCmd[256] = "";
    char   line[256];

    while (f.fgets(line, sizeof(line)) && !bdAbort) {
        lineNum++;
        // Strip trailing newline/CR
        int len = strlen(line);
        while (len > 0 && (line[len-1]=='\n'||line[len-1]=='\r'||line[len-1]==' '))
            line[--len] = '\0';
        if (len == 0) continue;

        statusCb(line, lineNum, totalLines);

        // Parse command
        char* cmd = line;
        char* arg = nullptr;
        char* sp  = strchr(line, ' ');
        if (sp) { *sp = '\0'; arg = sp + 1; }

        // Commands
        if (strcmp(cmd,"REM")==0) {
            // Comment — nothing
        } else if (strcmp(cmd,"DELAY")==0) {
            int ms = arg ? atoi(arg) : 500;
            unsigned long t0 = millis();
            while (millis()-t0 < (unsigned long)ms && !bdAbort) { delay(10); yield(); }
        } else if (strcmp(cmd,"DEFAULT_DELAY")==0||strcmp(cmd,"DEFAULTDELAY")==0) {
            defaultDelay = arg ? atoi(arg) : 0;
        } else if (strcmp(cmd,"STRING")==0) {
            if (arg) bdTypeString(arg);
            strncpy(lastCmd, line, 255); if(arg) *(lastCmd+(arg-line)-1)=' ';
        } else if (strcmp(cmd,"STRINGLN")==0) {
            if (arg) { bdTypeString(arg); bdSendReport(MOD_NONE, KEY_ENTER); }
        } else if (strcmp(cmd,"REPEAT")==0) {
            int n = arg ? atoi(arg) : 1;
            for (int i = 0; i < n && !bdAbort; i++) {
                // Re-execute last command (simplified — re-parse lastCmd)
                char repBuf[256]; strncpy(repBuf, lastCmd, 255);
                char* rcmd = repBuf;
                char* rarg = nullptr;
                char* rsp  = strchr(repBuf, ' ');
                if (rsp) { *rsp = '\0'; rarg = rsp + 1; }
                if (strcmp(rcmd,"STRING")==0||strcmp(rcmd,"STRINGLN")==0) {
                    if (rarg) { bdTypeString(rarg);
                        if (strcmp(rcmd,"STRINGLN")==0) bdSendReport(MOD_NONE,KEY_ENTER); }
                }
                delay(defaultDelay > 0 ? defaultDelay : 50);
            }
            continue;  // Don't update lastCmd for REPEAT
        } else if (strcmp(cmd,"NAME")==0) {
            // Handled before connect — skip at runtime
        } else if (strcmp(cmd,"WAIT_FOR_CONNECT")==0) {
            unsigned long t0 = millis();
            while (!bdConnected && millis()-t0 < 30000 && !bdAbort) { delay(100); yield(); }
        } else {
            // Modifier + optional key command (CTRL, ALT, GUI, SHIFT, ENTER, etc.)
            uint8_t mod  = bdModifierForKeyword(cmd);
            uint8_t key  = KEY_NONE;

            if (mod != MOD_NONE) {
                // Has modifier — arg is the key
                if (arg) key = bdKeywordCode(arg);
                else     key = KEY_NONE;
            } else {
                // Standalone key
                key = bdKeywordCode(cmd);
            }

            if (mod != MOD_NONE || key != KEY_NONE) {
                bdSendReport(mod, key);
            }
        }

        strncpy(lastCmd, line, 255);
        if (arg) *(lastCmd + (arg - line) - 1) = ' ';  // restore space

        if (defaultDelay > 0) delay(defaultDelay);
        yield();
    }
    f.close();
}

// ─────────────────────────────────────────────
//  BLE SERVER INIT — Raw GATT (version-independent)
//  Manually builds the HID over GATT profile without
//  relying on NimBLEHIDDevice wrapper API versioning.
// ─────────────────────────────────────────────
static void bdStartServer(const char* deviceName) {
    NimBLEDevice::init(deviceName);
    NimBLEDevice::setSecurityAuth(false, false, true);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    bdServer = NimBLEDevice::createServer();
    bdServer->setCallbacks(&bdServerCB);

    // ── Device Information Service ──
    NimBLEService* devInfoSvc = bdServer->createService(DEVICE_INFO_UUID);
    NimBLECharacteristic* pnpChar = devInfoSvc->createCharacteristic(
        PNP_ID_UUID, NIMBLE_PROPERTY::READ);
    // PnP ID: sig=0x02 (USB), vid=0x045E (Microsoft), pid=0x0800, version=0x0100
    uint8_t pnpData[] = {0x02, 0x5E, 0x04, 0x00, 0x08, 0x00, 0x01};
    pnpChar->setValue(pnpData, sizeof(pnpData));
    devInfoSvc->start();

    // ── Battery Service ──
    NimBLEService* batSvc = bdServer->createService(BATTERY_SERVICE_UUID);
    NimBLECharacteristic* batLevel = batSvc->createCharacteristic(
        BATTERY_LEVEL_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    uint8_t bat = 100;
    batLevel->setValue(&bat, 1);
    batSvc->start();

    // ── HID Service ──
    NimBLEService* hidSvc = bdServer->createService(HID_SERVICE_UUID);

    // HID Information: bcdHID=1.11, bCountryCode=0, flags=0x02 (normally connectable)
    NimBLECharacteristic* hidInfo = hidSvc->createCharacteristic(
        HID_INFO_UUID, NIMBLE_PROPERTY::READ);
    uint8_t hidInfoData[] = {0x11, 0x01, 0x00, 0x02};
    hidInfo->setValue(hidInfoData, sizeof(hidInfoData));

    // Report Map
    NimBLECharacteristic* reportMap = hidSvc->createCharacteristic(
        HID_REPORT_MAP_UUID, NIMBLE_PROPERTY::READ);
    reportMap->setValue((uint8_t*)BD_HID_DESCRIPTOR, sizeof(BD_HID_DESCRIPTOR));

    // HID Control Point (required by spec, host writes suspend/exit-suspend)
    NimBLECharacteristic* ctrlPoint = hidSvc->createCharacteristic(
        HID_CONTROL_POINT_UUID, NIMBLE_PROPERTY::WRITE_NR);

    // Input Report (keyboard) — notify only
    bdInput = hidSvc->createCharacteristic(
        HID_REPORT_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    // Report Reference descriptor: report ID=1, type=1 (input)
    NimBLEDescriptor* reportRef = bdInput->createDescriptor(
        "2908", NIMBLE_PROPERTY::READ, 2);
    uint8_t reportRefData[] = {0x01, 0x01};
    reportRef->setValue(reportRefData, sizeof(reportRefData));

    hidSvc->start();

    // ── Advertise as HID keyboard ──
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(0x03C1);  // Keyboard
    adv->addServiceUUID(HID_SERVICE_UUID);
    adv->setScanResponse(false);
    adv->start();
    Serial.printf("[BLE DUCKY] Advertising as: %s\n", deviceName);
}

// ─────────────────────────────────────────────
//  PAYLOAD FILE LIST
// ─────────────────────────────────────────────
#define MAX_PAYLOADS 32
static char bdPayloads[MAX_PAYLOADS][48];
static int  bdPayloadCount = 0;

static void bdScanPayloads() {
    bdPayloadCount = 0;
    if (!sd.exists(PAYLOAD_DIR)) {
        sd.mkdir(PAYLOAD_DIR);
        // Write a sample payload
        FsFile demo = sd.open("/payloads/hello_world.txt", O_WRITE|O_CREAT|O_TRUNC);
        if (demo) {
            demo.println("REM Demo payload — Hello World");
            demo.println("DELAY 1000");
            demo.println("GUI r");
            demo.println("DELAY 500");
            demo.println("STRING notepad");
            demo.println("ENTER");
            demo.println("DELAY 1000");
            demo.println("STRING Hello from Pisces Moon OS!");
            demo.println("ENTER");
            demo.close();
        }
    }
    FsFile dir = sd.open(PAYLOAD_DIR);
    if (!dir) return;
    FsFile f;
    while (f.openNext(&dir, O_RDONLY) && bdPayloadCount < MAX_PAYLOADS) {
        char nm[48]; f.getName(nm, sizeof(nm));
        if (strstr(nm, ".txt") || strstr(nm, ".ds")) {
            snprintf(bdPayloads[bdPayloadCount++], 48, "%s/%s", PAYLOAD_DIR, nm);
        }
        f.close();
    }
    dir.close();
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void bdDrawHeader(bool connected) {
    gfx->fillRect(0, 0, 320, 24, BD_HDR);
    gfx->drawFastHLine(0, 23, 320, BD_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(BD_RED);
    gfx->setCursor(6, 4);
    gfx->print("BLE DUCKY");
    gfx->setTextColor(connected ? BD_GREEN : BD_ORANGE);
    gfx->setCursor(100, 4);
    gfx->print(connected ? "CONNECTED" : "WAITING...");
    gfx->setTextColor(BD_DIM);
    gfx->setCursor(244, 4);
    gfx->print("[Q=EXIT]");
}

static void bdDrawPayloadList(int scroll, int selected) {
    gfx->fillRect(0, 26, 320, 172, BD_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(BD_DIM);
    gfx->setCursor(4, 28);
    gfx->printf("PAYLOADS (%d)  — /payloads/", bdPayloadCount);
    gfx->drawFastHLine(0, 37, 320, 0x2000);

    if (bdPayloadCount == 0) {
        gfx->setTextColor(BD_DIM);
        gfx->setCursor(10, 80); gfx->print("No payloads found in /payloads/");
        gfx->setCursor(10, 96); gfx->print("Add .txt DuckyScript files to SD card.");
        return;
    }

    int show = min(7, bdPayloadCount);
    for (int i = scroll; i < scroll + show && i < bdPayloadCount; i++) {
        int ry  = 40 + (i - scroll) * 20;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, 19, sel ? BD_SEL : (i%2==0 ? 0x0821 : BD_BG));
        const char* slash = strrchr(bdPayloads[i], '/');
        const char* fname = slash ? slash + 1 : bdPayloads[i];
        gfx->setTextColor(sel ? BD_ORANGE : 0xC618);
        gfx->setCursor(8, ry + 5);
        char nb[38]; strncpy(nb, fname, 37); nb[37] = '\0';
        gfx->print(nb);
        if (sel) {
            gfx->setTextColor(BD_DIM);
            gfx->setCursor(252, ry + 5);
            gfx->print("[CLICK]");
        }
    }
}

static int bdStatusLine = 200;

static void bdDrawStatus(const char* msg, uint16_t col = BD_DIM) {
    gfx->fillRect(0, 210, 320, 30, 0x1000);
    gfx->drawFastHLine(0, 210, 320, BD_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(col);
    gfx->setCursor(4, 220);
    gfx->print(msg);
}

static void bdProgressStatus(const char* lineText, int lineNum, int total) {
    // Called during execution — update the status bar
    gfx->fillRect(0, 198, 320, 42, 0x1000);
    gfx->drawFastHLine(0, 198, 320, BD_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(BD_DIM);
    gfx->setCursor(4, 202);
    gfx->printf("Line %d/%d", lineNum, total);
    // Progress bar
    if (total > 0) {
        int bw = (298 * lineNum) / total;
        gfx->fillRect(4, 213, 298, 6, 0x0821);
        gfx->fillRect(4, 213, bw,  6, BD_RED);
    }
    // Current line
    gfx->setTextColor(BD_ORANGE);
    gfx->setCursor(4, 222);
    char buf[44]; strncpy(buf, lineText, 43); buf[43]='\0';
    gfx->print(buf);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_ble_ducky() {
    gfx->fillScreen(BD_BG);
    gfx->setTextColor(BD_RED);
    gfx->setTextSize(2);
    gfx->setCursor(10, 50); gfx->print("BLE DUCKY");
    gfx->setTextSize(1);
    gfx->setTextColor(BD_WHITE);
    gfx->setCursor(10, 80); gfx->print("Wireless HID keyboard injection.");
    gfx->setTextColor(BD_RED);
    gfx->setCursor(10, 96); gfx->print("USE ON AUTHORIZED SYSTEMS ONLY.");
    gfx->setTextColor(BD_DIM);
    gfx->setCursor(10, 114); gfx->print("Scanning payloads...");
    delay(1500);

    bdScanPayloads();

    // Hand off BLE from wardrive
    wardrive_ble_stop();
    delay(200);

    // Check first payload for NAME command
    char bleName[32] = "PM-Keyboard";
    if (bdPayloadCount > 0) {
        FsFile pf = sd.open(bdPayloads[0], O_RDONLY);
        if (pf) {
            char firstLine[128];
            if (pf.fgets(firstLine, sizeof(firstLine))) {
                if (strncmp(firstLine, "NAME ", 5) == 0) {
                    int nl = strlen(firstLine);
                    while (nl > 0 && (firstLine[nl-1]=='\n'||firstLine[nl-1]=='\r')) firstLine[--nl]='\0';
                    strncpy(bleName, firstLine + 5, 31); bleName[31] = '\0';
                }
            }
            pf.close();
        }
    }

    // Start BLE HID server
    bdConnected    = false;
    bdWasConnected = false;
    bdAbort        = false;
    bdStartServer(bleName);

    int scroll = 0, selected = 0;

    gfx->fillScreen(BD_BG);
    bdDrawHeader(false);
    bdDrawPayloadList(scroll, selected);
    char waitMsg[48];
    snprintf(waitMsg, sizeof(waitMsg), "Pair '%s' on target — then select payload", bleName);
    bdDrawStatus(waitMsg, BD_ORANGE);

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) { while(get_touch(&tx,&ty)){delay(10);} break; }
        if (k == 'q' || k == 'Q') break;

        // Update connection indicator
        static bool lastConn = false;
        if (bdConnected != lastConn) {
            lastConn = bdConnected;
            bdDrawHeader(bdConnected);
            if (bdConnected) bdDrawStatus("Connected! Select payload and press CLICK to inject.", BD_GREEN);
            else bdDrawStatus(waitMsg, BD_ORANGE);
        }

        // Scroll
        if (tb.y == -1 && selected > 0)               { selected--; if(selected<scroll) scroll--; }
        if (tb.y ==  1 && selected < bdPayloadCount-1) { selected++; if(selected>=scroll+7) scroll++; }
        bdDrawPayloadList(scroll, selected);

        // Execute selected payload
        if (tb.clicked && bdPayloadCount > 0) {
            if (!bdConnected) {
                bdDrawStatus("Not connected — pair with target first", BD_RED);
            } else {
                bdAbort = false;
                gfx->fillRect(0, 24, 320, 174, BD_BG);
                gfx->setTextColor(BD_ORANGE); gfx->setTextSize(1);
                gfx->setCursor(6, 30);
                const char* slash = strrchr(bdPayloads[selected], '/');
                gfx->printf("INJECTING: %s", slash ? slash+1 : bdPayloads[selected]);
                gfx->setTextColor(BD_DIM);
                gfx->setCursor(6, 44); gfx->print("Q or tap header to abort.");

                bdExecutePayload(bdPayloads[selected], [](const char* line, int n, int tot){
                    bdProgressStatus(line, n, tot);
                    // Check for abort keypress during execution
                    char k = get_keypress();
                    if (k == 'q' || k == 'Q') bdAbort = true;
                    int16_t tx2, ty2;
                    if (get_touch(&tx2, &ty2) && ty2 < 24) bdAbort = true;
                });

                if (bdAbort) {
                    bdDrawStatus("Payload ABORTED.", BD_RED);
                } else {
                    bdDrawStatus("Payload complete.", BD_GREEN);
                }
                delay(1500);

                // Restore payload list
                gfx->fillRect(0, 24, 320, 174, BD_BG);
                bdDrawPayloadList(scroll, selected);
            }
        }

        delay(20); yield();
    }

    // Cleanup
    NimBLEDevice::stopAdvertising();
    if (bdServer) { bdServer->disconnect(0); }
    NimBLEDevice::deinit(true);
    bdServer = nullptr; bdInput = nullptr;
    wardrive_ble_resume();
    gfx->fillScreen(BD_BG);
}
