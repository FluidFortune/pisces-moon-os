// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_text_input.cpp — Touch keyboard for C28P + Maxine
//
//  Modal full-screen overlay with a QWERTY keyboard. Used by every
//  kiosk app that needs text entry (notes, contacts, calendar).
//
//  LAYOUT (C28P 240×320 portrait base; Maxine scales ~2x):
//
//    y=  0..14    Exit bar (tap to cancel)
//    y= 16..36    Prompt label
//    y= 40..120   Text edit area (wraps lines, soft cursor at end)
//    y=124..280   Keyboard — 4 rows of keys on a 10-cell grid
//    y=284..318   Footer hint
//
//  KEYS:
//
//    Layout is a 10-column grid; each row sums to 10 cells. Some
//    keys span multiple cells (SHIFT and 123 are 2 cells wide,
//    SPACE is 5 cells wide).
//
//    Row 0 (10 × 1): q w e r t y u i o p
//    Row 1 (10 × 1): a s d f g h j k l ⌫
//    Row 2 (2 + 7×1 + 1): ⇧⇧ z x c v b n m .
//    Row 3 (2 + 1 + 5 + 1 + 1): 123 , SPACE . ↵
//
//    Upper layer is the same shape with letters capitalized.
//    Sym layer replaces letters with digits and punctuation.
//
//  SHIFT model: tapping ⇧ once toggles to UPPER; the next tap (after
//  typing) toggles back. No timing-based caps-lock; explicit and
//  predictable.
//
//  Confirm: ENTER on the keyboard (↵).
//  Cancel:  tap the top EXIT bar.
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <string.h>
#include "pm_text_input.h"

extern Arduino_GFX *gfx;

#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
static inline bool _ti_touch(int16_t* x, int16_t* y) { return c28p_touch_read(x, y); }
static const int TI_W           = 240;
static const int TI_H           = 320;
static const int TI_EXIT_H      = 14;
static const int TI_TITLE_Y     = 16;
static const int TI_TITLE_H     = 22;
static const int TI_EDIT_Y      = 42;
static const int TI_EDIT_H      = 78;
static const int TI_KBD_Y       = 124;
static const int TI_KBD_ROWS    = 4;
static const int TI_KBD_ROW_H   = 38;
static const int TI_FOOTER_Y    = 280;
static const int TI_FOOTER_H    = 38;
static const int TI_TITLE_TS    = 2;
static const int TI_EDIT_TS     = 1;
static const int TI_KEY_TS      = 2;
static const int TI_HINT_TS     = 1;
#else  // DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
static inline bool _ti_touch(int16_t* x, int16_t* y) { return maxine_touch_read(x, y); }
static const int TI_W           = 480;
static const int TI_H           = 800;
static const int TI_EXIT_H      = 32;
static const int TI_TITLE_Y     = 38;
static const int TI_TITLE_H     = 56;
static const int TI_EDIT_Y      = 104;
static const int TI_EDIT_H      = 220;
static const int TI_KBD_Y       = 336;
static const int TI_KBD_ROWS    = 4;
static const int TI_KBD_ROW_H   = 96;
static const int TI_FOOTER_Y    = 728;
static const int TI_FOOTER_H    = 72;
static const int TI_TITLE_TS    = 4;
static const int TI_EDIT_TS     = 2;
static const int TI_KEY_TS      = 4;
static const int TI_HINT_TS     = 2;
#endif

static const int TI_CELL_W       = TI_W / 10;          // each grid cell
static const int TI_KBD_H        = TI_KBD_ROWS * TI_KBD_ROW_H;

// ─── Theme ───
static const uint16_t COL_BG       = 0x0000;
static const uint16_t COL_HEADER   = 0x0841;
static const uint16_t COL_KEY_BG   = 0x18C3;
static const uint16_t COL_KEY_FG   = 0xFFFF;
static const uint16_t COL_KEY_HI   = 0x07FF;   // pressed
static const uint16_t COL_KEY_FUNC = 0xFD20;   // shift / 123 / enter
static const uint16_t COL_KEY_DEL  = 0xF800;   // backspace
static const uint16_t COL_EDIT_BG  = 0x10A2;
static const uint16_t COL_CURSOR   = 0x07E0;
static const uint16_t COL_DIM      = 0x4208;

