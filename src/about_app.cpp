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
#include "touch.h" // Added for Touch Exit
#include "theme.h"
#include "apps.h"

extern Arduino_GFX *gfx;

void run_about() {
    gfx->fillScreen(C_BLACK);
    
    // Header
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->print("ABOUT | TAP HEADER TO EXIT");

    // Big Title
    gfx->setTextSize(2);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(45, 60);
    gfx->print("PISCES MOON OS");
    
    // Version and info
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(105, 95); gfx->print("Version 1.0.0");
    gfx->setCursor(95, 110); gfx->print("\"The Arsenal\"");
    
    gfx->setTextColor(C_GREY);
    gfx->setCursor(50, 145); gfx->print("Built for LilyGO T-Deck Plus");
    gfx->setCursor(90, 165); gfx->print("April 2026");

    // Nostalgia Quote
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(20, 190); gfx->print("> \"Reticulating Splines since '94.\"");

    // Wait for touch exit
    while(true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) { // Top 30 pixels (Header Area)
                while(get_touch(&tx, &ty)) { delay(10); yield(); } // Wait for lift
                break;
            }
        }
        delay(50);
        yield();
    }
}