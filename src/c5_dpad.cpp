// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c5_dpad.cpp — Virtual D-pad for NM-CYD-C5 (XPT2046 resistive)
//
//  Mirrors c28p_dpad.cpp's visual layout and selective-redraw model
//  with one structural difference:
//
//    SINGLE-TOUCH ONLY. The XPT2046 is a resistive ADC controller —
//    it reports the centroid of every active touch as one point.
//    Press two fingers at once and you get a position between them,
//    not two distinct points. There is no `c5_touch_read_multi()`
//    counterpart to `c28p_touch_read_multi()`.
//
//    Implication: simultaneous direction + action presses don't
//    register as both — they register as a single phantom touch
//    somewhere between the two fingers, in the dead space between
//    the D-pad and the A/B cluster, which hits neither.
//
//    Games that depend on simultaneous input (Mario jump-while-
//    running, Galaga fire-while-moving) degrade to sequential
//    interaction on the C5. Games that use one input at a time
//    (Tetris, Snake, Pac-Man, Breakout, Frogger, etc.) behave
//    identically to the C28P.
//
//  Layout — identical numeric values to c28p_dpad.cpp so games can
//  share viewport/dpad-area constants.
//
//  Touch driver — c5_touch_read() lives in c5_boot.cpp and returns
//  coordinates already mapped to 240×320 portrait space. The dpad
//  bounds checks below assume that coordinate system.
// ─────────────────────────────────────────────

#ifdef DEVICE_C5

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "c5_dpad.h"
#include "game_input.h"

extern Arduino_GFX *gfx;

// XPT2046 touch driver — declared in c5_boot.cpp.
// Returns a single (x,y) point in 240×320 portrait coords, or false
// if no stable touch is detected (two-read stability gate rejects
// ADC noise on untouched panels).
extern bool c5_touch_read(int16_t* x, int16_t* y);

// ─────────────────────────────────────────────
//  Layout — identical to c28p_dpad.cpp.
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

// SELECT / song-cycle — small note glyph between the D-pad and A/B.
static ButtonRect rect_select = { 116, 248, 26, 34 };

// ─────────────────────────────────────────────
//  Colors — matched to c28p_dpad.cpp.
// ─────────────────────────────────────────────
#define DPAD_BG_IDLE        0x18C3
#define DPAD_BG_PRESSED     0x39E7
#define DPAD_OUTLINE        0x4208
#define DPAD_ARROW_IDLE     0x07FF
#define DPAD_ARROW_PRESSED  0x0000
#define BTN_A_IDLE          0xF800
#define BTN_A_PRESSED       0xFFFF
#define BTN_B_IDLE          0x07E0
#define BTN_B_PRESSED       0xFFFF
#define BTN_SEL_IDLE        0xFD20
#define BTN_SEL_PRESSED     0xFFFF
#define DPAD_DIVIDER        0x2104

// ─────────────────────────────────────────────
//  Pressed-state tracking — for selective redraw.
// ─────────────────────────────────────────────
static bool was_up = false, was_down = false, was_left = false, was_right = false;
static bool was_a  = false, was_b    = false, was_select = false;
static bool rendered_once = false;

// ─────────────────────────────────────────────
//  Arrow glyph helpers — copied verbatim from c28p_dpad.cpp.
// ─────────────────────────────────────────────
static void draw_arrow_up(int16_t cx, int16_t cy, uint16_t color) {
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
    int text_w = (int)strlen(label) * 18;
    gfx->setCursor(r.x + (r.w - text_w) / 2, r.y + (r.h - 21) / 2);
    gfx->print(label);
}