// ─── Key model ───
//
// Each key is an (action, value, label, width) tuple. Width is in
// 10-cell grid units. Action codes:
//
//   0 = insert character at value into the buffer
//   1 = SHIFT toggle (lower/upper)
//   2 = BACKSPACE (delete last char)
//   3 = LAYER toggle (letters ↔ sym)
//   4 = ENTER (confirm)
//   5 = SPACE
enum KbAction { KB_CHAR = 0, KB_SHIFT, KB_BKSP, KB_LAYER, KB_ENTER, KB_SPACE };

struct KbKey {
    uint8_t  action;
    char     value;      // produced character (KB_CHAR) or 0
    const char* label;   // override draw label (nullptr = use value)
    uint8_t  width;      // grid cells (default 1)
};

// ─── Layer definitions ───
//
// We use three layouts: lower, upper, sym. Each is 4 rows of keys
// that total 10 cells per row. The layouts mirror each other so the
// per-row geometry stays identical across layers.
static const KbKey ROW0_LOWER[] = {
    {KB_CHAR,'q',nullptr,1},{KB_CHAR,'w',nullptr,1},{KB_CHAR,'e',nullptr,1},
    {KB_CHAR,'r',nullptr,1},{KB_CHAR,'t',nullptr,1},{KB_CHAR,'y',nullptr,1},
    {KB_CHAR,'u',nullptr,1},{KB_CHAR,'i',nullptr,1},{KB_CHAR,'o',nullptr,1},
    {KB_CHAR,'p',nullptr,1},
};
static const KbKey ROW1_LOWER[] = {
    {KB_CHAR,'a',nullptr,1},{KB_CHAR,'s',nullptr,1},{KB_CHAR,'d',nullptr,1},
    {KB_CHAR,'f',nullptr,1},{KB_CHAR,'g',nullptr,1},{KB_CHAR,'h',nullptr,1},
    {KB_CHAR,'j',nullptr,1},{KB_CHAR,'k',nullptr,1},{KB_CHAR,'l',nullptr,1},
    {KB_BKSP,0,"<X",1},
};
static const KbKey ROW2_LOWER[] = {
    {KB_SHIFT,0,"SH",2},
    {KB_CHAR,'z',nullptr,1},{KB_CHAR,'x',nullptr,1},{KB_CHAR,'c',nullptr,1},
    {KB_CHAR,'v',nullptr,1},{KB_CHAR,'b',nullptr,1},{KB_CHAR,'n',nullptr,1},
    {KB_CHAR,'m',nullptr,1},
    {KB_CHAR,'.',nullptr,1},
};
static const KbKey ROW3_LOWER[] = {
    {KB_LAYER,0,"123",2},
    {KB_CHAR,',',nullptr,1},
    {KB_SPACE,' ',"SPACE",5},
    {KB_CHAR,'\'',nullptr,1},
    {KB_ENTER,0,"OK",1},
};

