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
 * PISCES MOON OS — BLE GATT EXPLORER v1.0
 * Scans for BLE devices, selects one, connects via NimBLE GATT client,
 * and enumerates all services and characteristics.
 *
 * For each characteristic displays:
 *   UUID, properties (READ/WRITE/NOTIFY/INDICATE), and the current value
 *   if the property includes READ (up to 20 bytes shown as hex + ASCII).
 *
 * Session log saved to /cyber_logs/gatt_NNNNNNNNNN.json
 *
 * NimBLE coexistence:
 *   wardrive_ble_stop()   — called before scan (hands off NimBLE singleton)
 *   wardrive_ble_resume() — called on exit (wardrive resumes BLE scanning)
 *
 * USE ONLY ON DEVICES YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 *
 * Controls:
 *   Trackball UP/DOWN = scroll device list / characteristic list
 *   Trackball CLICK   = select device / connect
 *   Q / tap header    = exit / back
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <NimBLEDevice.h>
#include <NimBLEClient.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "wardrive.h"
#include "ble_gatt_explorer.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define GX_BG       0x0000
#define GX_HDR      0x1800   // Dark red header
#define GX_RED      0xF800
#define GX_ORANGE   0xFD20
#define GX_GREEN    0x07E0
#define GX_CYAN     0x07FF
#define GX_WHITE    0xFFFF
#define GX_DIM      0x4208
#define GX_SEL      0x2800   // Selected row highlight

#define LOG_DIR     "/cyber_logs"

// ─────────────────────────────────────────────
//  SCAN STATE
// ─────────────────────────────────────────────
#define MAX_SCAN_RESULTS 16

struct GXDevice {
    char    mac[18];
    char    name[32];
    int8_t  rssi;
    uint8_t addrType;
};

static GXDevice  gxDevices[MAX_SCAN_RESULTS];
static int       gxDevCount  = 0;
static volatile bool gxScanDone = false;

class GXScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (gxDevCount >= MAX_SCAN_RESULTS) return;
        const char* mac = dev->getAddress().toString().c_str();
        // Dedup by MAC
        for (int i = 0; i < gxDevCount; i++) {
            if (strcmp(gxDevices[i].mac, mac) == 0) {
                gxDevices[i].rssi = dev->getRSSI();
                return;
            }
        }
        GXDevice& d = gxDevices[gxDevCount++];
        strncpy(d.mac, mac, 17);            d.mac[17] = '\0';
        d.rssi     = dev->getRSSI();
        d.addrType = (uint8_t)dev->getAddress().getType();
        if (dev->haveName()) {
            strncpy(d.name, dev->getName().c_str(), 31);
            d.name[31] = '\0';
        } else {
            strcpy(d.name, "Unknown");
        }
    }
};

static GXScanCallbacks gxScanCB;