static void draw_note_glyph(int16_t cx, int16_t cy, uint16_t color) {
    gfx->fillCircle(cx - 3, cy + 6, 4, color);
    gfx->fillRect(cx + 1, cy - 8, 2, 15, color);
    gfx->fillTriangle(cx + 3, cy - 8, cx + 3, cy - 1,
                      cx + 8, cy - 5, color);
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
void c5_dpad_render() {
    gfx->fillRect(0, C5_DPAD_AREA_Y, 240, C5_DPAD_AREA_H, 0x0000);

    gfx->drawFastHLine(0, C5_DPAD_AREA_Y,     240, DPAD_DIVIDER);
    gfx->drawFastHLine(0, C5_DPAD_AREA_Y + 1, 240, DPAD_DIVIDER);

    draw_dpad_button(rect_up,    false, draw_arrow_up);
    draw_dpad_button(rect_down,  false, draw_arrow_down);
    draw_dpad_button(rect_left,  false, draw_arrow_left);
    draw_dpad_button(rect_right, false, draw_arrow_right);

    draw_action_button(rect_a, "A", false, BTN_A_IDLE);
    draw_action_button(rect_b, "B", false, BTN_B_IDLE);

    draw_select_button(rect_select, false);

    was_up = was_down = was_left = was_right = false;
    was_a = was_b = was_select = false;
    rendered_once = true;
}

// ─────────────────────────────────────────────
//  Hit-testing helpers
// ─────────────────────────────────────────────
static bool in_rect(int16_t x, int16_t y, const ButtonRect& r) {
    return (x >= r.x) && (x < r.x + r.w) &&
           (y >= r.y) && (y < r.y + r.h);
}

bool c5_dpad_touched() {
    int16_t x, y;
    return c5_touch_read(&x, &y);
}

// ─────────────────────────────────────────────
//  Poll — populate PMNesInput from touch state.
//
//  Single-point hit-testing (XPT2046 resistive). Quadrant-from-
//  center mapping over the d-pad's bounding box (same logic as
//  c28p_dpad.cpp) — covers the whole d-pad continuously, with a
//  dom/2 threshold that lets near-cardinal touches stay pure and
//  only engages diagonals on clearly-angular presses.
//
//  Smoothing layer — single point doesn't have the FT6336G's
//  per-finger slot-reshuffle dropouts, but it does have brief
//  driver dropouts during the XPT2046's two-read stability gate
//  (when the second read disagrees with the first by >20 ADC
//  counts, c5_touch_read() returns false to reject noise). A
//  light grace window (40 ms while touching, 20 ms after
//  release) keeps a single tap from flickering across two poll
//  cycles when the stability gate vetoes one reading mid-hold.
//
//  Latch model — DIRECTION is a unit bitmask (one of 4 cardinals
//  + 4 diagonals) that REPLACES on every direction-active frame.
//  A finger sliding from LEFT to RIGHT releases LEFT atomically
//  when it reaches RIGHT — opposite directions never both assert.
//  A / B / SELECT have independent latches so releasing one
//  doesn't drag the others with it.
// ─────────────────────────────────────────────
bool c5_dpad_poll(PMNesInput* input) {
    if (!rendered_once) c5_dpad_render();

    int16_t tx, ty;
    bool touching = c5_touch_read(&tx, &ty);

    bool now_up    = false, now_down  = false;
    bool now_left  = false, now_right = false;
    bool now_a     = false, now_b     = false;
    bool now_select = false;

    const int dpad_x_min = rect_left.x;
    const int dpad_x_max = rect_right.x + rect_right.w;
    const int dpad_y_min = rect_up.y;
    const int dpad_y_max = rect_down.y + rect_down.h;
    const int dpad_cx    = (dpad_x_min + dpad_x_max) / 2;
    const int dpad_cy    = (dpad_y_min + dpad_y_max) / 2;

    if (touching && ty >= C5_DPAD_AREA_Y) {
        // Action buttons — strict rectangle hit-test.
        if (in_rect(tx, ty, rect_a))      now_a      = true;
        if (in_rect(tx, ty, rect_b))      now_b      = true;
        if (in_rect(tx, ty, rect_select)) now_select = true;

        // D-pad: quadrant-from-center with dom/2 diagonal gate.
        if (tx >= dpad_x_min && tx < dpad_x_max &&
            ty >= dpad_y_min && ty < dpad_y_max) {
            int dx  = tx - dpad_cx;
            int dy  = ty - dpad_cy;
            int adx = (dx < 0) ? -dx : dx;
            int ady = (dy < 0) ? -dy : dy;
            int dom = (adx > ady) ? adx : ady;
            if (dom > 6) {
                int threshold = dom / 2;
                if (adx >= threshold) {
                    if (dx < 0) now_left  = true;
                    else        now_right = true;
                }
                if (ady >= threshold) {
                    if (dy < 0) now_up    = true;
                    else        now_down  = true;
                }
            }
        }
    }

    // ── Smoothing layer ───────────────────────────────────────
    // Lighter than the C28P's (no multi-finger reshuffle to mask)
    // but still useful: bridges the XPT2046's stability-gate
    // rejections so a single tap doesn't flicker visually or
    // briefly drop input mid-hold.
    static uint32_t dir_last_ms    = 0;
    static uint8_t  dir_last_mask  = 0;
    static uint32_t a_last_ms      = 0;
    static uint32_t b_last_ms      = 0;
    static uint32_t select_last_ms = 0;

    constexpr uint32_t LATCH_MS         = 40;   // while touching
    constexpr uint32_t RELEASE_GRACE_MS = 20;   // after release

    const uint32_t now_ms = millis();
    const uint32_t grace  = touching ? LATCH_MS : RELEASE_GRACE_MS;

    const bool any_dir_now = now_up || now_down || now_left || now_right;
    if (any_dir_now) {
        dir_last_mask = (now_up    ? 0x01 : 0)
                      | (now_down  ? 0x02 : 0)
                      | (now_left  ? 0x04 : 0)
                      | (now_right ? 0x08 : 0);
        dir_last_ms = now_ms;
    } else if (dir_last_mask != 0 && (now_ms - dir_last_ms) < grace) {
        now_up    = (dir_last_mask & 0x01) != 0;
        now_down  = (dir_last_mask & 0x02) != 0;
        now_left  = (dir_last_mask & 0x04) != 0;
        now_right = (dir_last_mask & 0x08) != 0;
    } else {
        dir_last_mask = 0;
    }

    auto smooth = [&](bool& flag, uint32_t& last_ms) {
        if (flag) {
            last_ms = now_ms;
        } else if (last_ms != 0 && (now_ms - last_ms) < grace) {
            flag = true;
        } else {
            last_ms = 0;
        }
    };
    smooth(now_a,      a_last_ms);
    smooth(now_b,      b_last_ms);
    smooth(now_select, select_last_ms);

    // OR touch state into the existing input.
    input->up     = input->up     || now_up;
    input->down   = input->down   || now_down;
    input->left   = input->left   || now_left;
    input->right  = input->right  || now_right;
    input->a      = input->a      || now_a;
    input->b      = input->b      || now_b;
    input->select = input->select || now_select;

    // Selective redraw — only buttons whose state changed.
    if (now_up     != was_up)     draw_dpad_button(rect_up,    now_up,    draw_arrow_up);
    if (now_down   != was_down)   draw_dpad_button(rect_down,  now_down,  draw_arrow_down);
    if (now_left   != was_left)   draw_dpad_button(rect_left,  now_left,  draw_arrow_left);
    if (now_right  != was_right)  draw_dpad_button(rect_right, now_right, draw_arrow_right);
    if (now_a      != was_a)      draw_action_button(rect_a,   "A", now_a, BTN_A_IDLE);
    if (now_b      != was_b)      draw_action_button(rect_b,   "B", now_b, BTN_B_IDLE);
    if (now_select != was_select) draw_select_button(rect_select, now_select);

    was_up     = now_up;     was_down  = now_down;
    was_left   = now_left;   was_right = now_right;
    was_a      = now_a;      was_b     = now_b;
    was_select = now_select;

    return now_up || now_down || now_left || now_right || now_a || now_b || now_select;
}

#endif // DEVICE_C5
