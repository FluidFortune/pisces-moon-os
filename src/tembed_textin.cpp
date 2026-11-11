// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// tembed_textin.cpp — encoder-only text entry for the T-Embed
//
// A small on-screen keyboard: three rows of up to ten characters plus a
// control row [layer] [SPACE] [DEL] [DONE]. Turning the encoder walks the
// highlight through every key in reading order (wrapping), push types the
// highlighted key. The side key is a quick backspace; holding it for
// 0.8 s cancels. Four layers: abc, ABC, 123, and extra symbols, which
// together cover every printable ASCII character a WiFi password can use.

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <string.h>
#include "tembed_ui.h"
#include "tembed_input.h"

using namespace tui;

namespace {

constexpr int kLayers = 4;
const char* const kLayerRows[kLayers][3] = {
    { "abcdefghij", "klmnopqrst", "uvwxyz.-_@" },
    { "ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ.-_@" },
    { "1234567890", "!@#$%^&*()", "-_=+[]{};:" },
    { "'\",.<>/?\\|", "`~",        ""           },
};
const char* const kLayerName[kLayers] = { "abc", "ABC", "123", "#+=" };

enum Ctl { CTL_LAYER = 0, CTL_SPACE, CTL_DEL, CTL_DONE, CTL_COUNT };
const char* const kCtlLabel[CTL_COUNT] = { nullptr, "SPACE", "DEL", "DONE" };

// Geometry (320x170).
constexpr int KEY_W   = 32;
constexpr int KEY_H   = 20;
constexpr int KEYS_Y  = 66;          // first character row
constexpr int CTL_Y   = KEYS_Y + 3 * KEY_H + 4;
constexpr int CTL_W   = 80;
constexpr int CTL_H   = 22;
constexpr int FIELD_Y = 38;

int  g_layer = 0;
int  g_pos   = 0;                    // highlighted key index

int rowLen(int r)   { return (int)strlen(kLayerRows[g_layer][r]); }
int charKeys()      { return rowLen(0) + rowLen(1) + rowLen(2); }
int totalKeys()     { return charKeys() + CTL_COUNT; }

// Map a linear key index to (row, col) for char keys, or -1 for controls.
bool keyRC(int idx, int& r, int& c) {
    for (r = 0; r < 3; r++) {
        int n = rowLen(r);
        if (idx < n) { c = idx; return true; }
        idx -= n;
    }
    c = idx;            // control index
    return false;
}

void drawKey(int idx, bool hl) {
    int r, c;
    if (keyRC(idx, r, c)) {
        int x = c * KEY_W, y = KEYS_Y + r * KEY_H;
        gfx->fillRect(x, y, KEY_W, KEY_H, hl ? SEL : BG);
        gfx->drawRect(x, y, KEY_W, KEY_H, DIM);
        gfx->setTextSize(2);
        gfx->setTextColor(hl ? TXT : HI);
        gfx->setCursor(x + 10, y + 3);
        gfx->print(kLayerRows[g_layer][r][c]);
    } else {
        int x = c * CTL_W, y = CTL_Y;
        gfx->fillRect(x, y, CTL_W, CTL_H, hl ? SEL : BG);
        gfx->drawRect(x, y, CTL_W, CTL_H, DIM);
        gfx->setTextSize(2);
        gfx->setTextColor(hl ? TXT : WARN);
        const char* lbl = (c == CTL_LAYER) ? kLayerName[(g_layer + 1) % kLayers] : kCtlLabel[c];
        int tw = (int)strlen(lbl) * 12;
        gfx->setCursor(x + (CTL_W - tw) / 2, y + 4);
        gfx->print(lbl);
    }
}

void drawKeyboard() {
    gfx->fillRect(0, KEYS_Y, W, CTL_Y + CTL_H - KEYS_Y + 1, BG);
    for (int i = 0; i < totalKeys(); i++) drawKey(i, i == g_pos);
}

void drawField(const char* buf) {
    gfx->fillRect(0, FIELD_Y - 2, W, 24, BG);
    gfx->drawRect(2, FIELD_Y - 2, W - 4, 22, DIM);
    gfx->setTextSize(2);
    gfx->setTextColor(TXT);
    size_t len = strlen(buf);
    const size_t kShow = 24;                 // 24 * 12 px = 288 px
    const char* start = len > kShow ? buf + (len - kShow) : buf;
    gfx->setCursor(8, FIELD_Y + 1);
    gfx->print(start);
    gfx->print("_");
    gfx->setTextSize(1);
    gfx->setTextColor(DIM);
    gfx->fillRect(W - 40, 28, 40, 9, BG);
    gfx->setCursor(W - 36, 29);
    gfx->printf("%u", (unsigned)len);
}

} // namespace

