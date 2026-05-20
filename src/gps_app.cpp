// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include <TinyGPSPlus.h>
#include "keyboard.h"
#include "pm_input.h"
#include "touch.h"
#include "theme.h"
#include "apps.h"
#include "wardrive.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
static constexpr int DISP_W = 480;
static constexpr int DISP_H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 240;
static constexpr int DISP_H = 135;
#else
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 240;
#endif
extern TinyGPSPlus gps;

void run_gps() {
    gfx->fillScreen(C_BLACK);

    // Header — full width
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->drawFastHLine(0, 24, DISP_W, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->print("GPS LINK | " PM_EXIT_COPY);

    // Label column on left, value column with offset
    const int labelX = 10;
    const int valueX = 130;

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(labelX, 50);  gfx->print("Latitude:");
    gfx->setCursor(labelX, 80);  gfx->print("Longitude:");
    gfx->setCursor(labelX, 110); gfx->print("Altitude:");
    gfx->setCursor(labelX, 140); gfx->print("Satellites:");
    gfx->setCursor(labelX, 170); gfx->print("Status:");

    unsigned long lastUpdate = 0;

    while (true) {
        if (millis() - lastUpdate > 1000) {
            lastUpdate = millis();
            // Clear the values column to the right edge
            gfx->fillRect(valueX, 40, DISP_W - valueX - 5, 160, C_BLACK);
            gfx->setTextColor(C_GREEN);

            if (gps.location.isValid()) {
                gfx->setCursor(valueX, 50); gfx->print(gps.location.lat(), 6);
                gfx->setCursor(valueX, 80); gfx->print(gps.location.lng(), 6);
            } else {
                gfx->setCursor(valueX, 50); gfx->print("--");
                gfx->setCursor(valueX, 80); gfx->print("--");
            }

            gfx->setCursor(valueX, 110);
            if (gps.altitude.isValid()) gfx->printf("%.1f ft", gps.altitude.feet());
            else gfx->print("--");

            gfx->setCursor(valueX, 140);
            gfx->print(gps.satellites.value());

            gfx->setCursor(valueX, 170);
            if (!gps.location.isValid()) {
                gfx->setTextColor(C_GREY);
                gfx->print("Searching...");
            } else {
                gfx->setTextColor(C_GREEN);
                gfx->print("3D FIX OK");
            }
        }

        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
                break;
            }
        }
        if (pm_is_exit_key(get_keypress())) break;
        yield();
    }
}
