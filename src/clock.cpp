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

#include <Arduino.h>
#include <WiFi.h>
#include <Arduino_GFX_Library.h>
#include "keyboard.h"
#include "theme.h"
#include "touch.h"
#include "time.h"

extern Arduino_GFX *gfx;

static bool clockSyncNTP() {
    if (WiFi.status() != WL_CONNECTED) return false;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    unsigned long t0 = millis();
    while (time(nullptr) < 100000UL && millis() - t0 < 5000) {
        delay(200); yield();
    }
    return time(nullptr) > 100000UL;
}

void run_clock() {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->print("CLOCK | TAP HEADER TO EXIT");

    // Try NTP sync if not already done this boot
    bool hasRealTime = (time(nullptr) > 100000UL);
    if (!hasRealTime) {
        gfx->setTextColor(C_GREY);
        gfx->setCursor(10, 35);
        if (WiFi.status() == WL_CONNECTED) {
            gfx->print("Syncing time via NTP...");
            hasRealTime = clockSyncNTP();
            gfx->fillRect(10, 35, 300, 12, C_BLACK);
        } else {
            gfx->print("No WiFi — showing uptime. Connect via COMMS > WIFI JOIN.");
        }
    }

    bool     swRunning  = false;
    bool     swStarted  = false;
    unsigned long swStart   = 0;
    unsigned long swElapsed = 0;
    unsigned long lastDraw  = 0;

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            break;
        }
        char k = get_keypress();
        if (k == 'q' || k == 'Q') break;

        // Stopwatch controls
        if (k == ' ') {
            if (!swStarted) {
                swStart = millis(); swStarted = true; swRunning = true;
            } else {
                swRunning = !swRunning;
                if (swRunning) swStart = millis() - swElapsed;
            }
        }
        if (k == 'r' || k == 'R') {
            swRunning = false; swStarted = false; swElapsed = 0;
        }
        if (swRunning) swElapsed = millis() - swStart;

        if (millis() - lastDraw < 500) { delay(20); yield(); continue; }
        lastDraw = millis();

        // Main time display
        gfx->fillRect(0, 35, 320, 95, C_BLACK);
        gfx->setTextSize(4);

        if (time(nullptr) > 100000UL) {
            hasRealTime = true;
            struct tm t;
            getLocalTime(&t);
            gfx->setTextColor(C_GREEN);
            gfx->setCursor(18, 45);
            gfx->printf("%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
            gfx->setTextSize(1);
            gfx->setTextColor(C_GREY);
            gfx->setCursor(18, 103);
            const char* dow[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
            const char* mon[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                  "JUL","AUG","SEP","OCT","NOV","DEC"};
            gfx->printf("%s %s %02d %04d  UTC",
                dow[t.tm_wday], mon[t.tm_mon],
                t.tm_mday, t.tm_year + 1900);
        } else {
            // Uptime fallback
            unsigned long up = millis() / 1000;
            gfx->setTextColor(0xFD20);
            gfx->setCursor(18, 45);
            gfx->printf("%02lu:%02lu:%02lu", up/3600, (up%3600)/60, up%60);
            gfx->setTextSize(1);
            gfx->setTextColor(C_GREY);
            gfx->setCursor(18, 103);
            gfx->print("UPTIME  (no WiFi — connect for real time)");
        }

        // Stopwatch
        gfx->fillRect(0, 118, 320, 60, C_BLACK);
        gfx->setTextSize(1);
        gfx->setTextColor(C_GREY);
        gfx->setCursor(10, 122);
        gfx->print("STOPWATCH  SPACE=start/pause  R=reset");
        unsigned long sw   = swElapsed / 1000;
        unsigned long swMs = (swElapsed % 1000) / 10;
        gfx->setTextSize(2);
        gfx->setTextColor(swRunning ? C_GREEN : C_GREY);
        gfx->setCursor(40, 140);
        gfx->printf("%02lu:%02lu:%02lu.%02lu",
                    sw/3600, (sw%3600)/60, sw%60, swMs);

        delay(20); yield();
    }
}
