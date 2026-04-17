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

#include <Arduino_GFX_Library.h>
#include <TinyGPSPlus.h>
#include "touch.h" // Added for Touch Exit
#include "theme.h"
#include "apps.h"
#include "wardrive.h" 

extern Arduino_GFX *gfx;
extern TinyGPSPlus gps;

void run_gps() {
    gfx->fillScreen(C_BLACK);
    
    // Header
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->print("GPS LINK | TAP HEADER TO EXIT");

    // UI Labels
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 50); gfx->print("Latitude:");
    gfx->setCursor(10, 80); gfx->print("Longitude:");
    gfx->setCursor(10, 110); gfx->print("Altitude:");
    gfx->setCursor(10, 140); gfx->print("Satellites:");
    gfx->setCursor(10, 170); gfx->print("Status:");

    unsigned long lastUpdate = 0;
    
    while(true) {
        if (millis() - lastUpdate > 1000) {
            lastUpdate = millis();
            gfx->fillRect(120, 40, 200, 160, C_BLACK);
            gfx->setTextColor(C_GREEN);
            
            if (gps.location.isValid()) {
                gfx->setCursor(120, 50); gfx->print(gps.location.lat(), 6);
                gfx->setCursor(120, 80); gfx->print(gps.location.lng(), 6);
            } else {
                gfx->setCursor(120, 50); gfx->print("--");
                gfx->setCursor(120, 80); gfx->print("--");
            }

            gfx->setCursor(120, 110);
            if (gps.altitude.isValid()) gfx->printf("%.1f ft", gps.altitude.feet());
            else gfx->print("--");

            gfx->setCursor(120, 140);
            gfx->print(gps.satellites.value());

            gfx->setCursor(120, 170);
            if (!gps.location.isValid()) {
                gfx->setTextColor(C_GREY);
                gfx->print("Searching...");
            } else {
                gfx->setTextColor(C_GREEN);
                gfx->print("3D FIX OK");
            }
        }
        
        // --- NEW EXIT LOGIC: HEADER TAP ---
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while(get_touch(&tx, &ty)) { delay(10); yield(); }
                break;
            }
        }
        yield();
    }
}