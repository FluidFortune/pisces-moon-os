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
#include "keyboard.h"
#include "theme.h"
#include "touch.h" // Added for Touch Exit

extern Arduino_GFX *gfx;

void run_clock() {
    unsigned long startMillis = millis();
    bool stopwatchRunning = false;
    unsigned long stopTime = 0;

    gfx->fillScreen(C_BLACK);
    
    // Draw Header
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->print("SYSTEM CLOCK | TAP HEADER TO EXIT");
    
    gfx->setCursor(10, 150);
    gfx->print("STOPWATCH (SPACE): ");

    while(true) {
        unsigned long up = millis() / 1000;
        int h = up / 3600;
        int m = (up % 3600) / 60;
        int s = up % 60;
        
        gfx->setCursor(40, 60);
        gfx->setTextSize(3);
        gfx->setTextColor(C_GREEN, C_BLACK); 
        gfx->printf("%02d:%02d:%02d", h, m, s);

        char c = get_keypress();
        if (c == ' ') stopwatchRunning = !stopwatchRunning;
        
        gfx->setTextSize(1);
        gfx->setCursor(140, 150); 
        if (stopwatchRunning) stopTime = millis() - startMillis;
        gfx->printf("%lu.00  ", stopTime / 1000); 
        
        // --- NEW EXIT LOGIC: HEADER TAP ---
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) { // If tap is in the top 30 pixels
                while (get_touch(&tx, &ty)) { delay(10); yield(); } // Wait for lift
                break; // Exit app
            }
        }
        
        delay(100); 
        yield();
    }
}