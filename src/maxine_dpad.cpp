// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  maxine_dpad.cpp — Virtual D-pad implementation (480x800)
//
//  Direct scale-up of c28p_dpad.cpp for Maxine's larger panel.
//  Same two-pass render model (chrome once, then selective
//  per-button redraw). Same behavior; larger geometry sized for
//  the bottom 280px of the 480x800 portrait screen.
//
//  Touch is polled via the GT911 driver in maxine_boot.cpp
//  (maxine_touch_read). Single-touch only for now — same
//  limitation/justification as the C28P dpad.
// ─────────────────────────────────────────────

#ifdef DEVICE_MAXINE

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "maxine_dpad.h"
#include "game_input.h"

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  Touch driver — declared in maxine_boot.cpp.
//  Single-touch polling; returns true if a finger is on screen
//  and writes display coords (0..479, 0..799) to *x, *y.
// ─────────────────────────────────────────────
extern bool maxine_touch_read(int16_t* x, int16_t* y);

// ─────────────────────────────────────────────
//  Layout — bottom 280px control strip (y = 520..800).
//  Roughly 2x the C28P rects, re-centered for the taller panel.
//  D-pad cluster centered around (120, 660); action buttons in
//  the right half.
// ─────────────────────────────────────────────
struct ButtonRect {
    int16_t x, y, w, h;
};

// D-pad arrows (each ~76x60). Cluster centered near (120, 660).
static ButtonRect rect_up    = {  84, 560, 72, 60 };
static ButtonRect rect_down  = {  84, 684, 72, 60 };
static ButtonRect rect_left  = {  12, 622, 60, 72 };
static ButtonRect rect_right = { 168, 622, 60, 72 };

// Action buttons (each ~128x80). Right half of the control strip.
static ButtonRect rect_a     = { 290, 568, 128, 80 };
static ButtonRect rect_b     = { 290, 664, 128, 80 };

// ─────────────────────────────────────────────
//  Colors — match the C28P dpad / launcher aesthetic.
// ─────────────────────────────────────────────
#define DPAD_BG_IDLE        0x18C3    // very dark gray
#define DPAD_BG_PRESSED     0x39E7    // mid gray
#define DPAD_OUTLINE        0x4208    // outline gray
#define DPAD_ARROW_IDLE     0x07FF    // cyan
#define DPAD_ARROW_PRESSED  0x0000    // black on bright bg
#define BTN_A_IDLE          0xF800    // red
#define BTN_A_PRESSED       0xFFFF    // white
#define BTN_B_IDLE          0x07E0    // green
#define BTN_B_PRESSED       0xFFFF    // white
#define DPAD_DIVIDER        0x2104    // very dark gray divider line
#define DPAD_LABEL          0xC618    // light gray text

// ─────────────────────────────────────────────
//  Pressed-state tracking — drives selective redraw.
// ─────────────────────────────────────────────
static bool was_up = false, was_down = false, was_left = false, was_right = false;
static bool was_a  = false, was_b    = false;
static bool rendered_once = false;

// ─────────────────────────────────────────────
//  Drawing helpers — arrows scaled ~2x vs the C28P (28px wide /
//  22px tall triangles).
// ─────────────────────────────────────────────

static void draw_arrow_up(int16_t cx, int16_t cy, uint16_t color) {
    for (int dy = 0; dy < 22; dy++) {
        int half_w = 14 - (dy * 14) / 22;
        gfx->drawFastHLine(cx - half_w, cy - 10 + dy, half_w * 2 + 1, color);
    }
}

static void draw_arrow_down(int16_t cx, int16_t cy, uint16_t color) {
    for (int dy = 0; dy < 22; dy++) {
        int half_w = (dy * 14) / 22;
        gfx->drawFastHLine(cx - half_w, cy - 10 + dy, half_w * 2 + 1, color);
    }
}

static void draw_arrow_left(int16_t cx, int16_t cy, uint16_t color) {
    for (int dx = 0; dx < 22; dx++) {
        int half_h = 14 - (dx * 14) / 22;
        gfx->drawFastVLine(cx - 10 + dx, cy - half_h, half_h * 2 + 1, color);
    }
}

static void draw_arrow_right(int16_t cx, int16_t cy, uint16_t color) {
    for (int dx = 0; dx < 22; dx++) {
        int half_h = (dx * 14) / 22;
        gfx->drawFastVLine(cx - 10 + dx, cy - half_h, half_h * 2 + 1, color);
    }
}

static void draw_dpad_button(const ButtonRect& r, bool pressed,
                              void (*draw_arrow)(int16_t, int16_t, uint16_t)) {
    uint16_t bg     = pressed ? DPAD_BG_PRESSED    : DPAD_BG_IDLE;
    uint16_t arrow  = pressed ? DPAD_ARROW_PRESSED : DPAD_ARROW_IDLE;
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 8, DPAD_OUTLINE);
    draw_arrow(r.x + r.w / 2, r.y + r.h / 2, arrow);
}