static const KbKey ROW0_UPPER[] = {
    {KB_CHAR,'Q',nullptr,1},{KB_CHAR,'W',nullptr,1},{KB_CHAR,'E',nullptr,1},
    {KB_CHAR,'R',nullptr,1},{KB_CHAR,'T',nullptr,1},{KB_CHAR,'Y',nullptr,1},
    {KB_CHAR,'U',nullptr,1},{KB_CHAR,'I',nullptr,1},{KB_CHAR,'O',nullptr,1},
    {KB_CHAR,'P',nullptr,1},
};
static const KbKey ROW1_UPPER[] = {
    {KB_CHAR,'A',nullptr,1},{KB_CHAR,'S',nullptr,1},{KB_CHAR,'D',nullptr,1},
    {KB_CHAR,'F',nullptr,1},{KB_CHAR,'G',nullptr,1},{KB_CHAR,'H',nullptr,1},
    {KB_CHAR,'J',nullptr,1},{KB_CHAR,'K',nullptr,1},{KB_CHAR,'L',nullptr,1},
    {KB_BKSP,0,"<X",1},
};
static const KbKey ROW2_UPPER[] = {
    {KB_SHIFT,0,"sh",2},
    {KB_CHAR,'Z',nullptr,1},{KB_CHAR,'X',nullptr,1},{KB_CHAR,'C',nullptr,1},
    {KB_CHAR,'V',nullptr,1},{KB_CHAR,'B',nullptr,1},{KB_CHAR,'N',nullptr,1},
    {KB_CHAR,'M',nullptr,1},
    {KB_CHAR,'?',nullptr,1},
};
static const KbKey ROW3_UPPER[] = {
    {KB_LAYER,0,"123",2},
    {KB_CHAR,'!',nullptr,1},
    {KB_SPACE,' ',"SPACE",5},
    {KB_CHAR,'\"',nullptr,1},
    {KB_ENTER,0,"OK",1},
};

static const KbKey ROW0_SYM[] = {
    {KB_CHAR,'1',nullptr,1},{KB_CHAR,'2',nullptr,1},{KB_CHAR,'3',nullptr,1},
    {KB_CHAR,'4',nullptr,1},{KB_CHAR,'5',nullptr,1},{KB_CHAR,'6',nullptr,1},
    {KB_CHAR,'7',nullptr,1},{KB_CHAR,'8',nullptr,1},{KB_CHAR,'9',nullptr,1},
    {KB_CHAR,'0',nullptr,1},
};
static const KbKey ROW1_SYM[] = {
    {KB_CHAR,'-',nullptr,1},{KB_CHAR,'/',nullptr,1},{KB_CHAR,':',nullptr,1},
    {KB_CHAR,';',nullptr,1},{KB_CHAR,'(',nullptr,1},{KB_CHAR,')',nullptr,1},
    {KB_CHAR,'$',nullptr,1},{KB_CHAR,'&',nullptr,1},{KB_CHAR,'@',nullptr,1},
    {KB_BKSP,0,"<X",1},
};
static const KbKey ROW2_SYM[] = {
    {KB_LAYER,0,"abc",2},
    {KB_CHAR,'#',nullptr,1},{KB_CHAR,'+',nullptr,1},{KB_CHAR,'=',nullptr,1},
    {KB_CHAR,'*',nullptr,1},{KB_CHAR,'_',nullptr,1},{KB_CHAR,'!',nullptr,1},
    {KB_CHAR,'?',nullptr,1},
};
static const KbKey ROW3_SYM[] = {
    // Sym layer's bottom row keeps SPACE/ENTER reachable; the 123→abc
    // toggle has already moved up to row 2 to reduce the gymnastics
    // of switching back to letters mid-word.
    {KB_CHAR,'.',nullptr,1},
    {KB_CHAR,',',nullptr,1},
    {KB_CHAR,'<',nullptr,1},
    {KB_SPACE,' ',"SPACE",4},
    {KB_CHAR,'>',nullptr,1},
    {KB_CHAR,'\'',nullptr,1},
    {KB_ENTER,0,"OK",1},
};

struct LayerSpec {
    const KbKey* rows[TI_KBD_ROWS];
    uint8_t      counts[TI_KBD_ROWS];
};

