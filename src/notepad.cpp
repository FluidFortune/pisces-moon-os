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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "SdFat.h"
#include "keyboard.h"
#include "touch.h" // Needed for Header Tap
#include "theme.h"
#include "apps.h"

extern Arduino_GFX *gfx;
extern SdFat sd;
extern SemaphoreHandle_t spi_mutex;

void draw_journal_ui(String &text) {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, C_GREEN);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
    gfx->print("JOURNAL | TAP HEADER TO SAVE & EXIT");

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(5, 32);
    gfx->print(text);
}

void run_notepad() {
    String buffer = "";

    // Ensure /logs/ exists — wrap in spi_mutex (SPI Bus Treaty)
    if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (!sd.exists("/logs")) {
            sd.mkdir("/logs");
        }
        xSemaphoreGive(spi_mutex);
    }
    
    draw_journal_ui(buffer);
    bool running = true;

    while(running) {
        char c = get_keypress(); 

        if (c != 0) {
            if (c == 8 || c == 127) { 
                if (buffer.length() > 0) {
                    buffer.remove(buffer.length() - 1);
                    draw_journal_ui(buffer); 
                }
            } 
            else if (c == 13 || c == 10) { 
                buffer += "\n";
                gfx->println();
            } 
            else { 
                buffer += c;
                gfx->print(c); 
            }
        }

        // --- HEADER TAP — uses v1.0.1 ty < 40 global convention ---
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 40) {
                // Wait for lift
                while(get_touch(&tx, &ty)) { delay(10); yield(); }

                gfx->fillScreen(C_BLACK);
                gfx->fillRect(0, 0, 320, 24, C_DARK);
                gfx->setCursor(10, 7);
                gfx->setTextColor(C_GREEN);
                gfx->print("SAVE AS...");
                
                gfx->setCursor(10, 50);
                gfx->setTextColor(C_WHITE);
                gfx->print("Enter filename (no spaces):");
                
                String filename = get_text_input(10, 70);
                if(filename == "") filename = "journal.txt";
                
                String fullPath = "/logs/" + filename;

                // Wrap SD save in spi_mutex (SPI Bus Treaty)
                bool saved = false;
                if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    FsFile file = sd.open(fullPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
                    if (file) {
                        file.print(buffer);
                        file.close();
                        saved = true;
                    }
                    xSemaphoreGive(spi_mutex);
                }
                if (saved) {
                    gfx->setCursor(10, 110);
                    gfx->setTextColor(C_GREEN);
                    gfx->print("Saved to ");
                    gfx->print(fullPath);
                } else {
                    gfx->setCursor(10, 110);
                    gfx->setTextColor(C_RED);
                    gfx->print("SD Save Error!");
                }
                
                delay(2000); 
                running = false; 
            }
        }

        delay(15); 
        yield();
    }
}