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

// ============================================================
//  gamepad.cpp — Pisces Moon OS 8BitDo Zero 2 BLE HID Driver
//  v1.1 — MAC Persistence + Active Pairing Flow
// ============================================================

#include <Arduino.h>
#include <FS.h>
#include "SdFat.h"
#include <NimBLEDevice.h>
#include <Arduino_GFX_Library.h>
#include "gamepad.h"
#include "theme.h"
#include "keyboard.h"
#include "trackball.h"
#include "touch.h"

extern Arduino_GFX* gfx;
extern SdFat        sd;

GamepadState g_gamepad = {};

static NimBLEClient*               _ble_client        = nullptr;
static NimBLERemoteCharacteristic* _hid_report        = nullptr;
static bool                        _notify_registered = false;

#define GP_CFG_PATH  "/gamepad.cfg"
static char _saved_mac[18] = {};

// ─── SD MAC persistence ──────────────────────────────────────
static void _save_mac(const char* mac) {
    FsFile f = sd.open(GP_CFG_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) { Serial.println("[GAMEPAD] Cannot write gamepad.cfg"); return; }
    f.print(mac);
    f.close();
    strncpy(_saved_mac, mac, 17); _saved_mac[17] = '\0';
    Serial.printf("[GAMEPAD] MAC saved: %s\n", _saved_mac);
}

static bool _load_mac() {
    if (!sd.exists(GP_CFG_PATH)) return false;
    FsFile f = sd.open(GP_CFG_PATH, O_READ);
    if (!f) return false;
    int len = f.read(_saved_mac, 17); f.close();
    _saved_mac[len] = '\0';
    if (len < 17 || _saved_mac[2] != ':') { _saved_mac[0] = '\0'; return false; }
    Serial.printf("[GAMEPAD] Loaded saved MAC: %s\n", _saved_mac);
    return true;
}

// ─── HID notify callback ─────────────────────────────────────
static void _gamepad_notify_cb(NimBLERemoteCharacteristic* pChar,
                                uint8_t* data, size_t length, bool isNotify) {
    if (length < 2) return;
    uint16_t nb = 0;
    if (data[0] & 0x01) nb |= GP_A;
    if (data[0] & 0x02) nb |= GP_B;
    if (data[0] & 0x04) nb |= GP_X;
    if (data[0] & 0x08) nb |= GP_Y;
    if (length > 1) {
        if (data[1] & 0x01) nb |= GP_L;
        if (data[1] & 0x02) nb |= GP_R;
        if (data[1] & 0x04) nb |= GP_SELECT;
        if (data[1] & 0x08) nb |= GP_START;
        if (data[1] & 0x10) nb |= GP_HOME;
    }
    if (length > 2) {
        uint8_t hat = data[2];
        if (hat==0||hat==1||hat==7) nb |= GP_UP;
        if (hat==2||hat==1||hat==3) nb |= GP_RIGHT;
        if (hat==4||hat==3||hat==5) nb |= GP_DOWN;
        if (hat==6||hat==5||hat==7) nb |= GP_LEFT;
    }
    if (length > 4) {
        g_gamepad.axis_lx = (int8_t)(data[3] - 0x7F);
        g_gamepad.axis_ly = (int8_t)(data[4] - 0x7F);
    }
    g_gamepad.pressed  = nb & ~g_gamepad.buttons;
    g_gamepad.released = g_gamepad.buttons & ~nb;
    g_gamepad.buttons  = nb;
    if (nb != 0) g_gamepad.last_input_ms = millis();
}

// ─── Client callbacks ────────────────────────────────────────
class _GamepadClientCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* c) override {
        g_gamepad.connected = true;
        Serial.printf("[GAMEPAD] Connected: %s\n", g_gamepad.device_name);
    }
    void onDisconnect(NimBLEClient* c) override {
        g_gamepad.connected = false;
        g_gamepad.buttons = g_gamepad.pressed = g_gamepad.released = 0;
        g_gamepad.axis_lx = g_gamepad.axis_ly = 0;
        _notify_registered = false; _hid_report = nullptr;
        Serial.println("[GAMEPAD] Disconnected");
    }
};
static _GamepadClientCB _gp_client_cb;