static const LayerSpec LAYER_LOWER = {
    {ROW0_LOWER, ROW1_LOWER, ROW2_LOWER, ROW3_LOWER},
    {sizeof(ROW0_LOWER)/sizeof(KbKey),
     sizeof(ROW1_LOWER)/sizeof(KbKey),
     sizeof(ROW2_LOWER)/sizeof(KbKey),
     sizeof(ROW3_LOWER)/sizeof(KbKey)},
};
static const LayerSpec LAYER_UPPER = {
    {ROW0_UPPER, ROW1_UPPER, ROW2_UPPER, ROW3_UPPER},
    {sizeof(ROW0_UPPER)/sizeof(KbKey),
     sizeof(ROW1_UPPER)/sizeof(KbKey),
     sizeof(ROW2_UPPER)/sizeof(KbKey),
     sizeof(ROW3_UPPER)/sizeof(KbKey)},
};
static const LayerSpec LAYER_SYM = {
    {ROW0_SYM, ROW1_SYM, ROW2_SYM, ROW3_SYM},
    {sizeof(ROW0_SYM)/sizeof(KbKey),
     sizeof(ROW1_SYM)/sizeof(KbKey),
     sizeof(ROW2_SYM)/sizeof(KbKey),
     sizeof(ROW3_SYM)/sizeof(KbKey)},
};

// ─── Drawing ───
//
// Single key cell. cells = grid width (1, 2, 5...). Returns the
// pixel width consumed so the caller can advance x.
static int ti_draw_key(int x, int y, const KbKey& key, bool pressed) {
    int w = key.width * TI_CELL_W;
    int h = TI_KBD_ROW_H - 2;
    uint16_t bg = pressed ? COL_KEY_HI : COL_KEY_BG;
    uint16_t fg = COL_KEY_FG;
    if (key.action == KB_SHIFT || key.action == KB_LAYER || key.action == KB_ENTER)
        fg = COL_KEY_FUNC;
    if (key.action == KB_BKSP)
        fg = COL_KEY_DEL;
    gfx->fillRect(x + 1, y + 1, w - 2, h, bg);
    gfx->drawRect(x + 1, y + 1, w - 2, h, fg);

    char draw_buf[8];
    const char* label = key.label;
    if (!label || !label[0]) {
        draw_buf[0] = key.value;
        draw_buf[1] = 0;
        label = draw_buf;
    }
    int tw = (int)strlen(label) * 6 * TI_KEY_TS;
    int th = 8 * TI_KEY_TS;
    gfx->setTextSize(TI_KEY_TS);
    gfx->setTextColor(fg);
    gfx->setCursor(x + (w - tw) / 2, y + (h - th) / 2);
    gfx->print(label);
    return w;
}

static void ti_draw_layer(const LayerSpec& layer) {
    gfx->fillRect(0, TI_KBD_Y, TI_W, TI_KBD_H, COL_BG);
    for (int r = 0; r < TI_KBD_ROWS; r++) {
        int y = TI_KBD_Y + r * TI_KBD_ROW_H;
        int x = 0;
        for (int k = 0; k < layer.counts[r]; k++) {
            const KbKey& key = layer.rows[r][k];
            ti_draw_key(x, y, key, false);
            x += key.width * TI_CELL_W;
        }
    }
}

// Highlight a single key (called on press, redrawn flat on release).
static void ti_highlight(const LayerSpec& layer, int row, int col, bool pressed) {
    if (row < 0 || row >= TI_KBD_ROWS) return;
    int x = 0;
    for (int k = 0; k < col && k < layer.counts[row]; k++) {
        x += layer.rows[row][k].width * TI_CELL_W;
    }
    if (col >= layer.counts[row]) return;
    int y = TI_KBD_Y + row * TI_KBD_ROW_H;
    ti_draw_key(x, y, layer.rows[row][col], pressed);
}

// Hit-test: given an (x,y) inside the keyboard region, find which
// key was struck. Returns (row, col) by reference. Returns false
// if no key at that point.
static bool ti_hit(const LayerSpec& layer, int tx, int ty,
                   int* out_row, int* out_col) {
    if (ty < TI_KBD_Y || ty >= TI_KBD_Y + TI_KBD_H) return false;
    int row = (ty - TI_KBD_Y) / TI_KBD_ROW_H;
    if (row < 0 || row >= TI_KBD_ROWS) return false;
    int x = 0;
    for (int k = 0; k < layer.counts[row]; k++) {
        int w = layer.rows[row][k].width * TI_CELL_W;
        if (tx >= x && tx < x + w) {
            *out_row = row;
            *out_col = k;
            return true;
        }
        x += w;
    }
    return false;
}

