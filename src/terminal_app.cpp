// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)
//
// The Gemini terminal — type a prompt, get an AI response. Text buffer
// scrolls with the trackball. Width-aware so the larger T-LoRa Pager display
// shows more characters per line.

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
#include "apps.h"
#include "gemini_client.h"
#include "text_buffer.h"

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

#define TERM_HDR_H      24
#define TERM_BODY_Y     26
static constexpr int TERM_INPUT_H = 22;
static constexpr int TERM_INPUT_Y = DISP_H - TERM_INPUT_H;
static constexpr int TERM_BODY_H  = TERM_INPUT_Y - TERM_BODY_Y;

static void drawHeader() {
    gfx->fillRect(0, 0, DISP_W, TERM_HDR_H, C_DARK);
    gfx->drawFastHLine(0, TERM_HDR_H, DISP_W, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->print("GEMINI TERM | TRACKBALL SCROLL | Q EXIT");
}

static void drawInputPrompt(const String &input, bool active) {
    gfx->fillRect(0, TERM_INPUT_Y, DISP_W, TERM_INPUT_H, 0x0841);
    gfx->drawFastHLine(0, TERM_INPUT_Y, DISP_W, C_GREEN);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(5, TERM_INPUT_Y + 7);
    gfx->print("USER>");
#ifdef DEVICE_TLORAPAGER
    if (!active && input.length() == 0) {
        gfx->setTextColor(C_GREY);
        gfx->setCursor(45, TERM_INPUT_Y + 7);
        gfx->print("CLK TYPE | Q EXIT");
        return;
    }
#endif
    gfx->setTextColor(active ? C_WHITE : C_GREY);
    gfx->setCursor(45, TERM_INPUT_Y + 7);
    int maxChars = (DISP_W - 52) / 6;
    String visible = input;
    if ((int)visible.length() > maxChars) visible = visible.substring(visible.length() - maxChars);
    gfx->print(visible);
    if (active) gfx->print("_");
}

void run_terminal() {
    gfx->fillScreen(C_BLACK);
    drawHeader();

    TextBuffer tb;
    if (!tb.init()) {
        gfx->setTextColor(0xF800);
        gfx->setCursor(10, 50);
        gfx->print("ERROR: Could not allocate text buffer.");
        delay(3000);
        return;
    }

    tb.visible_rows = TERM_BODY_H / 10;
    tb.append("Gemini Terminal ready.", 0x07E0);
    tb.append("Type your prompt below and press ENTER.", C_GREY);
    tb.append("Roll trackball UP/DOWN to scroll history.", C_GREY);
    tb.append("", C_WHITE);
    tb.draw(0, TERM_BODY_Y, DISP_W, TERM_BODY_H, 1);

    String prompt = "";
    bool inputActive =
#ifdef DEVICE_TLORAPAGER
        false;
#else
        true;
#endif
    drawInputPrompt(prompt, inputActive);

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < TERM_HDR_H) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            tb.free_mem();
            return;
        }

        TrackballState tbs = update_trackball();
        char k = get_keypress();

#ifdef DEVICE_TLORAPAGER
        if (tbs.clicked) {
            inputActive = !inputActive;
            drawInputPrompt(prompt, inputActive);
            delay(120);
            yield();
            continue;
        }
        if (!inputActive && pm_is_exit_key(k)) {
            tb.free_mem();
            return;
        }
#endif

        if (!inputActive && tbs.y == -1) {
            tb.scroll_up(2);
            tb.draw(0, TERM_BODY_Y, DISP_W, TERM_BODY_H, 1);
        } else if (!inputActive && tbs.y == 1) {
            tb.scroll_down(2);
            tb.draw(0, TERM_BODY_Y, DISP_W, TERM_BODY_H, 1);
        }

#ifndef DEVICE_TLORAPAGER
        if (pm_is_exit_key(k) && prompt.length() == 0) {
            tb.free_mem();
            return;
        }
#endif

        if (inputActive && k) {
            if (k == 8 || k == 127) {
                if (prompt.length() > 0) prompt.remove(prompt.length() - 1);
                drawInputPrompt(prompt, inputActive);
            } else if (k == 13 || k == 10) {
                inputActive = false;
            } else if (k >= 32 && k <= 126) {
                prompt += k;
                drawInputPrompt(prompt, inputActive);
            }
        }

        if (inputActive || prompt.length() == 0) {
            delay(50);
            continue;
        }

        String userLine = "USER> " + prompt;
        tb.append_wrapped(userLine.c_str(), 0x07E0);
        tb.append("", C_WHITE);
        tb.scroll_to_bottom();
        tb.draw(0, TERM_BODY_Y, DISP_W, TERM_BODY_H, 1);

        tb.append("AI> [thinking...]", 0xFD20);
        tb.scroll_to_bottom();
        tb.draw(0, TERM_BODY_Y, DISP_W, TERM_BODY_H, 1);

        String response = ask_gemini(prompt);

        if (tb.count > 0) tb.count--;

        String aiLine = "AI> " + response;
        tb.append_wrapped(aiLine.c_str(), 0xFFFF);
        tb.append("", C_WHITE);
        tb.scroll_to_bottom();
        tb.draw(0, TERM_BODY_Y, DISP_W, TERM_BODY_H, 1);
        prompt = "";
#ifndef DEVICE_TLORAPAGER
        inputActive = true;
#endif
        drawInputPrompt(prompt, inputActive);

        yield();
    }
}