// ─── Subscribe to HID ────────────────────────────────────────
static bool _subscribe_hid(NimBLEClient* client) {
    NimBLERemoteService* svc = client->getService("1812");
    if (!svc) { client->disconnect(); return false; }
    _hid_report = svc->getCharacteristic("2a4d");
    if (!_hid_report || !_hid_report->canNotify()) { client->disconnect(); return false; }
    if (!_hid_report->subscribe(true, _gamepad_notify_cb)) { client->disconnect(); return false; }
    _notify_registered = true;
    Serial.println("[GAMEPAD] HID notifications active");
    return true;
}

// ─── Connect by MAC ──────────────────────────────────────────
static bool _connect_by_mac(const char* mac_str) {
    Serial.printf("[GAMEPAD] Connecting by MAC: %s\n", mac_str);
    NimBLEAddress addr(mac_str);
    if (!_ble_client) {
        _ble_client = NimBLEDevice::createClient();
        _ble_client->setClientCallbacks(&_gp_client_cb, false);
        _ble_client->setConnectionParams(12, 12, 0, 51);
    }
    if (!_ble_client->connect(addr)) { Serial.println("[GAMEPAD] MAC connect failed"); return false; }
    strncpy(g_gamepad.device_name, mac_str, sizeof(g_gamepad.device_name) - 1);
    g_gamepad.device_name[sizeof(g_gamepad.device_name)-1] = '\0';
    return _subscribe_hid(_ble_client);
}

// ─── Pairing scan callback ───────────────────────────────────
static char          _pair_found_mac[18]  = {};
static char          _pair_found_name[32] = {};
static volatile bool _pair_found          = false;

class _GamepadPairScanCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (_pair_found) return;
        const char* name = dev->getName().c_str();
        if (!strstr(name, "8BitDo") && !strstr(name, "Zero 2")) return;
        strncpy(_pair_found_mac,  dev->getAddress().toString().c_str(), 17); _pair_found_mac[17]='\0';
        strncpy(_pair_found_name, name, 31); _pair_found_name[31]='\0';
        _pair_found = true;
        NimBLEDevice::getScan()->stop();
        Serial.printf("[GAMEPAD] Pair found: %s  %s\n", _pair_found_mac, _pair_found_name);
    }
};
static _GamepadPairScanCB _gp_pair_scan_cb;

// ─── Auto-reconnect scan callback ────────────────────────────
class _GamepadAutoScanCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        const char* name = dev->getName().c_str();
        if (!strstr(name, "8BitDo") && !strstr(name, "Zero 2")) return;
        NimBLEDevice::getScan()->stop();
        if (!_ble_client) {
            _ble_client = NimBLEDevice::createClient();
            _ble_client->setClientCallbacks(&_gp_client_cb, false);
            _ble_client->setConnectionParams(12, 12, 0, 51);
        }
        if (!_ble_client->connect(dev)) return;
        strncpy(g_gamepad.device_name, name, sizeof(g_gamepad.device_name)-1);
        g_gamepad.device_name[sizeof(g_gamepad.device_name)-1] = '\0';
        if (_subscribe_hid(_ble_client))
            _save_mac(dev->getAddress().toString().c_str());
    }
};
static _GamepadAutoScanCB _gp_auto_scan_cb;

// ============================================================
//  gamepad_init()
// ============================================================
void gamepad_init() {
    Serial.println("[GAMEPAD] Init v1.1 — MAC persist + pairing flow");
    strncpy(g_gamepad.device_name, "none", sizeof(g_gamepad.device_name)-1);

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);
    scan->setInterval(200);
    scan->setWindow(150);

    if (_load_mac() && _saved_mac[0]) {
        if (_connect_by_mac(_saved_mac)) { Serial.println("[GAMEPAD] Reconnected by saved MAC"); return; }
        Serial.println("[GAMEPAD] Saved MAC failed — scan fallback");
    }

    scan->setAdvertisedDeviceCallbacks(&_gp_auto_scan_cb, false);
    scan->start(5, false);
    Serial.println("[GAMEPAD] BLE auto-scan started (5s)");
}

