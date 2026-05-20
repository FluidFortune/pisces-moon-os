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
#include "SdFat.h"
#include "keyboard.h"
#include "touch.h"
#include "trackball.h"
#include "pm_input.h"
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
extern SdFat sd;

static void draw_journal_ui(String &text, bool textActive = true) {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->drawFastHLine(0, 24, DISP_W, C_GREEN);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
#ifdef DEVICE_TLORAPAGER
    gfx->print(textActive ? "JOURNAL | TYPING | " PM_TEXT_ACTIVE_COPY
                          : "JOURNAL | CLK TYPE | Q SAVE/EXIT");
#else
    gfx->print("JOURNAL | TAP HEADER TO SAVE & EXIT");
#endif

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(5, 32);
    gfx->print(text);
}

static bool save_journal(String &buffer) {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->print("SAVE AS...");

    gfx->setCursor(10, 50);
    gfx->setTextColor(C_WHITE);
    gfx->print("Enter filename (no spaces):");

    String filename = get_text_input(10, 70);
    if (filename == "##EXIT##") filename = "";
    if (filename == "") filename = "journal.txt";

    String fullPath = "/logs/" + filename;

    FsFile file = sd.open(fullPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
        file.print(buffer);
        file.close();
        gfx->setCursor(10, 110);
        gfx->setTextColor(C_GREEN);
        gfx->print("Saved to ");
        gfx->print(fullPath);
    } else {
        gfx->setCursor(10, 110);
        gfx->setTextColor(C_RED);
        gfx->print("SD Save Error!");
        delay(2000);
        return false;
    }

    delay(2000);
    return true;
}

void run_notepad() {
    String buffer = "";

    if (!sd.exists("/logs")) sd.mkdir("/logs");

    bool textActive =
#ifdef DEVICE_TLORAPAGER
        false;
#else
        true;
#endif
    draw_journal_ui(buffer, textActive);
    bool running = true;

    while (running) {
        char c = get_keypress();
#ifdef DEVICE_TLORAPAGER
        TrackballState tb = update_trackball();
        if (tb.clicked) {
            textActive = !textActive;
            draw_journal_ui(buffer, textActive);
            delay(120);
            yield();
            continue;
        }
        if (!textActive) {
            if (pm_is_exit_key(c)) {
                save_journal(buffer);
                running = false;
            }
            delay(15);
            yield();
            continue;
        }
#endif

        if (c != 0) {
            if (c == 8 || c == 127) {
                if (buffer.length() > 0) {
                    buffer.remove(buffer.length() - 1);
                    draw_journal_ui(buffer, textActive);
                }
            } else if (c == 13 || c == 10) {
                buffer += "\n";
                gfx->println();
            } else {
                buffer += c;
                gfx->print(c);
            }
        }

        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }

                save_journal(buffer);
                running = false;
            }
        }

        delay(15);
        yield();
    }
}