// ─── Text edit area ───
//
// Soft-wrap the buffer into the edit area; show a cursor block
// after the last character. Truncates from the top if the text
// exceeds the visible area (the cursor stays visible).
static void ti_draw_edit(const char* buf, size_t len) {
    gfx->fillRect(0, TI_EDIT_Y, TI_W, TI_EDIT_H, COL_EDIT_BG);
    gfx->drawRect(0, TI_EDIT_Y, TI_W, TI_EDIT_H, COL_DIM);

    int char_w = 6 * TI_EDIT_TS;
    int line_h = 8 * TI_EDIT_TS + 3;
    int chars_per_line = (TI_W - 12) / char_w;
    if (chars_per_line < 1) chars_per_line = 1;
    int max_lines = (TI_EDIT_H - 8) / line_h;
    if (max_lines < 1) max_lines = 1;

    // First pass: figure out wrapped line starts so we can scroll.
    static int line_starts[64];
    int line_count = 1;
    line_starts[0] = 0;
    int col = 0;
    for (size_t i = 0; i < len && line_count < 64; i++) {
        if (buf[i] == '\n' || col >= chars_per_line) {
            if (line_count < 64) line_starts[line_count++] = (int)i + (buf[i] == '\n' ? 1 : 0);
            col = 0;
            if (buf[i] == '\n') continue;
        }
        col++;
    }
    int first_line = (line_count > max_lines) ? (line_count - max_lines) : 0;

    gfx->setTextSize(TI_EDIT_TS);
    gfx->setTextColor(COL_KEY_FG);

    int y = TI_EDIT_Y + 4;
    int draw_col = 0;
    size_t cursor_x_px = 6;
    size_t cursor_y_px = (size_t)y;
    bool   cursor_placed = false;

    // Skip to first visible line
    int start_idx = line_starts[first_line];
    col = 0;
    gfx->setCursor(6, y);
    for (size_t i = start_idx; i <= len && y < TI_EDIT_Y + TI_EDIT_H - line_h + 4; i++) {
        if (i == len) {
            cursor_x_px = 6 + col * char_w;
            cursor_y_px = y;
            cursor_placed = true;
            break;
        }
        char c = buf[i];
        if (c == '\n' || col >= chars_per_line) {
            y += line_h;
            col = 0;
            gfx->setCursor(6, y);
            if (c == '\n') continue;
        }
        gfx->write((uint8_t)c);
        col++;
        draw_col = col;
        (void)draw_col;
    }
    if (!cursor_placed) {
        cursor_x_px = 6 + col * char_w;
        cursor_y_px = y;
    }

    // Cursor block
    gfx->fillRect((int)cursor_x_px, (int)cursor_y_px,
                  char_w - 1, 8 * TI_EDIT_TS, COL_CURSOR);
}

// ─── Chrome ───
static void ti_draw_chrome(const char* label) {
    gfx->fillScreen(COL_BG);
    // Exit bar
    gfx->fillRect(0, 0, TI_W, TI_EXIT_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, (TI_EXIT_H - 8) / 2);
    gfx->print("< CANCEL");

    // Title with prompt
    gfx->fillRect(0, TI_TITLE_Y, TI_W, TI_TITLE_H, COL_HEADER);
    gfx->setTextSize(TI_TITLE_TS);
    gfx->setTextColor(COL_KEY_HI);
    gfx->setCursor(8, TI_TITLE_Y + (TI_TITLE_H - 8 * TI_TITLE_TS) / 2);
    gfx->print(label ? label : "INPUT");

    // Footer hint
    gfx->fillRect(0, TI_FOOTER_Y, TI_W, TI_FOOTER_H, COL_BG);
    gfx->setTextSize(TI_HINT_TS);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(8, TI_FOOTER_Y + (TI_FOOTER_H - 8 * TI_HINT_TS) / 2);
    gfx->print("Tap OK to confirm, EXIT to cancel");
}