// ============================================================
//  gamepad_pair() — blocking UI, call from run_gamepad_setup()
// ============================================================
bool gamepad_pair() {
    // ── Instructions screen ──────────────────────────────────
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 23, 320, 0x8410);
    gfx->setTextSize(1); gfx->setTextColor(0x8410);
    gfx->setCursor(10, 8); gfx->print("GAMEPAD PAIRING");

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 36); gfx->print("On the 8BitDo Zero 2:");

    gfx->setTextColor(0xFFE0); gfx->setTextSize(2);
    gfx->setCursor(10, 54);  gfx->print("Hold SELECT + R");
    gfx->setCursor(10, 76);  gfx->print("for 3 seconds");

    gfx->setTextSize(1); gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 104); gfx->print("Until LED blinks PURPLE.");
    gfx->setTextColor(C_GREY);
    gfx->setCursor(10, 118); gfx->print("Blue=Classic BT (wrong)   Purple=BLE (correct)");

    gfx->drawFastHLine(10, 132, 300, C_DARK);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(10, 140); gfx->print("Scanning for controller...");

    gfx->fillRect(0, 210, 320, 30, C_DARK);
    gfx->drawFastHLine(0, 210, 320, 0x2104);
    gfx->setTextColor(C_GREY);
    gfx->setCursor(10, 220); gfx->print("Any key / tap header to cancel");

    // ── Start 30s pairing scan ───────────────────────────────
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan->isScanning()) { scan->stop(); delay(100); }

    _pair_found = false;
    _pair_found_mac[0] = _pair_found_name[0] = '\0';

    scan->setAdvertisedDeviceCallbacks(&_gp_pair_scan_cb, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(30, false);

    uint32_t start_ms    = millis();
    uint32_t last_dot_ms = 0;
    int      dot_count   = 0;

    while (millis() - start_ms < 30000) {
        if (_pair_found) break;

        if (millis() - last_dot_ms > 500) {
            last_dot_ms = millis();
            dot_count = (dot_count + 1) % 4;
            gfx->fillRect(10, 154, 240, 12, C_BLACK);
            gfx->setTextColor(0x07E0); gfx->setCursor(10, 154);
            String d = "Scanning"; for (int i=0;i<dot_count;i++) d+='.';
            gfx->print(d);
            uint32_t elapsed = millis() - start_ms;
            int bw = (int)(300 * (30000-elapsed) / 30000);
            gfx->fillRect(10, 188, 300, 8, 0x0821);
            gfx->fillRect(10, 188, bw,  8, 0x8410);
        }

        char k = get_keypress();
        if (k) { scan->stop(); return false; }
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            scan->stop(); return false;
        }
        TrackballState tb = update_trackball();
        if (tb.clicked || tb.x == -1) { scan->stop(); return false; }
        delay(50);
    }

    if (!_pair_found) {
        scan->stop();
        gfx->fillRect(10, 138, 300, 50, C_BLACK);
        gfx->setTextColor(C_RED);
        gfx->setCursor(10, 146); gfx->print("No controller found.");
        gfx->setTextColor(C_GREY);
        gfx->setCursor(10, 162); gfx->print("Ensure LED is PURPLE and try again.");
        delay(3000); return false;
    }

    // ── Connect ──────────────────────────────────────────────
    gfx->fillRect(10, 138, 300, 50, C_BLACK);
    gfx->setTextColor(0xFFE0); gfx->setCursor(10, 146);
    gfx->printf("Found: %.28s", _pair_found_name);
    gfx->setTextColor(C_GREY);  gfx->setCursor(10, 162); gfx->print("Connecting...");

    if (_ble_client && _ble_client->isConnected()) { _ble_client->disconnect(); delay(200); }
    if (!_ble_client) {
        _ble_client = NimBLEDevice::createClient();
        _ble_client->setClientCallbacks(&_gp_client_cb, false);
        _ble_client->setConnectionParams(12, 12, 0, 51);
    }

    NimBLEAddress addr(_pair_found_mac);
    if (!_ble_client->connect(addr)) {
        gfx->fillRect(10,162,300,16,C_BLACK); gfx->setTextColor(C_RED);
        gfx->setCursor(10,162); gfx->print("Connection failed — try again.");
        delay(3000); return false;
    }

    strncpy(g_gamepad.device_name, _pair_found_name, sizeof(g_gamepad.device_name)-1);
    g_gamepad.device_name[sizeof(g_gamepad.device_name)-1] = '\0';

    if (!_subscribe_hid(_ble_client)) {
        gfx->fillRect(10,162,300,16,C_BLACK); gfx->setTextColor(C_RED);
        gfx->setCursor(10,162); gfx->print("HID subscribe failed — try again.");
        delay(3000); return false;
    }

    _save_mac(_pair_found_mac);

    // ── Success screen ───────────────────────────────────────
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 23, 320, 0x07E0);
    gfx->setTextColor(0x07E0); gfx->setTextSize(1);
    gfx->setCursor(10, 8); gfx->print("GAMEPAD PAIRED");

    gfx->setTextSize(2); gfx->setTextColor(0x07E0);
    gfx->setCursor(60, 46); gfx->print("CONNECTED!");

    gfx->setTextSize(1); gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 80); gfx->printf("%.36s", _pair_found_name);
    gfx->setTextColor(C_GREY);
    gfx->setCursor(10, 96); gfx->print(_pair_found_mac);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 114); gfx->print("Saved to /gamepad.cfg");
    gfx->setCursor(10, 130); gfx->setTextColor(C_GREY);
    gfx->print("Auto-reconnects on next boot.");

    gfx->setTextColor(0x07E0);
    gfx->setCursor(10, 154); gfx->print("Press any button on controller to test.");

    uint32_t t0 = millis();
    while (millis() - t0 < 8000) {
        if (g_gamepad.buttons != 0) {
            gfx->fillRect(10, 168, 300, 14, C_BLACK);
            gfx->setTextColor(0x07E0);
            gfx->setCursor(10, 170); gfx->print("Input received!  Working.");
            delay(1500); break;
        }
        delay(50);
    }
    return true;
}

