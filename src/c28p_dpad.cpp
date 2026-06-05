// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_dpad.cpp — Virtual D-pad implementation
//
//  Renders a NES-style controller layout in the bottom 120px of
//  the C28P's 240x320 portrait screen. The render is two-pass:
//    1. Initial draw: chrome (dividing line, D-pad arrows in idle
//       state, A/B buttons in idle state).
//    2. Per-frame updates: only buttons that changed state are
//       redrawn, to keep the audio + game loop responsive.
//
//  Touch is polled via the FT6336G driver in c28p_boot.cpp.
//  c28p_dpad_poll() reads BOTH touch points the FT6336G reports
//  (via c28p_touch_read_multi), so holding a direction while
//  pressing A/B registers as both buttons at once — required for
//  Mario (jump while running) and Galaga (fire while moving). The
//  single-touch c28p_touch_read() is still used by menu screens
//  where only one press matters at a time.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "c28p_dpad.h"
#include "game_input.h"

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  Touch driver — declared in c28p_boot.cpp.
//  c28p_touch_read():       single point (menus).
//  c28p_touch_read_multi(): up to two points (gameplay D-pad).
// ─────────────────────────────────────────────
extern bool c28p_touch_read(int16_t* x, int16_t* y);
extern int  c28p_touch_read_multi(int16_t* xs, int16_t* ys);

// ─────────────────────────────────────────────
//  Layout — derived from C28P_GAME_VIEW_*/C28P_DPAD_AREA_* in
//  c28p_dpad.h, computed once at first render() call.
// ─────────────────────────────────────────────
struct ButtonRect {
    int16_t x, y, w, h;
};

// D-pad arrow rects (each ~38x38). Centered around (60, 260).
static ButtonRect rect_up    = {  42, 215, 36, 30 };
static ButtonRect rect_down  = {  42, 277, 36, 30 };
static ButtonRect rect_left  = {   6, 246, 30, 36 };
static ButtonRect rect_right = {  84, 246, 30, 36 };

// Action buttons (each ~52x40). Right half of control area.
static ButtonRect rect_a     = { 145, 220, 64, 40 };
static ButtonRect rect_b     = { 145, 268, 64, 40 };

// SELECT / song-cycle button — small note glyph in the gap between
// the D-pad and the A/B buttons. Maps to PMNesInput.select, which
// Tetris uses to cycle through its three songs. The C28P is touch-
// only (no keyboard), so without this button input.select could
// never be set and song selection was unreachable on this device.
static ButtonRect rect_select = { 116, 248, 26, 34 };

// ─────────────────────────────────────────────
//  Colors — chosen to match the launcher tile aesthetic:
//  dimmed when idle, fully saturated when pressed.
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
#define BTN_SEL_IDLE        0xFD20    // amber (song note, idle)
#define BTN_SEL_PRESSED     0xFFFF    // white (song note, pressed)
#define DPAD_DIVIDER        0x2104    // very dark gray divider line
#define DPAD_LABEL          0xC618    // light gray text

// ─────────────────────────────────────────────
//  Pressed-state tracking — drives selective redraw.
// ─────────────────────────────────────────────
static bool was_up = false, was_down = false, was_left = false, was_right = false;
static bool was_a  = false, was_b    = false, was_select = false;
static bool rendered_once = false;

// ─────────────────────────────────────────────
//  Drawing helpers
// ─────────────────────────────────────────────

static void draw_arrow_up(int16_t cx, int16_t cy, uint16_t color) {
    // Filled triangle pointing up — 14px wide × 11px tall
    for (int dy = 0; dy < 11; dy++) {
        int half_w = 7 - (dy * 7) / 11;
        gfx->drawFastHLine(cx - half_w, cy - 5 + dy, half_w * 2 + 1, color);
    }
}

static void draw_arrow_down(int16_t cx, int16_t cy, uint16_t color) {
    for (int dy = 0; dy < 11; dy++) {
        int half_w = (dy * 7) / 11;
        gfx->drawFastHLine(cx - half_w, cy - 5 + dy, half_w * 2 + 1, color);
    }
}

static void draw_arrow_left(int16_t cx, int16_t cy, uint16_t color) {
    for (int dx = 0; dx < 11; dx++) {
        int half_h = 7 - (dx * 7) / 11;
        gfx->drawFastVLine(cx - 5 + dx, cy - half_h, half_h * 2 + 1, color);
    }
}

static void draw_arrow_right(int16_t cx, int16_t cy, uint16_t color) {
    for (int dx = 0; dx < 11; dx++) {
        int half_h = (dx * 7) / 11;
        gfx->drawFastVLine(cx - 5 + dx, cy - half_h, half_h * 2 + 1, color);
    }
}

static void draw_dpad_button(const ButtonRect& r, bool pressed,
                              void (*draw_arrow)(int16_t, int16_t, uint16_t)) {
    uint16_t bg     = pressed ? DPAD_BG_PRESSED    : DPAD_BG_IDLE;
    uint16_t arrow  = pressed ? DPAD_ARROW_PRESSED : DPAD_ARROW_IDLE;
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 4, bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 4, DPAD_OUTLINE);
    draw_arrow(r.x + r.w / 2, r.y + r.h / 2, arrow);
}

static void draw_action_button(const ButtonRect& r, const char* label,
                                bool pressed, uint16_t idle_color) {
    uint16_t bg = pressed ? BTN_A_PRESSED : idle_color;
    uint16_t fg = pressed ? 0x0000 : 0xFFFF;
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 8, DPAD_OUTLINE);
    gfx->setTextSize(3);
    gfx->setTextColor(fg);
    int text_w = (int)strlen(label) * 18;   // size-3 ~18px/char
    gfx->setCursor(r.x + (r.w - text_w) / 2, r.y + (r.h - 21) / 2);
    gfx->print(label);
}