static void ti_wait_release() {
    int16_t x, y;
    while (_ti_touch(&x, &y)) { delay(20); yield(); }
}

// ─── Main entry ───
bool pm_text_input(const char* label, char* out, size_t outlen, const char* initial) {
    if (!out || outlen == 0) return false;

    // Backing buffer (separate from caller's so we don't smash their
    // content on cancel). Cap at 4 KB which is well over our editor
    // use cases (notes max ~1 KB, contact fields ~256 B).
    static char buf[4096];
    size_t cap = outlen - 1;
    if (cap > sizeof(buf) - 1) cap = sizeof(buf) - 1;

    size_t len = 0;
    if (initial) {
        while (initial[len] && len < cap) {
            buf[len] = initial[len];
            len++;
        }
    }
    buf[len] = 0;

    enum Layer { L_LOWER = 0, L_UPPER, L_SYM };
    Layer layer = L_LOWER;
    auto layer_spec = [&]() -> const LayerSpec& {
        if (layer == L_UPPER) return LAYER_UPPER;
        if (layer == L_SYM)   return LAYER_SYM;
        return LAYER_LOWER;
    };

    ti_draw_chrome(label);
    ti_draw_edit(buf, len);
    ti_draw_layer(layer_spec());

    bool was_touched = false;
    int  press_row = -1, press_col = -1;
    bool press_exit = false;

    while (true) {
        int16_t tx, ty;
        bool touched = _ti_touch(&tx, &ty);

        if (touched && !was_touched) {
            press_row = -1; press_col = -1; press_exit = false;
            if (ty < TI_EXIT_H) {
                press_exit = true;
            } else {
                if (ti_hit(layer_spec(), tx, ty, &press_row, &press_col)) {
                    ti_highlight(layer_spec(), press_row, press_col, true);
                }
            }
        } else if (!touched && was_touched) {
            if (press_exit) {
                ti_wait_release();
                return false;
            }
            if (press_row >= 0 && press_col >= 0) {
                ti_highlight(layer_spec(), press_row, press_col, false);
                const KbKey& key = layer_spec().rows[press_row][press_col];
                bool repaint_kbd = false;
                switch (key.action) {
                    case KB_CHAR:
                    case KB_SPACE:
                        if (len < cap) {
                            buf[len++] = (key.action == KB_SPACE) ? ' ' : key.value;
                            buf[len] = 0;
                        }
                        // Auto-fall-back to lower after a typed UPPER char,
                        // mirrors phone keyboards' shift-once behavior.
                        if (layer == L_UPPER) {
                            layer = L_LOWER;
                            repaint_kbd = true;
                        }
                        break;
                    case KB_BKSP:
                        if (len > 0) { len--; buf[len] = 0; }
                        break;
                    case KB_SHIFT:
                        layer = (layer == L_UPPER) ? L_LOWER : L_UPPER;
                        repaint_kbd = true;
                        break;
                    case KB_LAYER:
                        // 123 ↔ ABC. Always lands on LOWER coming back.
                        layer = (layer == L_SYM) ? L_LOWER : L_SYM;
                        repaint_kbd = true;
                        break;
                    case KB_ENTER:
                        ti_wait_release();
                        memcpy(out, buf, len);
                        out[len] = 0;
                        return true;
                }
                ti_draw_edit(buf, len);
                if (repaint_kbd) ti_draw_layer(layer_spec());
            }
            press_row = -1; press_col = -1; press_exit = false;
        }
        was_touched = touched;
        delay(15);
        yield();
    }
}

#endif  // DEVICE_C28P || DEVICE_MAXINE
