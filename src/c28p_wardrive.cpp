// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_wardrive.cpp — Stationary Wardrive UI for C28P
//
//  PHILOSOPHY:
//
//  The C28P has no GPS and no battery for mobility — it's a
//  desk kiosk. So instead of mobile wardriving with location
//  trails, we do *stationary* wardriving: passively monitor
//  the RF environment from one fixed location and detect what
//  changes over time. Networks that appear/disappear. Devices
//  that probe but never associate. BLE beacons that come and go.
//
//  Ghost Engine principle still applies: once started, the
//  scan task never stops. The UI is just a window into the
//  scan state. Tapping BACK returns to launcher, scan continues
//  in the background. Pinned to Core 0 so it doesn't compete
//  with the touch/display on Core 1.
//
//  USE CASES:
//    - Watch your home network for unfamiliar devices
//    - Spot wardrivers in your area (devices appearing briefly,
//      probing for known SSIDs, then leaving)
//    - Establish baseline RF environment for anomaly detection
//      (planned v1.3 feature)
//
//  NoSQL writes continue regardless of UI state. Data
//  accumulates whether the user is looking or not.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "wardrive.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t* x, int16_t* y);

// Defined in c28p_anomaly.cpp — stationary anomaly detection
extern void c28p_anomaly_init();
extern int  c28p_anomaly_baseline_size();
extern int  c28p_anomaly_recent_alerts();

static uint32_t last_ui_refresh = 0;
static int last_wifi = -1, last_ble = -1;
static int last_alerts = -1;

static void draw_wd_chrome() {
    gfx->fillScreen(0x0000);

    // Title bar
    gfx->fillRect(0, 0, 240, 32, 0x18C3);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFC00);   // orange — security tools
    gfx->setCursor(20, 8);
    gfx->print("WARDRIVE");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(150, 14);
    gfx->print("STATIONARY");

    // Sub-header explaining mode
    gfx->setTextColor(0x4208);
    gfx->setCursor(8, 38);
    gfx->print("Passive RF monitor — no GPS");

    // Stats panel
    gfx->drawFastHLine(0, 56, 240, 0x4208);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(20, 60);
    gfx->print("NOW SEEING");
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(20, 76);
    gfx->print("WIFI");
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(20, 104);
    gfx->print("BLE");
    gfx->setTextColor(0xFC00);
    gfx->setCursor(140, 76);
    gfx->print("ALERT");
    gfx->setTextColor(0x8410);
    gfx->setTextSize(1);
    gfx->setCursor(140, 108);
    gfx->printf("base:%d", c28p_anomaly_baseline_size());

    // Status panel (where ghost engine state lives)
    gfx->drawFastHLine(0, 132, 240, 0x4208);
    gfx->setTextColor(0x8410);
    gfx->setCursor(8, 140);
    gfx->print("GHOST ENGINE");

    // Activity log area
    gfx->drawFastHLine(0, 192, 240, 0x4208);
    gfx->setTextColor(0x8410);
    gfx->setCursor(8, 200);
    gfx->print("LIVE CAPTURE");

    // BACK button
    gfx->fillRect(180, 280, 56, 36, 0x3000);
    gfx->drawRect(180, 280, 56, 36, 0xF800);
    gfx->setTextSize(1);
    gfx->setTextColor(0xF800);
    gfx->setCursor(196, 294);
    gfx->print("BACK");

    // PAUSE/RESUME button — explicit control over background scan
    gfx->fillRect(4, 280, 80, 36, 0x18C3);
    gfx->drawRect(4, 280, 80, 36, wardrive_active ? 0x07E0 : 0xF800);
    gfx->setTextColor(wardrive_active ? 0x07E0 : 0xF800);
    gfx->setCursor(20, 294);
    gfx->print(wardrive_active ? "ACTIVE" : "STOPPED");
}

static void update_stats() {
    // Use the most recent scan counts (networks_found, bt_found) for
    // the at-a-glance numbers — what's visible right now. The lifetime
    // totals are shown below in the live capture panel for context.
    if (last_wifi != networks_found) {
        gfx->fillRect(100, 74, 40, 24, 0x0000);
        gfx->setTextSize(3);
        gfx->setTextColor(0x07FF);
        gfx->setCursor(100, 74);
        gfx->printf("%d", networks_found);
        last_wifi = networks_found;
    }
    if (last_ble != bt_found) {
        gfx->fillRect(100, 102, 40, 24, 0x0000);
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(100, 102);
        gfx->printf("%d", bt_found);
        last_ble = bt_found;
    }
    int alerts = c28p_anomaly_recent_alerts();
    if (last_alerts != alerts) {
        gfx->fillRect(200, 74, 40, 24, 0x0000);
        gfx->setTextSize(3);
        gfx->setTextColor(alerts > 0 ? 0xF800 : 0x4208);
        gfx->setCursor(200, 74);
        gfx->printf("%d", alerts);
        last_alerts = alerts;
    }

    // Update ghost engine status
    gfx->fillRect(8, 155, 230, 30, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(wardrive_active ? 0x07E0 : 0xF800);
    gfx->setCursor(8, 158);
    gfx->print(wardrive_active ? "Scanning WiFi + BLE" : "Idle");
    gfx->setTextColor(0x8410);
    gfx->setCursor(8, 172);
    uint32_t age = (millis() - last_scan_ms) / 1000;
    gfx->printf("Last scan: %lus ago", age);
}

static void update_live_capture() {
    // Big numbers above are CURRENT (most recent scan).
    // Numbers here are SESSION totals — cumulative since the wardrive
    // task started.
    gfx->fillRect(8, 215, 230, 60, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(8, 215);
    gfx->print("Session totals:");
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(8, 230);
    gfx->printf("  WiFi:   %d obs", networks_total);
    gfx->setCursor(8, 245);
    gfx->printf("  BLE:    %d obs", ble_total);
    gfx->setCursor(8, 260);
    gfx->printf("  ESP:    %d", esp_found);
    gfx->setTextColor(0x4208);
    gfx->setCursor(8, 275);
    gfx->print("Tap BACK -- scan persists.");
}

void c28p_run_wardrive() {
    Serial.println("[C28P-WD] Wardrive UI starting");

    // Initialize anomaly detection (loads baseline from NoSQL).
    // Idempotent — safe to call repeatedly.
    c28p_anomaly_init();

    // Ghost Engine: start the wardrive task if not already running.
    // Once started it never stops. The UI just observes its state.
    if (!wardrive_active) {
        Serial.println("[C28P-WD] Spawning wardrive task (Ghost Engine)");
        init_wardrive_core();
    }

    draw_wd_chrome();
    last_wifi = -1;
    last_ble = -1;
    last_alerts = -1;
    update_stats();
    update_live_capture();

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            // BACK button
            if (tx >= 180 && tx < 236 && ty >= 280 && ty < 316) {
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                return;
            }
            // PAUSE/RESUME button
            if (tx >= 4 && tx < 84 && ty >= 280 && ty < 316) {
                if (wardrive_active) {
                    Serial.println("[C28P-WD] Pausing wardrive");
                    wardrive_teardown(3000);
                } else {
                    Serial.println("[C28P-WD] Resuming wardrive");
                    init_wardrive_core();
                }
                delay(200);
                draw_wd_chrome();
                last_wifi = -1; last_ble = -1;
                update_stats();
                update_live_capture();
            }
        }

        // Refresh stats every 500ms
        if (millis() - last_ui_refresh > 500) {
            update_stats();
            update_live_capture();
            last_ui_refresh = millis();
        }

        was_touched = touched;
        delay(20);
        yield();
    }
}

#endif // DEVICE_C28P