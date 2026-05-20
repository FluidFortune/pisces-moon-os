// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN: stays 320x240 canvas, centered horizontally on T-LoRa Pager (X offset = 80)
// Etch is a drawing canvas — keeping it 320x240 preserves the artwork aspect ratio.

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "keyboard.h"
#include "pm_input.h"
#include "gamepad.h"
#include "apps.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
static constexpr int X_OFF = (480 - 320) / 2;  // = 80 — center 320 canvas in 480 viewport
static constexpr int DISP_W = 480;
#elif defined(DEVICE_CARDPUTER_ADV)
extern Arduino_GFX *gfx;
static constexpr int X_OFF = 0;
static constexpr int DISP_W = 240;
#else
extern Arduino_GFX *gfx;
static constexpr int X_OFF = 0;
static constexpr int DISP_W = 320;
#endif
static constexpr int CANVAS_W = 320;
#ifdef DEVICE_TLORAPAGER
static constexpr int CANVAS_H = 222;
#else
static constexpr int CANVAS_H = 240;
#endif

// Helper: draw the header line — clears entire screen width on T-LoRa Pager,
// so we don't leave artifacts to the sides of the centered canvas.
static void drawEtchHeader() {
    // Full-width black band so anything outside the canvas stays clean
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->setCursor(X_OFF + 10, 7); gfx->setTextColor(C_RED);
#ifdef DEVICE_TLORAPAGER
    gfx->print("ETCH | SPC:ERASE | A:DRAW | Y:ERASE | Q EXIT");
#else
    gfx->print("ETCH | SPC:ERASE | A:DRAW | Y:ERASE | HDR:EXIT");
#endif
}

void run_etch() {
    // Fill the whole screen black (so the side margins on T-LoRa Pager are dark)
    gfx->fillScreen(C_BLACK);
    // Draw the 320x240 canvas area in grey, offset on T-LoRa Pager
    gfx->fillRect(X_OFF, 0, CANVAS_W, CANVAS_H, C_GREY);
    drawEtchHeader();

    int lastX = -1, lastY = -1;

    // Gamepad cursor — starts center of drawing area (canvas coordinates + offset)
    int gpCurX = X_OFF + 160;
    int gpCurY = 130;
    bool gpDrawing = false;

    while (true) {
        TouchData f = update_touch();

        // Header tap = exit (only count touches within canvas region horizontally)
        if (f.pressed && f.y < 30 && f.x >= X_OFF && f.x < X_OFF + CANVAS_W) {
            int16_t dumpX, dumpY;
            while (get_touch(&dumpX, &dumpY)) { delay(10); yield(); }
            break;
        }

        // Touch drawing (below header, within canvas bounds)
        if (f.pressed && f.y >= 30 && f.x >= X_OFF && f.x < X_OFF + CANVAS_W) {
            if (lastX != -1) gfx->drawLine(lastX, lastY, f.x, f.y, C_BLACK);
            lastX = f.x; lastY = f.y;
        } else {
            lastX = -1;
        }

        // Keyboard
        char c = get_keypress();
        if (c == ' ' || pm_is_exit_key(c)) {
            if (pm_is_exit_key(c)) break;
            // Erase — redraw canvas
            gfx->fillRect(X_OFF, 0, CANVAS_W, CANVAS_H, C_GREY);
            drawEtchHeader();
            gfx->setCursor(X_OFF + 10, 7); gfx->setTextColor(C_RED);
            gfx->print("SHAKING... ERASED.");
            delay(500);
            drawEtchHeader();
            gpCurX = X_OFF + 160; gpCurY = 130;
            gpDrawing = false; lastX = -1;
        }

        // Gamepad
        if (gamepad_poll()) break;

        int step = 4;
        if (gamepad_held(GP_UP))    gpCurY = max(30,                       gpCurY - step);
        if (gamepad_held(GP_DOWN))  gpCurY = min(CANVAS_H - 1,             gpCurY + step);
        if (gamepad_held(GP_LEFT))  gpCurX = max(X_OFF,                    gpCurX - step);
        if (gamepad_held(GP_RIGHT)) gpCurX = min(X_OFF + CANVAS_W - 1,     gpCurX + step);

        if (gamepad_held(GP_A)) {
            if (gpDrawing) {
                gfx->drawLine(lastX, lastY, gpCurX, gpCurY, C_BLACK);
            }
            gfx->fillRect(gpCurX - 1, gpCurY - 1, 3, 3, C_BLACK);
            lastX = gpCurX; lastY = gpCurY;
            gpDrawing = true;
        } else {
            if (gpDrawing) {
                gfx->fillRect(gpCurX - 2, gpCurY - 2, 5, 5, C_GREY);
            }
            gpDrawing = false;
            lastX = -1;
        }

        if (gamepad_pressed(GP_Y)) {
            gfx->fillRect(X_OFF, 0, CANVAS_W, CANVAS_H, C_GREY);
            drawEtchHeader();
            gfx->setCursor(X_OFF + 10, 7); gfx->setTextColor(C_RED);
            gfx->print("SHAKING... ERASED.");
            delay(500);
            drawEtchHeader();
            gpCurX = X_OFF + 160; gpCurY = 130;
            gpDrawing = false; lastX = -1;
        }

        if (gamepad_pressed(GP_START)) break;

        delay(10); yield();
    }
}