// ============================================================
//  gamepad_is_paired() / gamepad_forget()
// ============================================================
bool gamepad_is_paired() {
    return (_saved_mac[0] != '\0') || sd.exists(GP_CFG_PATH);
}

void gamepad_forget() {
    sd.remove(GP_CFG_PATH); _saved_mac[0] = '\0';
    if (_ble_client && _ble_client->isConnected()) _ble_client->disconnect();
    strncpy(g_gamepad.device_name, "none", sizeof(g_gamepad.device_name)-1);
    g_gamepad.connected = false;
    Serial.println("[GAMEPAD] Pairing forgotten");
}

// ============================================================
//  gamepad_poll()
// ============================================================
bool gamepad_poll() {
    if (g_gamepad.pressed & GP_HOME) {
        g_gamepad.buttons = g_gamepad.pressed = g_gamepad.released = 0;
        return true;
    }
    static uint32_t last_rescan_ms = 0;
    if (!g_gamepad.connected && millis() - last_rescan_ms > 15000) {
        last_rescan_ms = millis();
        if (_saved_mac[0] && !NimBLEDevice::getScan()->isScanning()) {
            _connect_by_mac(_saved_mac);
        } else if (!NimBLEDevice::getScan()->isScanning()) {
            NimBLEDevice::getScan()->setAdvertisedDeviceCallbacks(&_gp_auto_scan_cb, false);
            NimBLEDevice::getScan()->start(3, false);
        }
    }
    return false;
}

// ============================================================
//  gamepad_disconnect()
// ============================================================
void gamepad_disconnect() {
    if (_ble_client && _ble_client->isConnected()) _ble_client->disconnect();
    g_gamepad.connected = false;
    g_gamepad.buttons = g_gamepad.pressed = g_gamepad.released = 0;
}