bool tembed_text_input(const char* title, const char* subtitle,
                       char* out, size_t outlen, const char* initial) {
    if (!out || outlen < 2) return false;
    char buf[96] = {0};
    size_t cap = outlen - 1 < sizeof(buf) - 1 ? outlen - 1 : sizeof(buf) - 1;
    if (initial) { strncpy(buf, initial, cap); buf[cap] = 0; }

    header(title);
    if (subtitle) {
        gfx->setTextSize(1);
        gfx->setTextColor(SUBT);
        gfx->setCursor(6, 28);
        gfx->print(subtitle);
    }
    g_layer = 0;
    g_pos   = 0;
    drawField(buf);
    drawKeyboard();
    footer("TURN=move  PUSH=type  SIDE=del  HOLD SIDE=cancel");
    tembed_input_flush();

    bool     backWasDown = false;
    uint32_t backDownAt  = 0;

    for (;;) {
        int d = tembed_encoder_delta();
        if (d != 0) {
            int n = totalKeys();
            int prev = g_pos;
            g_pos = ((g_pos + d) % n + n) % n;
            drawKey(prev, false);
            drawKey(g_pos, true);
        }

        if (tembed_btn_select()) {
            int r, c;
            size_t len = strlen(buf);
            if (keyRC(g_pos, r, c)) {
                if (len < cap) { buf[len] = kLayerRows[g_layer][r][c]; buf[len + 1] = 0; }
                drawField(buf);
            } else if (c == CTL_LAYER) {
                g_layer = (g_layer + 1) % kLayers;
                // Layers differ in key count; keep the highlight on the
                // layer key so repeated pushes keep cycling.
                g_pos = charKeys() + CTL_LAYER;
                drawKeyboard();
            } else if (c == CTL_SPACE) {
                if (len < cap) { buf[len] = ' '; buf[len + 1] = 0; }
                drawField(buf);
            } else if (c == CTL_DEL) {
                if (len) buf[len - 1] = 0;
                drawField(buf);
            } else if (c == CTL_DONE) {
                strncpy(out, buf, outlen - 1);
                out[outlen - 1] = 0;
                tembed_input_flush();
                return true;
            }
        }

        // Side key: tap = backspace, hold = cancel.
        bool backDown = tembed_btn_back_down();
        if (backDown && !backWasDown) backDownAt = millis();
        if (backDown && millis() - backDownAt > 800) {
            gfx->fillRect(0, FIELD_Y - 2, W, 24, BG);
            gfx->setTextSize(2); gfx->setTextColor(WARN);
            gfx->setCursor(8, FIELD_Y + 1); gfx->print("cancelled");
            while (tembed_btn_back_down()) delay(5);
            tembed_input_flush();
            return false;
        }
        if (!backDown && backWasDown && millis() - backDownAt > 25) {
            size_t len = strlen(buf);
            if (len) { buf[len - 1] = 0; drawField(buf); }
        }
        backWasDown = backDown;
        delay(3);
    }
}

#endif // DEVICE_TEMBED_CC1101