// Tiny eighth-note glyph for the SELECT/song button: filled note
// head + stem + flag. Drawn centered on (cx, cy).
static void draw_note_glyph(int16_t cx, int16_t cy, uint16_t color) {
    gfx->fillCircle(cx - 3, cy + 6, 4, color);            // note head
    gfx->fillRect(cx + 1, cy - 8, 2, 15, color);          // stem
    gfx->fillTriangle(cx + 3, cy - 8, cx + 3, cy - 1,
                      cx + 8, cy - 5, color);             // flag
}

static void draw_select_button(const ButtonRect& r, bool pressed) {
    uint16_t bg   = pressed ? BTN_SEL_PRESSED : DPAD_BG_IDLE;
    uint16_t note = pressed ? 0x0000 : BTN_SEL_IDLE;
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 6, bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 6, DPAD_OUTLINE);
    draw_note_glyph(r.x + r.w / 2, r.y + r.h / 2, note);
}

// ─────────────────────────────────────────────
//  Render — initial draw of all chrome.
// ─────────────────────────────────────────────
void c28p_dpad_render() {
    // Background fill — clear the control area without touching
    // the game viewport above.
    gfx->fillRect(0, C28P_DPAD_AREA_Y, 240, C28P_DPAD_AREA_H, 0x0000);

    // Subtle dividing line between game and controls
    gfx->drawFastHLine(0, C28P_DPAD_AREA_Y, 240, DPAD_DIVIDER);
    gfx->drawFastHLine(0, C28P_DPAD_AREA_Y + 1, 240, DPAD_DIVIDER);

    // D-pad in idle state
    draw_dpad_button(rect_up,    false, draw_arrow_up);
    draw_dpad_button(rect_down,  false, draw_arrow_down);
    draw_dpad_button(rect_left,  false, draw_arrow_left);
    draw_dpad_button(rect_right, false, draw_arrow_right);

    // Action buttons in idle state
    draw_action_button(rect_a, "A", false, BTN_A_IDLE);
    draw_action_button(rect_b, "B", false, BTN_B_IDLE);

    // SELECT / song-cycle button (note glyph)
    draw_select_button(rect_select, false);

    // Reset state tracking so the first poll() correctly detects
    // initial idle state for every button.
    was_up = was_down = was_left = was_right = false;
    was_a = was_b = was_select = false;
    rendered_once = true;
}

// ─────────────────────────────────────────────
//  Hit-testing
// ─────────────────────────────────────────────

static bool in_rect(int16_t x, int16_t y, const ButtonRect& r) {
    return (x >= r.x) && (x < r.x + r.w) &&
           (y >= r.y) && (y < r.y + r.h);
}

bool c28p_dpad_touched() {
    int16_t x, y;
    return c28p_touch_read(&x, &y);
}

// ─────────────────────────────────────────────
//  Poll — populate PMNesInput from touch state.
// ─────────────────────────────────────────────
bool c28p_dpad_poll(PMNesInput* input) {
    if (!rendered_once) c28p_dpad_render();

    int16_t txs[2], tys[2];
    int n = c28p_touch_read_multi(txs, tys);

    bool now_up    = false, now_down  = false;
    bool now_left  = false, now_right = false;
    bool now_a     = false, now_b     = false;
    bool now_select = false;

    // Hit-test EVERY active touch point and OR the results. This is
    // what lets a direction press and an A/B press register in the
    // same frame (two fingers) — the fix for Mario jump-while-running
    // and Galaga fire-while-moving. Touches in the game viewport
    // (above the control strip) are ignored by the D-pad.
    for (int i = 0; i < n; i++) {
        int16_t tx = txs[i], ty = tys[i];
        if (ty < C28P_DPAD_AREA_Y) continue;
        if (in_rect(tx, ty, rect_up))     now_up     = true;
        if (in_rect(tx, ty, rect_down))   now_down   = true;
        if (in_rect(tx, ty, rect_left))   now_left   = true;
        if (in_rect(tx, ty, rect_right))  now_right  = true;
        if (in_rect(tx, ty, rect_a))      now_a      = true;
        if (in_rect(tx, ty, rect_b))      now_b      = true;
        if (in_rect(tx, ty, rect_select)) now_select = true;
    }

    // OR the touch state into the input. This lets callers
    // combine D-pad with other sources (e.g. a future Bluetooth
    // gamepad attached to C28P over BLE).
    input->up    = input->up    || now_up;
    input->down  = input->down  || now_down;
    input->left  = input->left  || now_left;
    input->right = input->right || now_right;
    input->a     = input->a     || now_a;
    input->b     = input->b     || now_b;
    input->select = input->select || now_select;

    // Selective redraw — only buttons whose state changed.
    if (now_up    != was_up)    draw_dpad_button(rect_up,    now_up,    draw_arrow_up);
    if (now_down  != was_down)  draw_dpad_button(rect_down,  now_down,  draw_arrow_down);
    if (now_left  != was_left)  draw_dpad_button(rect_left,  now_left,  draw_arrow_left);
    if (now_right != was_right) draw_dpad_button(rect_right, now_right, draw_arrow_right);
    if (now_a     != was_a)     draw_action_button(rect_a,   "A", now_a, BTN_A_IDLE);
    if (now_b     != was_b)     draw_action_button(rect_b,   "B", now_b, BTN_B_IDLE);
    if (now_select != was_select) draw_select_button(rect_select, now_select);

    was_up    = now_up;    was_down  = now_down;
    was_left  = now_left;  was_right = now_right;
    was_a     = now_a;     was_b     = now_b;
    was_select = now_select;

    return now_up || now_down || now_left || now_right || now_a || now_b || now_select;
}

#endif // DEVICE_C28P