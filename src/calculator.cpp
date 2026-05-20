// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)
//
// Layout adapts to display width: result panel on the left, button grid on the right.

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

void draw_calc_btn(int x, int y, int w, int h, const char* label, uint16_t color) {
    gfx->drawRoundRect(x, y, w, h, 8, color);
    gfx->setTextColor(color);
    gfx->setCursor(x + (w/3), y + (h/3) + 2);
    gfx->print(label);
}

void run_calculator() {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
    gfx->print("PISCES MATH | " PM_EXIT_SHORT_COPY);

    // Result panel — left side, ~45% of width
    const int resultW = (DISP_W < 400) ? 140 : 180;
    const int resultX = 10;
    const int resultY = 40;
    const int resultH = 180;
    gfx->drawRect(resultX, resultY, resultW, resultH, C_GREY);

    // Button grid — right side, 4 columns x 4 rows
    const int gridLeftMargin = resultX + resultW + 15;
    const int gridRightMargin = 10;
    const int gridW = DISP_W - gridLeftMargin - gridRightMargin;
    const int btnW  = (gridW - 12) / 4;        // 4 cols, 4px gaps
    const int btnH  = 40;
    const int btnGapX = 4;
    const int btnGapY = 5;
    const int startX = gridLeftMargin;
    const int startY = 40;

    String input = "0";
    float val1 = 0;
    char op = ' ';
    bool was_pressed = false;
    bool running = true;

    const char* keys[16] = {"1","2","3","+","4","5","6","-","7","8","9","*","C","0","=","/"};

    for (int i = 0; i < 16; i++) {
        int bx = startX + ((i % 4) * (btnW + btnGapX));
        int by = startY + ((i / 4) * (btnH + btnGapY));
        draw_calc_btn(bx, by, btnW, btnH, keys[i], C_WHITE);
    }

    while (running) {
        if (pm_is_exit_key(get_keypress())) break;
        TouchData f = update_touch();
        if (f.pressed && !was_pressed) {
            if (f.y < 30) {
                int16_t dumpX, dumpY;
                while (get_touch(&dumpX, &dumpY)) { delay(10); yield(); }
                running = false;
                break;
            }
            for (int i = 0; i < 16; i++) {
                int bx = startX + ((i % 4) * (btnW + btnGapX));
                int by = startY + ((i / 4) * (btnH + btnGapY));
                if (f.x > bx-2 && f.x < bx+btnW+2 && f.y > by-2 && f.y < by+btnH+2) {
                    const char* k = keys[i];
                    gfx->fillRoundRect(bx, by, btnW, btnH, 8, C_GREY);
                    if (isdigit(k[0])) {
                        if (input == "0") input = "";
                        input += k;
                    } else if (k[0] == 'C') {
                        input = "0"; val1 = 0; op = ' ';
                    } else if (k[0] == '=') {
                        float val2 = input.toFloat();
                        if (op == '+') val1 += val2;
                        else if (op == '-') val1 -= val2;
                        else if (op == '*') val1 *= val2;
                        else if (op == '/') { if (val2 != 0) val1 /= val2; }
                        input = (val1 == (long)val1) ? String((long)val1) : String(val1, 2);
                        op = ' ';
                    } else {
                        val1 = input.toFloat();
                        op = k[0];
                        input = "0";
                    }
                    gfx->fillRect(resultX + 2, resultY + 2, resultW - 4, resultH - 4, C_BLACK);
                    gfx->setCursor(resultX + 5, resultY + 20); gfx->setTextColor(C_WHITE); gfx->setTextSize(2);
                    gfx->print(input); gfx->setTextSize(1);
                    delay(100);
                    gfx->fillRoundRect(bx, by, btnW, btnH, 8, C_BLACK);
                    draw_calc_btn(bx, by, btnW, btnH, k, C_WHITE);
                }
            }
            was_pressed = true;
        } else if (!f.pressed) {
            was_pressed = false;
        }
        yield(); delay(10);
    }
}
