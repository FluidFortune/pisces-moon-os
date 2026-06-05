// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_calculator.cpp — Touch calculator for C28P + Maxine
//
//  Standard four-function calculator (+ − × ÷) with sign toggle
//  and percent. Numbers up to ~12 significant digits via double.
//
//  STATE MODEL:
//    display     - String of currently visible value
//    accumulator - the running left-hand operand
//    pending_op  - operator waiting for its right-hand operand:
//                  '+' '-' '*' '/' or 0 for "none"
//    after_op    - true if the next digit press should start a
//                  fresh display rather than appending to it
//
//  Tapping an operator while one is already pending applies the
//  current display to the accumulator first (chained ops like
//  "2 + 3 + 4 =" work the way you'd expect).
//
//  LAYOUT (C28P 240×320 portrait base):
//
//    y=  0..14   Exit bar
//    y= 16..36   Title bar ("CALCULATOR")
//    y= 40..120  Result display (right-aligned, fits 12 chars)
//    y=124..316  Keypad — 5 rows × 4 cols
//
//  Cell width = TI_W / 4. Row height ≈ 38 (C28P) / 76 (Maxine).
//  The 0 key in the bottom row spans 2 cells, matching iOS/Android
//  layouts so muscle memory translates.
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "pm_calculator.h"

extern Arduino_GFX *gfx;

#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
static inline bool _cl_touch(int16_t* x, int16_t* y) { return c28p_touch_read(x, y); }
static const int CL_W           = 240;
static const int CL_H           = 320;
static const int CL_EXIT_H      = 14;
static const int CL_TITLE_Y     = 16;
static const int CL_TITLE_H     = 22;
static const int CL_DISP_Y     = 42;
static const int CL_DISP_H     = 80;
static const int CL_KP_Y        = 126;
static const int CL_KP_ROWS     = 5;
static const int CL_KP_ROW_H    = 38;
static const int CL_TITLE_TS    = 2;
static const int CL_DISP_TS     = 4;
static const int CL_KEY_TS      = 2;
#else  // DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
static inline bool _cl_touch(int16_t* x, int16_t* y) { return maxine_touch_read(x, y); }
static const int CL_W           = 480;
static const int CL_H           = 800;
static const int CL_EXIT_H      = 32;
static const int CL_TITLE_Y     = 36;
static const int CL_TITLE_H     = 56;
static const int CL_DISP_Y      = 104;
static const int CL_DISP_H      = 160;
static const int CL_KP_Y        = 280;
static const int CL_KP_ROWS     = 5;
static const int CL_KP_ROW_H    = 100;
static const int CL_TITLE_TS    = 4;
static const int CL_DISP_TS     = 8;
static const int CL_KEY_TS      = 4;
#endif

static const int CL_CELL_W      = CL_W / 4;
static const int CL_KP_H        = CL_KP_ROWS * CL_KP_ROW_H;

// ─── Theme ───
static const uint16_t COL_BG     = 0x0000;
static const uint16_t COL_HEADER = 0x0841;
static const uint16_t COL_DISP   = 0x10A2;
static const uint16_t COL_NUM    = 0x18C3;
static const uint16_t COL_OP     = 0xFD20;   // amber for + − × ÷
static const uint16_t COL_FUNC   = 0x07FF;   // cyan for C / ± / %
static const uint16_t COL_EQ     = 0x07E0;   // green for =
static const uint16_t COL_TXT    = 0xFFFF;
static const uint16_t COL_DIM    = 0x4208;

// ─── Key model ───
//
// Each key has an action code and a label. Action codes 0..9 are
// digit insert. Special codes (>= 10) are operators / commands.
enum CkAction {
    CK_DIGIT_BASE = 0,    // 0..9 = digit
    CK_DOT  = 10,
    CK_CLR  = 11,
    CK_NEG  = 12,
    CK_PCT  = 13,
    CK_DIV  = 14,
    CK_MUL  = 15,
    CK_SUB  = 16,
    CK_ADD  = 17,
    CK_EQ   = 18,
};

struct CkKey {
    uint8_t action;
    const char* label;
    uint8_t  width;   // grid cells (default 1)
};

// 5 rows × 4 columns. The 0 key spans 2 cells in the bottom row.
static const CkKey ROW0[] = {
    {CK_CLR,"C",1}, {CK_NEG,"+/-",1}, {CK_PCT,"%",1}, {CK_DIV,"/",1},
};
static const CkKey ROW1[] = {
    {(uint8_t)(CK_DIGIT_BASE+7),"7",1},{(uint8_t)(CK_DIGIT_BASE+8),"8",1},
    {(uint8_t)(CK_DIGIT_BASE+9),"9",1},{CK_MUL,"x",1},
};
static const CkKey ROW2[] = {
    {(uint8_t)(CK_DIGIT_BASE+4),"4",1},{(uint8_t)(CK_DIGIT_BASE+5),"5",1},
    {(uint8_t)(CK_DIGIT_BASE+6),"6",1},{CK_SUB,"-",1},
};
static const CkKey ROW3[] = {
    {(uint8_t)(CK_DIGIT_BASE+1),"1",1},{(uint8_t)(CK_DIGIT_BASE+2),"2",1},
    {(uint8_t)(CK_DIGIT_BASE+3),"3",1},{CK_ADD,"+",1},
};
static const CkKey ROW4[] = {
    {(uint8_t)(CK_DIGIT_BASE+0),"0",2},{CK_DOT,".",1},{CK_EQ,"=",1},
};

