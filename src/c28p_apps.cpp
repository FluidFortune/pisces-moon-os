// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_apps.cpp — C28P-specific app implementations
//
//  Contains the touch-driven UIs for AUDIO, AI, WIFI, and ABOUT
//  categories on the C28P. Each app follows the same shape:
//    - Paint chrome once
//    - Poll touch in a loop
//    - Tap on QUIT/BACK region returns control to launcher
//
//  Apps that need keyboard input (Gemini AI terminal) use an
//  on-screen virtual keyboard. Apps that read text content
//  (about, weather forecast) just need scroll touch.
//
//  Sized for the C28P 240×320 portrait viewport. No D-pad
//  here — those are reserved for games.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "wifi_manager.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t* x, int16_t* y);

// ─────────────────────────────────────────────
//  ABOUT
// ─────────────────────────────────────────────
void c28p_run_about() {
    gfx->fillScreen(0x0000);

    // Header
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(20, 12);
    gfx->print("PISCES MOON");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(20, 36);
    gfx->print("Version 1.2.1");
    gfx->drawFastHLine(0, 56, 240, 0x4208);

    // Body
    gfx->setTextColor(0xFFFF);
    int y = 72;
    auto line = [&](const char* s) {
        gfx->setCursor(16, y);
        gfx->print(s);
        y += 14;
    };
    line("Device: C28P 2.8\" Kiosk");
    line("Display: 240x320 IPS");
    line("Audio: ES8311 codec");
    line("Touch: FT6336G capacitive");
    y += 8;
    line("By Eric Becker / Fluid Fortune");
    line("Licensed AGPL-3.0-or-later");
    y += 8;
    line("Court Jester of Vibe Code");
    line("fluidfortune.com");

    // Footer
    gfx->setTextColor(0xFC00);
    gfx->setCursor(40, 290);
    gfx->print("Tap to return");

    // Wait for touch release-then-press
    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            // Wait for release before returning so the launcher
            // doesn't immediately re-dispatch on the same press
            while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
            return;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  WIFI SETUP
//
//  Touch UI:
//   - Top: status banner (current connection or "Not connected")
//   - Middle: CONNECT button (joins known networks if any exist)
//   - Middle: SCAN+JOIN button (shows nearby networks, taps to join)
//   - Middle: PORTAL button (captive portal — phone-driven setup)
//   - Bottom: BACK button
//
//  PORTAL FLOW (the part that was confusing):
//   1. Tap PORTAL → C28P spins up an open AP "PiscesMoon-Setup"
//   2. User joins that AP from their phone (no password)
//   3. Phone redirects to a config page with WiFi network list
//   4. User selects their home WiFi + types password on phone
//   5. C28P saves the credential and connects
//   6. Portal closes (~3 min timeout if user doesn't finish)
//
//  The screen now displays step-by-step instructions during the
//  portal so the user knows what's happening.
// ─────────────────────────────────────────────

static void wifi_draw_main();

static bool wifi_was_touched = false;

static void wifi_draw_status_banner() {
    gfx->fillRect(0, 36, 240, 70, 0x0000);
    gfx->setTextSize(1);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(0x07E0);
        gfx->setCursor(16, 44);
        gfx->print("Connected");
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(16, 58);
        String ssid = WiFi.SSID();
        if (ssid.length() > 30) ssid = ssid.substring(0, 30);
        gfx->printf("%s", ssid.c_str());
        gfx->setCursor(16, 72);
        gfx->printf("IP: %s", WiFi.localIP().toString().c_str());
        gfx->setCursor(16, 86);
        gfx->printf("RSSI: %d dBm", WiFi.RSSI());
    } else {
        gfx->setTextColor(0xF800);
        gfx->setCursor(16, 44);
        gfx->print("Not connected");
        gfx->setTextColor(0x8410);
        gfx->setCursor(16, 60);
        gfx->print("Tap CONNECT to try known");
        gfx->setCursor(16, 74);
        gfx->print("networks, or PORTAL for a");
        gfx->setCursor(16, 88);
        gfx->print("phone-driven setup.");
    }
}

