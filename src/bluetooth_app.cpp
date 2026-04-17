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
 * PISCES MOON OS — BT RADAR
 * BLE scanner using NimBLE (correct stack for ESP32-S3 BLE-only hardware).
 * Classic Bluetooth does not exist on ESP32-S3 silicon.
 *
 * Controls:
 *   Trackball UP/DOWN = scroll device list
 *   Header tap        = exit
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Arduino_GFX_Library.h>
#include "bluetooth_app.h"
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "gamepad.h"

extern Arduino_GFX *gfx;
extern bool exitApp;

#define MAX_DEVICES 20
#define ROW_H       16
#define LIST_Y      28
#define MAX_ROWS    11

struct BLEDev {
    char mac[18];
    char name[24];
    int  rssi;
};

static BLEDev devices[MAX_DEVICES];
static int    devCount   = 0;
static int    listOffset = 0;
static int    totalSeen  = 0;

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        const char* mac = dev->getAddress().toString().c_str();
        for (int i = 0; i < devCount; i++) {
            if (strcmp(devices[i].mac, mac) == 0) {
                devices[i].rssi = dev->getRSSI();
                return;
            }
        }
        totalSeen++;
        if (devCount < MAX_DEVICES) {
            devCount++;
        } else {
            memmove(&devices[0], &devices[1], sizeof(BLEDev) * (MAX_DEVICES - 1));
        }
        BLEDev& d = devices[devCount - 1];
        strncpy(d.mac, mac, 17); d.mac[17] = '\0';
        d.rssi = dev->getRSSI();
        if (dev->haveName()) {
            strncpy(d.name, dev->getName().c_str(), 23);
            d.name[23] = '\0';
        } else {
            strncpy(d.name, "Unknown", sizeof(d.name));
        }
    }
};

static NimBLEScan*    pScan      = nullptr;
static ScanCallbacks* pCallbacks = nullptr;

static void drawHeader() {
    gfx->fillRect(0, 0, 320, 26, C_DARK);
    gfx->drawFastHLine(0, 25, 320, C_GREEN);
    gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN); gfx->setTextSize(1);
    gfx->print("BLE RADAR | HDR:EXIT | BALL:SCROLL");
}

static void drawList() {
    gfx->fillRect(0, LIST_Y, 320, 240 - LIST_Y - 16, C_BLACK);
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
        gfx->setCursor(155, rowY + 3); gfx->setTextColor(C_WHITE);
        gfx->print(d.name);
    }
}

static void drawFooter() {
    gfx->fillRect(0, 224, 320, 16, C_DARK);
    gfx->setCursor(10, 226); gfx->setTextColor(0xFFE0); gfx->setTextSize(1);
    gfx->printf("Unique: %d  Total: %d", devCount, totalSeen);
}

void runBluetoothApp() {
    devCount = 0; listOffset = 0; totalSeen = 0; exitApp = false;
    gfx->fillScreen(C_BLACK);
    drawHeader(); drawFooter();

    NimBLEDevice::getScan()->stop(); // Stop any active gamepad scan first
    pScan      = NimBLEDevice::getScan();
    pCallbacks = new ScanCallbacks();
    pScan->setAdvertisedDeviceCallbacks(pCallbacks, false);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->start(0, false);  // 0 = continuous non-blocking scan

    unsigned long lastDraw = 0, lastRescan = 0;

    while (!exitApp) {
        if (millis() - lastRescan > 5000) {
            if (!pScan->isScanning()) { pScan->clearResults(); pScan->start(0, false); }
            lastRescan = millis();
        }
        if (millis() - lastDraw > 1000) {
            drawList(); drawFooter(); lastDraw = millis();
        }
        TrackballState tb = update_trackball();
        if (tb.y == -1 && listOffset > 0)                          { listOffset--; drawList(); }
        else if (tb.y == 1 && listOffset < devCount - MAX_ROWS)    { listOffset++; drawList(); }

        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 26) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            exitApp = true;
        }
        delay(50); yield();
    }

    pScan->stop();
    // Do NOT deinit NimBLE — wardrive.cpp owns the stack.
    // Restore gamepad scan callbacks so auto-reconnect resumes.
    gamepad_init();
    delete pCallbacks; pCallbacks = nullptr;
}