static const CkKey* CL_ROWS[CL_KP_ROWS] = {ROW0, ROW1, ROW2, ROW3, ROW4};
static const uint8_t CL_ROW_COUNTS[CL_KP_ROWS] = {4, 4, 4, 4, 3};

// ─── Display formatter ───
//
// Returns a string representation of a double sized for the display.
// Trims trailing zeros and a hanging decimal point, but preserves
// "1.0e-7" style for very small / very large values via printf %g.
static String format_value(double v) {
    if (isnan(v))  return String("ERR");
    if (isinf(v))  return String("INF");
    char buf[24];
    snprintf(buf, sizeof(buf), "%.10g", v);
    return String(buf);
}

// ─── Drawing ───
static uint16_t key_color(const CkKey& key) {
    uint8_t a = key.action;
    if (a <= CK_DIGIT_BASE + 9 || a == CK_DOT) return COL_NUM;
    if (a == CK_CLR || a == CK_NEG || a == CK_PCT) return COL_FUNC;
    if (a == CK_DIV || a == CK_MUL || a == CK_SUB || a == CK_ADD) return COL_OP;
    if (a == CK_EQ) return COL_EQ;
    return COL_NUM;
}

static int cl_draw_key(int x, int y, const CkKey& key, bool pressed) {
    int w = key.width * CL_CELL_W;
    int h = CL_KP_ROW_H - 2;
    uint16_t fg = key_color(key);
    uint16_t bg = pressed ? fg : COL_DISP;
    uint16_t tx = pressed ? COL_BG : fg;
    gfx->fillRect(x + 1, y + 1, w - 2, h, bg);
    gfx->drawRect(x + 1, y + 1, w - 2, h, fg);
    int tw = (int)strlen(key.label) * 6 * CL_KEY_TS;
    int th = 8 * CL_KEY_TS;
    gfx->setTextSize(CL_KEY_TS);
    gfx->setTextColor(tx);
    gfx->setCursor(x + (w - tw) / 2, y + (h - th) / 2);
    gfx->print(key.label);
    return w;
}

static void cl_draw_keypad() {
    gfx->fillRect(0, CL_KP_Y, CL_W, CL_KP_H, COL_BG);
    for (int r = 0; r < CL_KP_ROWS; r++) {
        int y = CL_KP_Y + r * CL_KP_ROW_H;
        int x = 0;
        for (int k = 0; k < CL_ROW_COUNTS[r]; k++) {
            const CkKey& key = CL_ROWS[r][k];
            cl_draw_key(x, y, key, false);
            x += key.width * CL_CELL_W;
        }
    }
}

static void cl_draw_display(const String& s, char pending_op) {
    gfx->fillRect(0, CL_DISP_Y, CL_W, CL_DISP_H, COL_DISP);
    gfx->drawRect(0, CL_DISP_Y, CL_W, CL_DISP_H, COL_DIM);

    // Pending-op indicator in the top-left
    if (pending_op) {
        gfx->setTextSize(2);
        gfx->setTextColor(COL_OP);
        gfx->setCursor(6, CL_DISP_Y + 6);
        char buf[2] = { pending_op == '*' ? 'x' : pending_op, 0 };
        gfx->print(buf);
    }

    // Right-aligned numeric display
    int ts = CL_DISP_TS;
    int tw = (int)s.length() * 6 * ts;
    while (tw > CL_W - 16 && ts > 1) {
        ts--;
        tw = (int)s.length() * 6 * ts;
    }
    int x = CL_W - tw - 8;
    int y = CL_DISP_Y + (CL_DISP_H - 8 * ts) / 2;
    gfx->setTextSize(ts);
    gfx->setTextColor(COL_TXT);
    gfx->setCursor(x, y);
    gfx->print(s);
}

static void cl_draw_chrome() {
    gfx->fillScreen(COL_BG);
    gfx->fillRect(0, 0, CL_W, CL_EXIT_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, (CL_EXIT_H - 8) / 2);
    gfx->print("< EXIT");
    gfx->fillRect(0, CL_TITLE_Y, CL_W, CL_TITLE_H, COL_HEADER);
    gfx->setTextSize(CL_TITLE_TS);
    gfx->setTextColor(COL_FUNC);
    gfx->setCursor(8, CL_TITLE_Y + (CL_TITLE_H - 8 * CL_TITLE_TS) / 2);
    gfx->print("CALC");
}

