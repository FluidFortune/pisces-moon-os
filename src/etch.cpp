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
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "keyboard.h"
#include "gamepad.h"
#include "apps.h"

extern Arduino_GFX *gfx;

void run_etch() {
    gfx->fillScreen(C_GREY);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->setCursor(10, 7); gfx->setTextColor(C_RED);
    gfx->print("ETCH | SPC:ERASE | A:DRAW | Y:ERASE | HDR:EXIT");

    int lastX = -1, lastY = -1;

    // Gamepad cursor — starts center of drawing area
    int gpCurX = 160, gpCurY = 130;
    bool gpDrawing = false;

    while (true) {
        TouchData f = update_touch();

        // Header tap = exit
        if (f.pressed && f.y < 30) {
            int16_t dumpX, dumpY;
            while (get_touch(&dumpX, &dumpY)) { delay(10); yield(); }
            break;
        }

        // Touch drawing (below header)
        if (f.pressed && f.y >= 30) {
            if (lastX != -1) gfx->drawLine(lastX, lastY, f.x, f.y, C_BLACK);
            lastX = f.x; lastY = f.y;
        } else {
            lastX = -1;
        }

        // Keyboard
        char c = get_keypress();
        if (c == ' ' || c == 'q' || c == 'Q') {
            if (c == 'q' || c == 'Q') break;
            // Erase
            gfx->fillScreen(C_GREY);
            gfx->fillRect(0, 0, 320, 24, C_DARK);
            gfx->setCursor(10, 7); gfx->setTextColor(C_RED);
            gfx->print("SHAKING... ERASED.");
            delay(500);
            gfx->fillRect(0, 0, 320, 24, C_DARK);
            gfx->setCursor(10, 7);
            gfx->print("ETCH | SPC:ERASE | A:DRAW | Y:ERASE | HDR:EXIT");
            gpCurX = 160; gpCurY = 130;
            gpDrawing = false; lastX = -1;
        }

        // Gamepad
        if (gamepad_poll()) break; // HOME = exit

        // D-pad moves the gamepad cursor (4px per press, held = smooth)
        int step = 4;
        if (gamepad_held(GP_UP))    gpCurY = max(30,        gpCurY - step);
        if (gamepad_held(GP_DOWN))  gpCurY = min(239,       gpCurY + step);
        if (gamepad_held(GP_LEFT))  gpCurX = max(0,         gpCurX - step);
        if (gamepad_held(GP_RIGHT)) gpCurX = min(319,       gpCurX + step);

        // A = draw at cursor position
        if (gamepad_held(GP_A)) {
            if (gpDrawing) {
                gfx->drawLine(lastX, lastY, gpCurX, gpCurY, C_BLACK);
            }
            // Draw cursor dot
            gfx->fillRect(gpCurX - 1, gpCurY - 1, 3, 3, C_BLACK);
            lastX = gpCurX; lastY = gpCurY;
            gpDrawing = true;
        } else {
            // Not drawing — show cursor as small red dot, lift pen
            if (gpDrawing) {
                // Redraw cursor indicator without leaving a mark
                gfx->fillRect(gpCurX - 2, gpCurY - 2, 5, 5, C_GREY);
            }
            gpDrawing = false;
            lastX = -1;
        }

        // Y = erase entire canvas (shake gesture)
        if (gamepad_pressed(GP_Y)) {
            gfx->fillScreen(C_GREY);
            gfx->fillRect(0, 0, 320, 24, C_DARK);
            gfx->setCursor(10, 7); gfx->setTextColor(C_RED);
            gfx->print("SHAKING... ERASED.");
            delay(500);
            gfx->fillRect(0, 0, 320, 24, C_DARK);
            gfx->setCursor(10, 7);
            gfx->print("ETCH | SPC:ERASE | A:DRAW | Y:ERASE | HDR:EXIT");
            gpCurX = 160; gpCurY = 130;
            gpDrawing = false; lastX = -1;
        }

        // START = exit
        if (gamepad_pressed(GP_START)) break;

        delay(10); yield();
    }
}