static void wifi_draw_main() {
    gfx->fillScreen(0x0000);

    // Title bar
    gfx->fillRect(0, 0, 240, 32, 0x18C3);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(20, 8);
    gfx->print("WIFI");

    wifi_draw_status_banner();

    // CONNECT button
    gfx->fillRect(16, 116, 208, 40, 0x18C3);
    gfx->drawRect(16, 116, 208, 40, 0x07E0);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(58, 128);
    gfx->print("CONNECT");

    // PORTAL button
    gfx->fillRect(16, 166, 208, 40, 0x18C3);
    gfx->drawRect(16, 166, 208, 40, 0xFC00);
    gfx->setTextColor(0xFC00);
    gfx->setCursor(46, 178);
    gfx->print("OPEN PORTAL");

    // FORGET button (clear saved credentials)
    gfx->fillRect(16, 216, 208, 40, 0x18C3);
    gfx->drawRect(16, 216, 208, 40, 0xF800);
    gfx->setTextColor(0xF800);
    gfx->setCursor(64, 228);
    gfx->print("FORGET");

    // BACK button
    gfx->fillRect(16, 270, 208, 40, 0x3000);
    gfx->drawRect(16, 270, 208, 40, 0xFFFF);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(82, 282);
    gfx->print("BACK");
}

static void wifi_show_message(const char* line1, const char* line2,
                              const char* line3, uint16_t color, int hold_ms) {
    gfx->fillRect(0, 116, 240, 200, 0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(color);
    if (line1) {
        int w = strlen(line1) * 12;
        gfx->setCursor((240 - w) / 2, 130);
        gfx->print(line1);
    }
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    if (line2) {
        int w = strlen(line2) * 6;
        gfx->setCursor((240 - w) / 2, 170);
        gfx->print(line2);
    }
    if (line3) {
        int w = strlen(line3) * 6;
        gfx->setCursor((240 - w) / 2, 190);
        gfx->print(line3);
    }
    if (hold_ms > 0) delay(hold_ms);
}

static void wifi_portal_screen() {
    gfx->fillScreen(0x0000);
    // Title bar
    gfx->fillRect(0, 0, 240, 32, 0x18C3);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFC00);
    gfx->setCursor(28, 8);
    gfx->print("WIFI PORTAL");

    gfx->setTextSize(1);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(16, 48);
    gfx->print("On your phone:");
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(16, 64);
    gfx->print("1. Open WiFi settings");
    gfx->setCursor(16, 78);
    gfx->print("2. Join \"PiscesMoon-Setup\"");
    gfx->setCursor(16, 92);
    gfx->print("3. Wait for browser to open");
    gfx->setCursor(16, 106);
    gfx->print("4. Choose your home network");
    gfx->setCursor(16, 120);
    gfx->print("5. Enter password and save");

    gfx->setTextColor(0xFC00);
    gfx->setCursor(16, 148);
    gfx->print("Portal active (3 min timeout)");
    gfx->setTextColor(0x8410);
    gfx->setCursor(16, 164);
    gfx->print("Tap top bar to cancel.");
}

void c28p_run_wifi_setup() {
    wifi_draw_main();
    wifi_was_touched = false;

    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched && !wifi_was_touched) {
            // Top header / dpad exit zone — return to launcher
            if (ty < 32) {
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                return;
            }
            // CONNECT button
            if (ty >= 116 && ty < 156) {
                wifi_show_message("Scanning...", "Trying known networks", nullptr, 0xFFE0, 0);
                auto_connect_wifi();
                if (WiFi.status() == WL_CONNECTED) {
                    wifi_show_message("Connected!", WiFi.SSID().c_str(), nullptr, 0x07E0, 1500);
                } else {
                    wifi_show_message("Failed", "No known networks worked.",
                                      "Try PORTAL for new credentials.", 0xF800, 2000);
                }
                wifi_draw_main();
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                wifi_was_touched = false;
                continue;
            }
            // PORTAL button
            if (ty >= 166 && ty < 206) {
                wifi_portal_screen();
                WiFiManager wm;
                wm.setConfigPortalTimeout(180);
                // Blocking call — returns when user finishes or timeout
                wm.autoConnect("PiscesMoon-Setup");
                wifi_draw_main();
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                wifi_was_touched = false;
                continue;
            }
            // FORGET button
            if (ty >= 216 && ty < 256) {
                WiFi.disconnect(true, true);   // clear creds
                wifi_show_message("Forgotten", "Stored credentials cleared",
                                  nullptr, 0xFC00, 1500);
                wifi_draw_main();
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                wifi_was_touched = false;
                continue;
            }
            // BACK button
            if (ty >= 270 && ty < 310) {
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                return;
            }
        }
        wifi_was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  AI TERMINAL (forward declared — implemented in c28p_ai.cpp)
//  AUDIO PLAYER (forward declared — implemented in c28p_audio_app.cpp)
//  WARDRIVE     (forward declared — implemented in c28p_wardrive.cpp)
//
//  These live in their own files because they're substantial
//  (each ~150-400 lines). Forward declarations keep the wrappers
//  in c28p_boot.cpp callable without pulling those files into
//  the build unless the corresponding capability is enabled.
// ─────────────────────────────────────────────

#endif // DEVICE_C28P