static bool cl_hit(int tx, int ty, int* out_row, int* out_col) {
    if (ty < CL_KP_Y || ty >= CL_KP_Y + CL_KP_H) return false;
    int row = (ty - CL_KP_Y) / CL_KP_ROW_H;
    if (row < 0 || row >= CL_KP_ROWS) return false;
    int x = 0;
    for (int k = 0; k < CL_ROW_COUNTS[row]; k++) {
        int w = CL_ROWS[row][k].width * CL_CELL_W;
        if (tx >= x && tx < x + w) {
            *out_row = row; *out_col = k;
            return true;
        }
        x += w;
    }
    return false;
}

static void cl_highlight(int row, int col, bool pressed) {
    if (row < 0 || row >= CL_KP_ROWS || col >= CL_ROW_COUNTS[row]) return;
    int x = 0;
    for (int k = 0; k < col; k++) {
        x += CL_ROWS[row][k].width * CL_CELL_W;
    }
    cl_draw_key(x, CL_KP_Y + row * CL_KP_ROW_H, CL_ROWS[row][col], pressed);
}

static void cl_wait_release() {
    int16_t x, y;
    while (_cl_touch(&x, &y)) { delay(20); yield(); }
}

// ─── Math ───
static double apply_op(double a, double b, char op) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return (b == 0.0) ? NAN : a / b;
    }
    return b;
}

// ─── Main loop ───
void pm_run_calculator() {
    cl_draw_chrome();
    cl_draw_keypad();

    String display = "0";
    double accumulator = 0;
    char   pending_op = 0;
    bool   after_op = true;        // next digit should replace, not append
    bool   has_pending_value = false;

    cl_draw_display(display, pending_op);

    bool was_touched = false;
    int press_row = -1, press_col = -1;
    bool press_exit = false;

    while (true) {
        int16_t tx, ty;
        bool touched = _cl_touch(&tx, &ty);

        if (touched && !was_touched) {
            press_row = -1; press_col = -1; press_exit = false;
            if (ty < CL_EXIT_H) {
                press_exit = true;
            } else if (cl_hit(tx, ty, &press_row, &press_col)) {
                cl_highlight(press_row, press_col, true);
            }
        } else if (!touched && was_touched) {
            if (press_exit) { cl_wait_release(); return; }
            if (press_row >= 0 && press_col >= 0) {
                cl_highlight(press_row, press_col, false);
                const CkKey& key = CL_ROWS[press_row][press_col];
                uint8_t a = key.action;

                if (a >= CK_DIGIT_BASE && a <= CK_DIGIT_BASE + 9) {
                    char d = '0' + (a - CK_DIGIT_BASE);
                    if (after_op || display == "0") {
                        display = String(d);
                        after_op = false;
                    } else if (display.length() < 14) {
                        display += d;
                    }
                } else if (a == CK_DOT) {
                    if (after_op) { display = "0."; after_op = false; }
                    else if (display.indexOf('.') < 0) display += ".";
                } else if (a == CK_CLR) {
                    display = "0";
                    accumulator = 0;
                    pending_op = 0;
                    has_pending_value = false;
                    after_op = true;
                } else if (a == CK_NEG) {
                    if (display != "0") {
                        if (display.startsWith("-")) display = display.substring(1);
                        else display = "-" + display;
                    }
                } else if (a == CK_PCT) {
                    double v = display.toDouble() / 100.0;
                    display = format_value(v);
                } else if (a == CK_DIV || a == CK_MUL ||
                           a == CK_SUB || a == CK_ADD) {
                    char op = '+';
                    if (a == CK_DIV) op = '/';
                    else if (a == CK_MUL) op = '*';
                    else if (a == CK_SUB) op = '-';
                    else if (a == CK_ADD) op = '+';

                    double cur = display.toDouble();
                    if (pending_op && has_pending_value) {
                        accumulator = apply_op(accumulator, cur, pending_op);
                        display = format_value(accumulator);
                    } else {
                        accumulator = cur;
                    }
                    has_pending_value = true;
                    pending_op = op;
                    after_op = true;
                } else if (a == CK_EQ) {
                    if (pending_op && has_pending_value) {
                        double cur = display.toDouble();
                        accumulator = apply_op(accumulator, cur, pending_op);
                        display = format_value(accumulator);
                    }
                    pending_op = 0;
                    has_pending_value = false;
                    after_op = true;
                }
                cl_draw_display(display, pending_op);
            }
            press_row = -1; press_col = -1; press_exit = false;
        }
        was_touched = touched;
        delay(15);
        yield();
    }
}

#endif  // DEVICE_C28P || DEVICE_MAXINE
