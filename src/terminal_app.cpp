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
#include "apps.h"
#include "gemini_client.h"
#include "text_buffer.h"

extern Arduino_GFX *gfx;

#define TERM_HDR_H      24
#define TERM_BODY_Y     26
#define TERM_BODY_H     190
#define TERM_INPUT_Y    218
#define TERM_INPUT_H    22

static void drawHeader() {
    gfx->fillRect(0, 0, 320, TERM_HDR_H, C_DARK);
    gfx->drawFastHLine(0, TERM_HDR_H, 320, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->print("GEMINI TERM | TRACKBALL SCROLL | Q EXIT");
}

static void drawInputPrompt() {
    gfx->fillRect(0, TERM_INPUT_Y, 320, TERM_INPUT_H, 0x0841);
    gfx->drawFastHLine(0, TERM_INPUT_Y, 320, C_GREEN);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(5, TERM_INPUT_Y + 7);
    gfx->print("USER>");
}

void run_terminal() {
    gfx->fillScreen(C_BLACK);
    drawHeader();
    drawInputPrompt();

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
    tb.draw(0, TERM_BODY_Y, 320, TERM_BODY_H, 1);

    while (true) {
        // ── Exit check ──
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < TERM_HDR_H) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            tb.free_mem();
            return;
        }

        // ── Scroll via trackball (only when not typing) ──
        TrackballState tbs = update_trackball();
        if (tbs.y == -1) {
            tb.scroll_up(2);
            tb.draw(0, TERM_BODY_Y, 320, TERM_BODY_H, 1);
        } else if (tbs.y == 1) {
            tb.scroll_down(2);
            tb.draw(0, TERM_BODY_Y, 320, TERM_BODY_H, 1);
        }

        // ── Get input (blocks in keyboard loop) ──
        drawInputPrompt();
        gfx->setTextColor(C_WHITE);
        String prompt = get_text_input(45, TERM_INPUT_Y + 7);
        if (prompt == "##EXIT##") {
            tb.free_mem();
            return;
        }

        if (prompt.length() == 0) {
            delay(50);
            continue;
        }

        // ── Show prompt in history ──
        String userLine = "USER> " + prompt;
        tb.append_wrapped(userLine.c_str(), 0x07E0);
        tb.append("", C_WHITE);
        tb.scroll_to_bottom();
        tb.draw(0, TERM_BODY_Y, 320, TERM_BODY_H, 1);

        // ── Show "thinking" status ──
        tb.append("AI> [thinking...]", 0xFD20);
        tb.scroll_to_bottom();
        tb.draw(0, TERM_BODY_Y, 320, TERM_BODY_H, 1);

        // ── Call Gemini ──
        String response = ask_gemini(prompt);

        // ── Replace the "thinking" line ──
        if (tb.count > 0) tb.count--;  // Remove thinking placeholder

        String aiLine = "AI> " + response;
        tb.append_wrapped(aiLine.c_str(), 0xFFFF);
        tb.append("", C_WHITE);
        tb.scroll_to_bottom();
        tb.draw(0, TERM_BODY_Y, 320, TERM_BODY_H, 1);

        yield();
    }
}
