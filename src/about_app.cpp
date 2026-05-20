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

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "keyboard.h"
#include "pm_input.h"
#include "touch.h"
#include "theme.h"
#include "apps.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif

void run_about() {
#ifdef DEVICE_TLORAPAGER
    const int W = 480;
    const int H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
    const int W = 240;
    const int H = 135;
#else
    const int W = 320;
    const int H = 240;
#endif
    const int CX = W / 2;

    gfx->fillScreen(C_BLACK);

    // Header — full width
    gfx->fillRect(0, 0, W, 24, C_DARK);
    gfx->drawFastHLine(0, 24, W, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->print("ABOUT | " PM_EXIT_COPY);

    const int titleY =
#ifdef DEVICE_TLORAPAGER
        42;
#elif defined(DEVICE_CARDPUTER_ADV)
        32;
#else
        60;
#endif
    const int infoY =
#ifdef DEVICE_TLORAPAGER
        78;
#elif defined(DEVICE_CARDPUTER_ADV)
        62;
#else
        95;
#endif
    const int buildY =
#ifdef DEVICE_TLORAPAGER
        118;
#elif defined(DEVICE_CARDPUTER_ADV)
        94;
#else
        145;
#endif

    // Big Title centered (14 chars * 12px @ size 2 = ~168px)
    gfx->setTextSize(2);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(CX - 84, titleY);
    gfx->print("PISCES MOON OS");

    // Version / codename — centered
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    const char* version  = "Version 1.2.0";
    const char* codename = "\"Multi-Device\"";
    gfx->setCursor(CX - (int)strlen(version) * 3, infoY);
    gfx->print(version);
    gfx->setCursor(CX - (int)strlen(codename) * 3, infoY + 15);
    gfx->print(codename);

    // Build info — centered
    gfx->setTextColor(C_GREY);
#ifdef DEVICE_CARDPUTER_ADV
    const char* built = "Built for Cardputer ADV";
#else
    const char* built = "Built for LilyGO T-Deck Plus / T-LoRa Pager";
#endif
    const char* date  = "May 2026";
    gfx->setCursor(CX - (int)strlen(built) * 3, buildY);
    gfx->print(built);
    gfx->setCursor(CX - (int)strlen(date) * 3, buildY + 20);
    gfx->print(date);

    // Nostalgia Quote — left anchored with margin, near bottom
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(20, H - 28);
    gfx->print("> \"Reticulating Splines since '94.\"");

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
                break;
            }
        }
        if (pm_is_exit_key(get_keypress())) break;
        delay(50);
        yield();
    }
}
