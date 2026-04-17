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
#include "theme.h"
#include "apps.h"

extern Arduino_GFX *gfx;

void draw_calc_btn(int x, int y, int w, int h, const char* label, uint16_t color) {
    gfx->drawRoundRect(x, y, w, h, 8, color);
    gfx->setTextColor(color);
    gfx->setCursor(x + (w/3), y + (h/3) + 2);
    gfx->print(label);
}

void run_calculator() {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
    gfx->print("PISCES MATH | TAP HEADER EXIT");

    gfx->drawRect(10, 40, 140, 180, C_GREY);
    
    String input = "0";
    float val1 = 0;
    char op = ' ';
    bool was_pressed = false;
    bool running = true;

    const char* keys[16] = {"1", "2", "3", "+", "4", "5", "6", "-", "7", "8", "9", "*", "C", "0", "=", "/"};
    int startX = 165; int startY = 40;

    for(int i=0; i<16; i++) {
        draw_calc_btn(startX + ((i%4) * 38), startY + ((i/4) * 45), 34, 40, keys[i], C_WHITE);
    }

    while(running) {
        TouchData f = update_touch();
        
        if (f.pressed && !was_pressed) {
            
            // --- NEW EXIT LOGIC: HEADER TAP ---
            if (f.y < 30) {
                int16_t dumpX, dumpY;
                while(get_touch(&dumpX, &dumpY)) { delay(10); yield(); }
                running = false;
                break;
            }

            // Standard Calculator Buttons
            for(int i=0; i<16; i++) {
                int bx = startX + ((i%4) * 38);
                int by = startY + ((i/4) * 45);

                // Expanded Hitbox
                if(f.x > bx-2 && f.x < bx+36 && f.y > by-2 && f.y < by+42) {
                    const char* k = keys[i];
                    gfx->fillRoundRect(bx, by, 34, 40, 8, C_GREY); // Flash

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
                        else if (op == '/') { if(val2 != 0) val1 /= val2; }
                        input = (val1 == (long)val1) ? String((long)val1) : String(val1, 2);
                        op = ' '; 
                    } else {
                        val1 = input.toFloat();
                        op = k[0];
                        input = "0";
                    }

                    // Refresh Result Area
                    gfx->fillRect(12, 42, 136, 176, C_BLACK);
                    gfx->setCursor(15, 60); gfx->setTextColor(C_WHITE); gfx->setTextSize(2);
                    gfx->print(input); gfx->setTextSize(1);
                    
                    delay(100);
                    gfx->fillRoundRect(bx, by, 34, 40, 8, C_BLACK);
                    draw_calc_btn(bx, by, 34, 40, k, C_WHITE);
                }
            }
            was_pressed = true;
        } else if (!f.pressed) {
            was_pressed = false;
        }

        yield(); delay(10);
    }
}