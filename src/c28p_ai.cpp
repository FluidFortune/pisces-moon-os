// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_ai.cpp — Gemini AI terminal for C28P
//
//  Touch-driven chat interface to Gemini. Layout:
//    Top:    Title bar ("AI TERMINAL")
//    Middle: Scrollable conversation history (PSRAM-backed buffer)
//    Below:  Current input string preview
//    Bottom: On-screen QWERTY keyboard
//
//  The on-screen keyboard is intentionally compact — 26 letters +
//  space + backspace + send. Numbers and symbols are a future
//  iteration (v1.3 — long-press to swap to numeric layer).
//
//  Requires WiFi to be connected and a Gemini API key to be
//  configured. The launcher's WIFI app handles connection; the
//  API key is set via the captive portal or by editing
//  secrets.h before flashing.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include "gemini_client.h"
#include "text_buffer.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t* x, int16_t* y);

static String input_buf;
static String last_response;

#define KB_TOP        220
#define KB_KEY_W      22
#define KB_KEY_H      32
#define KB_PADDING    2

// Three QWERTY rows
static const char* kb_row1 = "QWERTYUIOP";
static const char* kb_row2 = "ASDFGHJKL";
static const char* kb_row3 = "ZXCVBNM";

// ─────────────────────────────────────────────
//  Draw chrome
// ─────────────────────────────────────────────
static void draw_ai_chrome() {
    gfx->fillScreen(0x0000);

    // Title bar
    gfx->fillRect(0, 0, 240, 28, 0x18C3);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(20, 6);
    gfx->print("AI TERMINAL");
    gfx->setTextSize(1);
    gfx->setTextColor(WiFi.status() == WL_CONNECTED ? 0x07E0 : 0xF800);
    gfx->setCursor(180, 12);
    gfx->print(WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE");

    // Conversation area
    gfx->fillRect(0, 32, 240, 160, 0x0000);
    gfx->drawFastHLine(0, 30, 240, 0x4208);
    gfx->drawFastHLine(0, 194, 240, 0x4208);

    // Input preview
    gfx->fillRect(0, 196, 240, 22, 0x18C3);
    gfx->drawFastHLine(0, 218, 240, 0x4208);

    // Keyboard background
    gfx->fillRect(0, KB_TOP, 240, 320 - KB_TOP, 0x18C3);
}

static void draw_response(const String& text) {
    gfx->fillRect(0, 32, 240, 160, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(4, 38);
    gfx->print("> ");
    gfx->setTextColor(0xFFFF);
    // Word-wrap into 38-char lines
    int line = 0;
    int start = 0;
    for (int i = 0; i <= text.length() && line < 18; i++) {
        if (i - start >= 38 || i == text.length()) {
            String chunk = text.substring(start, i);
            gfx->setCursor(4, 38 + line * 9);
            gfx->print(chunk);
            start = i;
            line++;
        }
    }
}

static void draw_input() {
    gfx->fillRect(0, 196, 240, 22, 0x18C3);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(4, 202);
    String show = input_buf;
    if (show.length() > 36) show = show.substring(show.length() - 36);
    gfx->printf(">%s_", show.c_str());
}

// ─────────────────────────────────────────────
//  Draw on-screen keyboard
// ─────────────────────────────────────────────
static void draw_keyboard() {
    auto draw_key = [&](int x, int y, int w, const char* label) {
        gfx->fillRect(x + KB_PADDING, y + KB_PADDING,
                      w - 2 * KB_PADDING, KB_KEY_H - 2 * KB_PADDING, 0x3208);
        gfx->drawRect(x + KB_PADDING, y + KB_PADDING,
                      w - 2 * KB_PADDING, KB_KEY_H - 2 * KB_PADDING, 0x8410);
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        int tw = strlen(label) * 6;
        gfx->setCursor(x + (w - tw) / 2, y + KB_PADDING + 10);
        gfx->print(label);
    };

    // Row 1: QWERTYUIOP (10 keys, centered)
    int row1_w = 10 * KB_KEY_W;
    int row1_x = (240 - row1_w) / 2;
    for (int i = 0; i < 10; i++) {
        char label[2] = { kb_row1[i], 0 };
        draw_key(row1_x + i * KB_KEY_W, KB_TOP, KB_KEY_W, label);
    }

    // Row 2: ASDFGHJKL (9 keys, centered)
    int row2_w = 9 * KB_KEY_W;
    int row2_x = (240 - row2_w) / 2;
    for (int i = 0; i < 9; i++) {
        char label[2] = { kb_row2[i], 0 };
        draw_key(row2_x + i * KB_KEY_W, KB_TOP + KB_KEY_H, KB_KEY_W, label);
    }

    // Row 3: ZXCVBNM + Backspace
    int row3_w = 7 * KB_KEY_W;
    int row3_x = (240 - row3_w - 32) / 2;
    for (int i = 0; i < 7; i++) {
        char label[2] = { kb_row3[i], 0 };
        draw_key(row3_x + i * KB_KEY_W, KB_TOP + 2 * KB_KEY_H, KB_KEY_W, label);
    }
    draw_key(row3_x + 7 * KB_KEY_W, KB_TOP + 2 * KB_KEY_H, 32, "BSP");

    // Row 4: BACK | SPACE | SEND
    int row4_y = KB_TOP + 3 * KB_KEY_H;
    draw_key(4,   row4_y, 50, "BACK");
    draw_key(56,  row4_y, 128, "SPACE");
    draw_key(186, row4_y, 50, "SEND");
}

// ─────────────────────────────────────────────
//  Hit-test the keyboard, returns the character pressed or:
//    0    nothing
//    1    backspace
//    2    space
//    3    send
//    4    back-to-launcher
// ─────────────────────────────────────────────
static int hit_keyboard(int16_t tx, int16_t ty) {
    if (ty < KB_TOP) return 0;

    int row = (ty - KB_TOP) / KB_KEY_H;
    if (row == 0) {
        int row1_x = (240 - 10 * KB_KEY_W) / 2;
        int col = (tx - row1_x) / KB_KEY_W;
        if (col >= 0 && col < 10) return kb_row1[col];
    } else if (row == 1) {
        int row2_x = (240 - 9 * KB_KEY_W) / 2;
        int col = (tx - row2_x) / KB_KEY_W;
        if (col >= 0 && col < 9) return kb_row2[col];
    } else if (row == 2) {
        int row3_x = (240 - 7 * KB_KEY_W - 32) / 2;
        int col = (tx - row3_x) / KB_KEY_W;
        if (col >= 0 && col < 7) return kb_row3[col];
        if (tx >= row3_x + 7 * KB_KEY_W && tx < row3_x + 7 * KB_KEY_W + 32)
            return 1;   // backspace
    } else if (row == 3) {
        if (tx < 56) return 4;    // BACK
        if (tx < 184) return 2;   // SPACE
        return 3;                  // SEND
    }
    return 0;
}

// ─────────────────────────────────────────────
//  PUBLIC ENTRY
// ─────────────────────────────────────────────
void c28p_run_ai_terminal() {
    Serial.println("[C28P-AI] Terminal starting");

    if (WiFi.status() != WL_CONNECTED) {
        // No network — show offline message
        gfx->fillScreen(0x0000);
        gfx->setTextSize(2);
        gfx->setTextColor(0xF800);
        gfx->setCursor(20, 100);
        gfx->print("AI OFFLINE");
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(20, 140);
        gfx->print("Connect WiFi in TOOLS");
        gfx->setCursor(20, 156);
        gfx->print("to use the AI terminal.");
        gfx->setTextColor(0x8410);
        gfx->setCursor(40, 250);
        gfx->print("Tap to return");

        bool was_touched = false;
        while (true) {
            int16_t tx, ty;
            bool touched = c28p_touch_read(&tx, &ty);
            if (touched && !was_touched) {
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                return;
            }
            was_touched = touched;
            delay(20);
            yield();
        }
    }

    init_gemini();
    if (!gemini_has_key()) {
        gfx->fillScreen(0x0000);
        gfx->setTextSize(2);
        gfx->setTextColor(0xFC00);
        gfx->setCursor(20, 100);
        gfx->print("NO API KEY");
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(8, 140);
        gfx->print("Set GEMINI_API_KEY in");
        gfx->setCursor(8, 156);
        gfx->print("secrets.h, or via WiFi");
        gfx->setCursor(8, 172);
        gfx->print("config portal.");
        gfx->setTextColor(0x8410);
        gfx->setCursor(40, 280);
        gfx->print("Tap to return");

        bool was_touched = false;
        while (true) {
            int16_t tx, ty;
            bool touched = c28p_touch_read(&tx, &ty);
            if (touched && !was_touched) {
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                return;
            }
            was_touched = touched;
            delay(20);
            yield();
        }
    }

    input_buf = "";
    last_response = "Ask me anything. Tap SEND when ready.";

    draw_ai_chrome();
    draw_response(last_response);
    draw_input();
    draw_keyboard();

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            int k = hit_keyboard(tx, ty);
            if (k == 0) {
                // ignore
            } else if (k == 4) {
                // BACK to launcher
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                return;
            } else if (k == 3) {
                // SEND
                if (input_buf.length() > 0) {
                    gfx->fillRect(0, 32, 240, 160, 0x0000);
                    gfx->setTextColor(0xFFE0);
                    gfx->setCursor(4, 38);
                    gfx->print("Thinking...");
                    String response = ask_gemini(input_buf);
                    last_response = response;
                    input_buf = "";
                    draw_response(last_response);
                    draw_input();
                }
            } else if (k == 2) {
                // SPACE
                input_buf += ' ';
                draw_input();
            } else if (k == 1) {
                // BACKSPACE
                if (input_buf.length() > 0) {
                    input_buf.remove(input_buf.length() - 1);
                    draw_input();
                }
            } else if (k >= 'A' && k <= 'Z') {
                // Letter (lowercase for prompt)
                input_buf += (char)(k + 32);
                draw_input();
            }
        }
        was_touched = touched;
        delay(40);   // slightly slower poll for keyboard debounce
        yield();
    }
}

#endif // DEVICE_C28P