static void draw_action_button(const ButtonRect& r, const char* label,
                                bool pressed, uint16_t idle_color) {
    uint16_t bg = pressed ? BTN_A_PRESSED : idle_color;
    uint16_t fg = pressed ? 0x0000 : 0xFFFF;
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 14, bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 14, DPAD_OUTLINE);
    gfx->setTextSize(6);
    gfx->setTextColor(fg);
    int text_w = (int)strlen(label) * 36;   // size-6 ~36px/char
    gfx->setCursor(r.x + (r.w - text_w) / 2, r.y + (r.h - 42) / 2);
    gfx->print(label);
}

// ─────────────────────────────────────────────
//  Render — initial draw of all chrome.
// ─────────────────────────────────────────────
void maxine_dpad_render() {
    // Clear the control area without touching the game viewport.
    gfx->fillRect(0, MAXINE_DPAD_AREA_Y, MAXINE_GAME_VIEW_W, MAXINE_DPAD_AREA_H, 0x0000);

    // Dividing line between game and controls (3px for the big panel).
    gfx->drawFastHLine(0, MAXINE_DPAD_AREA_Y,     MAXINE_GAME_VIEW_W, DPAD_DIVIDER);
    gfx->drawFastHLine(0, MAXINE_DPAD_AREA_Y + 1, MAXINE_GAME_VIEW_W, DPAD_DIVIDER);
    gfx->drawFastHLine(0, MAXINE_DPAD_AREA_Y + 2, MAXINE_GAME_VIEW_W, DPAD_DIVIDER);

    draw_dpad_button(rect_up,    false, draw_arrow_up);
    draw_dpad_button(rect_down,  false, draw_arrow_down);
    draw_dpad_button(rect_left,  false, draw_arrow_left);
    draw_dpad_button(rect_right, false, draw_arrow_right);

    draw_action_button(rect_a, "A", false, BTN_A_IDLE);
    draw_action_button(rect_b, "B", false, BTN_B_IDLE);

    was_up = was_down = was_left = was_right = false;
    was_a = was_b = false;
    rendered_once = true;
}

// ─────────────────────────────────────────────
//  Hit-testing
// ─────────────────────────────────────────────

static bool in_rect(int16_t x, int16_t y, const ButtonRect& r) {
    return (x >= r.x) && (x < r.x + r.w) &&
           (y >= r.y) && (y < r.y + r.h);
}

bool maxine_dpad_touched() {
    int16_t x, y;
    return maxine_touch_read(&x, &y);
}

// ─────────────────────────────────────────────
//  Poll — populate PMNesInput from touch state.
// ─────────────────────────────────────────────
bool maxine_dpad_poll(PMNesInput* input) {
    if (!rendered_once) maxine_dpad_render();

    int16_t tx, ty;
    bool touched = maxine_touch_read(&tx, &ty);

    bool now_up    = false, now_down  = false;
    bool now_left  = false, now_right = false;
    bool now_a     = false, now_b     = false;

    if (touched) {
        // Only test buttons in the control area — touches in the
        // game viewport are ignored by the D-pad.
        if (ty >= MAXINE_DPAD_AREA_Y) {
            if (in_rect(tx, ty, rect_up))    now_up    = true;
            if (in_rect(tx, ty, rect_down))  now_down  = true;
            if (in_rect(tx, ty, rect_left))  now_left  = true;
            if (in_rect(tx, ty, rect_right)) now_right = true;
            if (in_rect(tx, ty, rect_a))     now_a     = true;
            if (in_rect(tx, ty, rect_b))     now_b     = true;
        }
    }

    input->up    = input->up    || now_up;
    input->down  = input->down  || now_down;
    input->left  = input->left  || now_left;
    input->right = input->right || now_right;
    input->a     = input->a     || now_a;
    input->b     = input->b     || now_b;

    // Selective redraw — only buttons whose state changed.
    if (now_up    != was_up)    draw_dpad_button(rect_up,    now_up,    draw_arrow_up);
    if (now_down  != was_down)  draw_dpad_button(rect_down,  now_down,  draw_arrow_down);
    if (now_left  != was_left)  draw_dpad_button(rect_left,  now_left,  draw_arrow_left);
    if (now_right != was_right) draw_dpad_button(rect_right, now_right, draw_arrow_right);
    if (now_a     != was_a)     draw_action_button(rect_a,   "A", now_a, BTN_A_IDLE);
    if (now_b     != was_b)     draw_action_button(rect_b,   "B", now_b, BTN_B_IDLE);

    was_up    = now_up;    was_down  = now_down;
    was_left  = now_left;  was_right = now_right;
    was_a     = now_a;     was_b     = now_b;

    return now_up || now_down || now_left || now_right || now_a || now_b;
}

#endif // DEVICE_MAXINE
