// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "bluetooth_app.h"
#include "keyboard.h"
#include "pm_input.h"
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "wardrive.h"

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
extern bool exitApp;

#define MAX_DEVICES 20
#define ROW_H       16
#define LIST_Y      28
#define FOOTER_H    16
static constexpr int LIST_BOTTOM = DISP_H - FOOTER_H;
static constexpr int MAX_ROWS    = (LIST_BOTTOM - LIST_Y) / ROW_H;

struct BLEDev { char mac[18]; char name[24]; int rssi; };

static BLEDev devices[MAX_DEVICES];
static int    devCount   = 0;
static int    listOffset = 0;
static int    totalSeen  = 0;

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        std::string macStr = dev->getAddress().toString();
        const char* mac = macStr.c_str();
        for (int i = 0; i < devCount; i++) {
            if (strcmp(devices[i].mac, mac) == 0) {
                devices[i].rssi = dev->getRSSI();
                return;
            }
        }
        totalSeen++;
        if (devCount < MAX_DEVICES) devCount++;
        else memmove(&devices[0], &devices[1], sizeof(BLEDev) * (MAX_DEVICES - 1));
        BLEDev& d = devices[devCount - 1];
        strncpy(d.mac, mac, 17); d.mac[17] = '\0';
        d.rssi = dev->getRSSI();
        if (dev->haveName()) {
            std::string nameStr = dev->getName();
            strncpy(d.name, nameStr.c_str(), 23);
            d.name[23] = '\0';
        } else strncpy(d.name, "Unknown", sizeof(d.name));
    }
};

static NimBLEScan*    pScan      = nullptr;
static ScanCallbacks* pCallbacks = nullptr;

static void drawHeader() {
    gfx->fillRect(0, 0, DISP_W, 26, C_DARK);
    gfx->drawFastHLine(0, 25, DISP_W, C_GREEN);
    gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
#ifdef DEVICE_TLORAPAGER
    gfx->print("BLE RADAR | Q EXIT | BALL SCROLL");
#else
    gfx->print("BLE RADAR | Q EXIT | HDR EXIT | BALL SCROLL");
#endif
}

static void drawList() {
    gfx->fillRect(0, LIST_Y, DISP_W, LIST_BOTTOM - LIST_Y, C_BLACK);
    if (devCount == 0) {
        gfx->setCursor(10, LIST_Y + 20);
        gfx->setTextColor(C_GREY);
        gfx->print("Scanning... no devices yet.");
        return;
    }
    int maxOffset = max(0, devCount - MAX_ROWS);
    if (listOffset > maxOffset) listOffset = maxOffset;
    for (int i = 0; i < MAX_ROWS; i++) {
        int idx = listOffset + i;
        if (idx >= devCount) break;
        BLEDev& d = devices[idx];
        int rowY = LIST_Y + (i * ROW_H);
        uint16_t rssiColor = (d.rssi > -60) ? C_GREEN : (d.rssi > -80) ? 0xFFE0 : C_RED;
        gfx->setCursor(5, rowY + 3); gfx->setTextColor(rssiColor);
        gfx->printf("%4d", d.rssi);
        gfx->setCursor(38, rowY + 3); gfx->setTextColor(C_GREY);
        gfx->print(d.mac);
        // Name column starts after MAC; we have (DISP_W - 155) px available
        gfx->setCursor(155, rowY + 3); gfx->setTextColor(C_WHITE);
        int nameMax = (DISP_W - 160) / 6;
        char nbuf[32];
        strncpy(nbuf, d.name, sizeof(nbuf) - 1); nbuf[sizeof(nbuf)-1] = 0;
        if ((int)strlen(nbuf) > nameMax) nbuf[nameMax] = 0;
        gfx->print(nbuf);
    }
}

static void drawFooter() {
    gfx->fillRect(0, LIST_BOTTOM, DISP_W, FOOTER_H, C_DARK);
    gfx->setCursor(10, LIST_BOTTOM + 2); gfx->setTextColor(0xFFE0); gfx->setTextSize(1);
    gfx->printf("Unique: %d  Total: %d", devCount, totalSeen);
}

static void startScanWindow() {
    if (!pScan) return;
    if (pScan->isScanning()) pScan->stop();
    pScan->clearResults();
    pScan->start(5, false);
}

void runBluetoothApp() {
    devCount = 0; listOffset = 0; totalSeen = 0; exitApp = false;
    gfx->fillScreen(C_BLACK);
    drawHeader(); drawFooter();

    bool resumeWardrive = wardrive_active;
    wardrive_ble_stop();
    if (!NimBLEDevice::getInitialized()) {
        NimBLEDevice::init("");
    }

    pScan      = NimBLEDevice::getScan();
    pScan->stop();
    pCallbacks = new ScanCallbacks();
    pScan->setAdvertisedDeviceCallbacks(pCallbacks, false);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    startScanWindow();

    unsigned long lastDraw = 0, lastRescan = 0;
    while (!exitApp) {
        if (millis() - lastRescan > 5000) {
            if (!pScan->isScanning()) startScanWindow();
            lastRescan = millis();
        }
        if (millis() - lastDraw > 1000) {
            drawList(); drawFooter(); lastDraw = millis();
        }
        if (pm_is_exit_key(get_keypress())) {
            exitApp = true;
            break;
        }
        TrackballState tb = update_trackball();
        if (tb.y == -1 && listOffset > 0)                       { listOffset--; drawList(); }
        else if (tb.y == 1 && listOffset < devCount - MAX_ROWS) { listOffset++; drawList(); }

        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 40) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            exitApp = true;
        }
        delay(20); yield();
    }

    if (pScan) {
        pScan->stop();
        pScan->setAdvertisedDeviceCallbacks(nullptr, false);
    }
    delete pCallbacks; pCallbacks = nullptr;
    wardrive_ble_resume();
    if (!resumeWardrive) wardrive_active = false;
}