// ─────────────────────────────────────────────
//  DRAW HELPERS
// ─────────────────────────────────────────────
static void gxDrawHeader(const char* title, const char* sub = "") {
    gfx->fillRect(0, 0, 320, 24, GX_HDR);
    gfx->drawFastHLine(0, 23, 320, GX_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(GX_RED);
    gfx->setCursor(6, 4);
    gfx->print(title);
    if (sub[0]) {
        gfx->setTextColor(GX_DIM);
        gfx->setCursor(6, 14);
        gfx->print(sub);
    }
    gfx->setTextColor(GX_DIM);
    gfx->setCursor(270, 8);
    gfx->print("[Q=EXIT]");
}

static void gxStatus(const char* msg, uint16_t col = GX_DIM) {
    gfx->fillRect(0, 210, 320, 30, 0x0800);
    gfx->drawFastHLine(0, 210, 320, GX_RED);
    gfx->setTextSize(1);
    gfx->setTextColor(col);
    gfx->setCursor(6, 220);
    gfx->print(msg);
}

// ─────────────────────────────────────────────
//  DEVICE LIST SCREEN
// ─────────────────────────────────────────────
static int gxDevScroll = 0;
#define GX_DEV_ROWS 8
#define GX_ROW_H    22

static void gxDrawDeviceList(int selected) {
    gfx->fillRect(0, 26, 320, 182, GX_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(GX_DIM);
    gfx->setCursor(4, 28);
    gfx->printf("%-17s  %-16s  RSI", "MAC", "NAME");
    gfx->drawFastHLine(0, 37, 320, 0x2000);

    if (gxDevCount == 0) {
        gfx->setTextColor(GX_DIM);
        gfx->setCursor(10, 80);
        gfx->print("No devices found. Try again.");
        return;
    }

    int end = min(gxDevScroll + GX_DEV_ROWS, gxDevCount);
    for (int i = gxDevScroll; i < end; i++) {
        int ry  = 40 + (i - gxDevScroll) * GX_ROW_H;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, GX_ROW_H - 1, sel ? GX_SEL : (i%2==0 ? 0x0821 : GX_BG));
        gfx->setTextColor(sel ? GX_ORANGE : GX_CYAN);
        gfx->setCursor(4, ry + 4);
        gfx->print(gxDevices[i].mac);
        gfx->setTextColor(sel ? GX_WHITE : 0xC618);
        gfx->setCursor(120, ry + 4);
        char nameBuf[17]; strncpy(nameBuf, gxDevices[i].name, 16); nameBuf[16] = '\0';
        gfx->print(nameBuf);
        uint16_t rc = (gxDevices[i].rssi > -60) ? GX_GREEN :
                      (gxDevices[i].rssi > -80) ? GX_ORANGE : GX_RED;
        gfx->setTextColor(rc);
        gfx->setCursor(284, ry + 4);
        gfx->printf("%4d", gxDevices[i].rssi);
    }
}

// ─────────────────────────────────────────────
//  CHARACTERISTIC LIST SCREEN
// ─────────────────────────────────────────────
struct GXChar {
    char     svcUUID[40];
    char     charUUID[40];
    char     props[24];     // "R W N I"
    char     value[48];     // Hex + ASCII preview
};

// Characteristic result table — PSRAM allocated during enumeration, freed after log write
static GXChar* gxChars      = nullptr;
static int     gxCharCount  = 0;
static int     gxCharScroll = 0;
#define GX_CHAR_ROWS 7
#define GX_CHAR_H    24

static String gxPropsString(NimBLERemoteCharacteristic* ch) {
    String p = "";
    if (ch->canRead())     p += "R";
    if (ch->canWrite())    p += "W";
    if (ch->canNotify())   p += "N";
    if (ch->canIndicate()) p += "I";
    if (p.isEmpty())       p = "-";
    return p;
}

static String gxValueHex(NimBLERemoteCharacteristic* ch) {
    if (!ch->canRead()) return "(not readable)";
    std::string val;
    try { val = ch->readValue(); } catch (...) { return "(read error)"; }
    if (val.empty()) return "(empty)";
    String out = "";
    int lim = min((int)val.size(), 8);
    for (int i = 0; i < lim; i++) {
        char h[4]; sprintf(h, "%02X ", (uint8_t)val[i]);
        out += h;
    }
    out += " | ";
    for (int i = 0; i < lim; i++) {
        char c = val[i];
        out += (c >= 32 && c < 127) ? c : '.';
    }
    if ((int)val.size() > 8) out += "...";
    return out;
}

static void gxDrawCharList(int selected) {
    gfx->fillRect(0, 26, 320, 182, GX_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(GX_DIM);
    gfx->setCursor(4, 28);
    gfx->print("SVC UUID / CHAR UUID     PROP  VALUE");
    gfx->drawFastHLine(0, 37, 320, 0x2000);

    if (gxCharCount == 0) {
        gfx->setTextColor(GX_DIM);
        gfx->setCursor(10, 80);
        gfx->print("No characteristics found.");
        return;
    }

    int end = min(gxCharScroll + GX_CHAR_ROWS, gxCharCount);
    for (int i = gxCharScroll; i < end; i++) {
        int ry  = 40 + (i - gxCharScroll) * GX_CHAR_H;
        bool sel = (i == selected);
        gfx->fillRect(0, ry, 320, GX_CHAR_H - 1, sel ? GX_SEL : (i%2==0 ? 0x0821 : GX_BG));
        // SVC UUID (first 8 chars) + CHAR UUID
        gfx->setTextColor(GX_DIM);
        gfx->setCursor(4, ry + 2);
        char svcShort[10]; strncpy(svcShort, gxChars[i].svcUUID, 8); svcShort[8] = '\0';
        gfx->print(svcShort);
        gfx->setTextColor(sel ? GX_ORANGE : GX_CYAN);
        gfx->setCursor(4, ry + 13);
        char charShort[10]; strncpy(charShort, gxChars[i].charUUID, 8); charShort[8] = '\0';
        gfx->print(charShort);
        // Props
        gfx->setTextColor(sel ? GX_GREEN : GX_DIM);
        gfx->setCursor(70, ry + 7);
        gfx->print(gxChars[i].props);
        // Value
        gfx->setTextColor(sel ? GX_WHITE : 0x8C51);
        gfx->setCursor(100, ry + 7);
        gfx->print(gxChars[i].value);
    }
}

// ─────────────────────────────────────────────
//  SESSION LOG
// ─────────────────────────────────────────────
static void gxWriteLog(const char* targetMac, const char* targetName) {
    if (!sd.exists(LOG_DIR)) sd.mkdir(LOG_DIR);
    char fname[64];
    snprintf(fname, sizeof(fname), "%s/gatt_%010lu.json", LOG_DIR, millis());
    FsFile f = sd.open(fname, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) { Serial.println("[GATT] Log write failed"); return; }

    f.printf("{\"target\":\"%s\",\"name\":\"%s\",\"chars\":[\n", targetMac, targetName);
    for (int i = 0; i < gxCharCount; i++) {
        if (i > 0) f.print(",\n");
        // Escape value string for JSON
        String val = String(gxChars[i].value);
        val.replace("\"", "'");
        f.printf("  {\"svc\":\"%s\",\"char\":\"%s\",\"props\":\"%s\",\"value\":\"%s\"}",
            gxChars[i].svcUUID, gxChars[i].charUUID,
            gxChars[i].props, val.c_str());
    }
    f.print("\n]}\n");
    f.close();
    Serial.printf("[GATT] Log saved: %s\n", fname);
}

// ─────────────────────────────────────────────
//  CONNECT AND ENUMERATE
// ─────────────────────────────────────────────
static bool gxEnumerate(int deviceIdx) {
    gxCharCount  = 0;
    gxCharScroll = 0;

    // Allocate characteristic table in PSRAM — 64 × 152 bytes = ~9.7KB
    if (!gxChars) {
        gxChars = (GXChar*)ps_malloc(64 * sizeof(GXChar));
        if (!gxChars) {
            gfx->fillScreen(GX_BG);
            gxDrawHeader("BLE GATT EXPLORER", "Out of PSRAM");
            gxStatus("Cannot allocate result buffer. Tap to go back.", GX_RED);
            while (true) {
                if (get_keypress()) return false;
                int16_t tx, ty; if (get_touch(&tx, &ty)) return false;
                delay(50);
            }
        }
    }
    memset(gxChars, 0, 64 * sizeof(GXChar));

    GXDevice& dev = gxDevices[deviceIdx];

    gfx->fillScreen(GX_BG);
    gxDrawHeader("BLE GATT EXPLORER", "Connecting...");
    gfx->setTextColor(GX_ORANGE);
    gfx->setTextSize(1);
    gfx->setCursor(6, 40);
    gfx->print("Target:"); gfx->setTextColor(GX_WHITE);
    gfx->setCursor(50, 40); gfx->print(dev.mac);
    gfx->setCursor(6, 54);  gfx->setTextColor(GX_DIM); gfx->print(dev.name);

    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectionParams(16, 32, 0, 400);

    gfx->setTextColor(GX_DIM); gfx->setCursor(6, 74);
    gfx->print("Connecting...");

    NimBLEAddress addr(dev.mac, dev.addrType);
    if (!client->connect(addr)) {
        gfx->setTextColor(GX_RED);
        gfx->setCursor(6, 90); gfx->print("Connection FAILED.");
        gfx->setTextColor(GX_DIM);
        gfx->setCursor(6, 106); gfx->print("Device may be out of range or busy.");
        gfx->setCursor(6, 120); gfx->print("Some devices reject unauthorized connections.");
        NimBLEDevice::deleteClient(client);
        gxStatus("Connect failed — tap or Q to go back", GX_RED);
        while (true) {
            if (get_keypress()) return false;
            int16_t tx, ty; if (get_touch(&tx, &ty)) { while(get_touch(&tx,&ty)){delay(10);} return false; }
            delay(50);
        }
    }

    gfx->setTextColor(GX_GREEN); gfx->setCursor(6, 74); gfx->print("Connected!          ");
    gfx->setTextColor(GX_DIM);   gfx->setCursor(6, 90); gfx->print("Enumerating services...");

    // Walk all services
    std::vector<NimBLERemoteService*>* services = client->getServices(true);
    if (!services || services->empty()) {
        gfx->setTextColor(GX_RED); gfx->setCursor(6, 106); gfx->print("No services found.");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        gxStatus("No services — tap or Q to go back", GX_RED);
        while (true) {
            if (get_keypress()) return false;
            int16_t tx, ty; if (get_touch(&tx, &ty)) { while(get_touch(&tx,&ty)){delay(10);} return false; }
            delay(50);
        }
    }

    int svcNum = 0;
    for (auto svc : *services) {
        String svcUUID = svc->getUUID().toString().c_str();
        svcNum++;
        gfx->fillRect(6, 90, 314, 12, GX_BG);
        gfx->setTextColor(GX_DIM); gfx->setCursor(6, 90);
        gfx->printf("SVC %d: %s", svcNum, svcUUID.substring(0,16).c_str());

        std::vector<NimBLERemoteCharacteristic*>* chars = svc->getCharacteristics(true);
        if (!chars) continue;
        for (auto ch : *chars) {
            if (gxCharCount >= 64) break;
            GXChar& gc = gxChars[gxCharCount++];
            strncpy(gc.svcUUID,  svcUUID.c_str(),                    39); gc.svcUUID[39]  = '\0';
            strncpy(gc.charUUID, ch->getUUID().toString().c_str(),   39); gc.charUUID[39] = '\0';
            String props = gxPropsString(ch);
            strncpy(gc.props, props.c_str(), 23); gc.props[23] = '\0';
            String val = gxValueHex(ch);
            strncpy(gc.value, val.c_str(), 47); gc.value[47] = '\0';
            delay(5); yield();
        }
    }

    client->disconnect();
    NimBLEDevice::deleteClient(client);

    // Write log
    gxWriteLog(dev.mac, dev.name);

    return true;
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_ble_gatt_explorer() {
    gfx->fillScreen(GX_BG);
    gxDrawHeader("BLE GATT EXPLORER", "Scanning for devices...");
    gfx->setTextColor(GX_DIM); gfx->setTextSize(1);
    gfx->setCursor(6, 50); gfx->print("Handing off BLE from wardrive...");

    // Stop wardrive BLE so we get clean scan access
    wardrive_ble_stop();
    delay(200);

    gxDevCount = 0;
    gxScanDone = false;

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&gxScanCB, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(5, false);  // 5 second scan

    gfx->fillRect(6, 50, 314, 12, GX_BG);
    gfx->setTextColor(GX_GREEN); gfx->setCursor(6, 50);
    gfx->printf("Scan complete. %d devices found.", gxDevCount);

    int selected = 0;
    gxDevScroll = 0;

    gfx->fillScreen(GX_BG);
    gxDrawHeader("BLE GATT EXPLORER", "Select device to inspect");
    gxDrawDeviceList(selected);
    gxStatus("CLICK=connect  BALL=scroll  Q=exit", GX_DIM);

    // Device selection loop
    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) delay(10);
            break;
        }
        if (k == 'q' || k == 'Q') break;

        // Rescan
        if (k == 'r' || k == 'R') {
            gxDevCount = 0;
            gfx->fillRect(0, 26, 320, 182, GX_BG);
            gfx->setTextColor(GX_ORANGE); gfx->setCursor(6, 80);
            gfx->print("Rescanning (5s)...");
            scan->clearResults();
            scan->start(5, false);
            selected = 0; gxDevScroll = 0;
            gfx->fillScreen(GX_BG);
            gxDrawHeader("BLE GATT EXPLORER", "Select device to inspect");
            gxDrawDeviceList(selected);
            gxStatus("CLICK=connect  BALL=scroll  R=rescan  Q=exit", GX_DIM);
            continue;
        }

        // Navigation
        bool nav = false;
        if (tb.y == -1 && selected > 0) {
            selected--;
            if (selected < gxDevScroll) gxDevScroll--;
            nav = true;
        }
        if (tb.y == 1 && selected < gxDevCount - 1) {
            selected++;
            if (selected >= gxDevScroll + GX_DEV_ROWS) gxDevScroll++;
            nav = true;
        }
        if (nav) { gxDrawDeviceList(selected); continue; }

        // Connect
        if ((tb.clicked || k == 13) && gxDevCount > 0) {
            bool ok = gxEnumerate(selected);
            if (!ok) {
                // Back to device list
                gfx->fillScreen(GX_BG);
                gxDrawHeader("BLE GATT EXPLORER", "Select device to inspect");
                gxDrawDeviceList(selected);
                gxStatus("CLICK=connect  BALL=scroll  R=rescan  Q=exit", GX_DIM);
                continue;
            }

            // Characteristic browse loop
            int charSel = 0;
            gxCharScroll = 0;
            gfx->fillScreen(GX_BG);
            char subTitle[48];
            snprintf(subTitle, sizeof(subTitle), "%s  %d chars", gxDevices[selected].mac, gxCharCount);
            gxDrawHeader("GATT RESULTS", subTitle);
            gxDrawCharList(charSel);
            gxStatus("Logged to /cyber_logs/  Q=back", GX_GREEN);

            while (true) {
                char k2 = get_keypress();
                TrackballState tb2 = update_trackball();
                int16_t tx2, ty2;

                if (get_touch(&tx2, &ty2) && ty2 < 24) { while(get_touch(&tx2,&ty2)){delay(10);} break; }
                if (k2 == 'q' || k2 == 'Q') break;

                bool nav2 = false;
                if (tb2.y == -1 && charSel > 0) {
                    charSel--;
                    if (charSel < gxCharScroll) gxCharScroll--;
                    nav2 = true;
                }
                if (tb2.y == 1 && charSel < gxCharCount - 1) {
                    charSel++;
                    if (charSel >= gxCharScroll + GX_CHAR_ROWS) gxCharScroll++;
                    nav2 = true;
                }
                if (nav2) gxDrawCharList(charSel);
                delay(20); yield();
            }

            // Back to device list
            gfx->fillScreen(GX_BG);
            gxDrawHeader("BLE GATT EXPLORER", "Select device to inspect");
            gxDrawDeviceList(selected);
            gxStatus("CLICK=connect  BALL=scroll  R=rescan  Q=exit", GX_DIM);
        }

        delay(20); yield();
    }

    // Resume wardrive BLE
    wardrive_ble_resume();
    free(gxChars); gxChars = nullptr;
    gfx->fillScreen(GX_BG);
}
