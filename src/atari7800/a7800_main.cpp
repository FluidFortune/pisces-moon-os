// Pisces Moon OS — Atari 7800 emulator (stub for v1.2.0)
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// This is the placeholder entry. The actual emulator core is under
// development; this stub shows users that the system slot exists
// while keeping the build green and the RetroPack browser useful.

#ifdef DEVICE_CARDPUTER_ADV
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "cardputer_ui.h"
#include "keyboard.h"
#include "pm_input.h"
#include "theme.h"

extern Arduino_GFX *gfx;

void retro_launch_a7800() {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, CP_DISP_W, CP_HDR_H, CP_HDR_BG);
    gfx->drawFastHLine(0, CP_HDR_H - 1, CP_DISP_W, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(CP_HDR_FG);
    gfx->setCursor(4, 4);
    gfx->print("ATARI 7800 | Q exit");

    gfx->setTextSize(2);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(50, 26);
    gfx->print("7800");
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(60, 50);
    gfx->print("ATARI");

    gfx->setTextColor(C_GREY);
    gfx->setCursor(4, 74);
    gfx->print("Emulator in development.");
    gfx->setCursor(4, 86);
    gfx->print("Status:");
    gfx->setCursor(4, 98);
    gfx->setTextColor(C_WHITE);
    gfx->print("[ ] 6502 CPU core");
    gfx->setCursor(4, 108);
    gfx->print("[ ] MARIA display list");
    gfx->setCursor(4, 118);
    gfx->print("[ ] Cartridge loader");

    while (true) {
        char c = get_keypress();
        if (pm_is_exit_key(c)) return;
        delay(50); yield();
    }
}
#endif // DEVICE_CARDPUTER_ADV
