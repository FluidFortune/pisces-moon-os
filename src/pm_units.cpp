// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_units.cpp — Unit converter for C28P + Maxine
//
//  Touch-only kiosk app for measurement conversion.
//
//  DESIGN:
//
//  Each category has a list of units with conversion factors to a
//  category-internal SI base unit (meters for length, grams for
//  weight, liters for volume). Conversion is "value → base via
//  multiplier → target via reciprocal multiplier". Temperature is
//  the exception — it's affine, not just scalar, so it uses
//  explicit to-Kelvin / from-Kelvin lambdas.
//
//  CURRENCY OMISSION (deliberate):
//
//  Currency converters need live exchange rates, which means an
//  internet round-trip every time a user opens the app. Outside
//  the scope of this kiosk app — measurement conversion is offline
//  and deterministic, currency would be neither.
//
//  LAYOUT (C28P 240×320 portrait base):
//
//    y=  0..14   Exit bar
//    y= 16..36   Title bar ("UNITS")
//    y= 40..76   Category tabs: LEN / WT / TEMP / VOL
//    y= 80..120  FROM unit selector (tap to cycle)
//    y=122..162  TO unit selector (tap to cycle)
//    y=164..210  Input value display (right-aligned)
//    y=212..258  Result value display (right-aligned)
//    y=260..316  Number pad (3×3 + bottom row)
//
//  The number pad is compact: 9 digit keys, decimal point, +/-,
//  and a backspace. No equals key — conversion is live as you
//  type. Swap (↕) button swaps FROM and TO units.
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "pm_units.h"

extern Arduino_GFX *gfx;

#if defined(DEVICE_C28P) || defined(DEVICE_C5)
#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
static inline bool _un_touch(int16_t* x, int16_t* y) { return c28p_touch_read(x, y); }
#else  // DEVICE_C5
extern bool c5_touch_read(int16_t* x, int16_t* y);
static inline bool _un_touch(int16_t* x, int16_t* y) { return c5_touch_read(x, y); }
#endif
static const int UN_W           = 240;
static const int UN_H           = 320;
static const int UN_EXIT_H      = 14;
static const int UN_TITLE_Y     = 16;
static const int UN_TITLE_H     = 22;
static const int UN_TABS_Y      = 42;
static const int UN_TABS_H      = 34;
static const int UN_FROM_Y      = 80;
static const int UN_SEL_H       = 38;
static const int UN_TO_Y        = UN_FROM_Y + UN_SEL_H + 4;
static const int UN_IN_Y        = UN_TO_Y + UN_SEL_H + 4;
static const int UN_DISP_H      = 46;
static const int UN_OUT_Y       = UN_IN_Y + UN_DISP_H + 2;
static const int UN_PAD_Y       = UN_OUT_Y + UN_DISP_H + 4;
static const int UN_PAD_ROWS    = 2;       // compact: digits in 2 rows + ops
static const int UN_PAD_ROW_H   = 28;
static const int UN_TITLE_TS    = 2;
static const int UN_TAB_TS      = 1;
static const int UN_SEL_TS      = 2;
static const int UN_DISP_TS     = 3;
static const int UN_PAD_TS      = 2;
#else  // DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
static inline bool _un_touch(int16_t* x, int16_t* y) { return maxine_touch_read(x, y); }
static const int UN_W           = 480;
static const int UN_H           = 800;
static const int UN_EXIT_H      = 32;
static const int UN_TITLE_Y     = 36;
static const int UN_TITLE_H     = 56;
static const int UN_TABS_Y      = 104;
static const int UN_TABS_H      = 80;
static const int UN_FROM_Y      = 200;
static const int UN_SEL_H       = 80;
static const int UN_TO_Y        = UN_FROM_Y + UN_SEL_H + 8;
static const int UN_IN_Y        = UN_TO_Y + UN_SEL_H + 12;
static const int UN_DISP_H      = 90;
static const int UN_OUT_Y       = UN_IN_Y + UN_DISP_H + 6;
static const int UN_PAD_Y       = UN_OUT_Y + UN_DISP_H + 12;
static const int UN_PAD_ROWS    = 2;
static const int UN_PAD_ROW_H   = 70;
static const int UN_TITLE_TS    = 4;
static const int UN_TAB_TS      = 2;
static const int UN_SEL_TS      = 4;
static const int UN_DISP_TS     = 6;
static const int UN_PAD_TS      = 4;
#endif

// ─── Theme ───
static const uint16_t COL_BG     = 0x0000;
static const uint16_t COL_HEADER = 0x0841;
static const uint16_t COL_ACC    = 0x07FF;
static const uint16_t COL_TXT    = 0xFFFF;
static const uint16_t COL_DIM    = 0x4208;
static const uint16_t COL_TILE   = 0x18C3;
static const uint16_t COL_OP     = 0xFD20;
static const uint16_t COL_DEL    = 0xF800;

// ─── Categories and units ───
//
// Length/weight/volume: { name, factor-to-base }. Conversion is
//   base = value * unit.factor
//   result = base / target.factor
//
// Temperature is special — affine, not scalar — handled below.
struct ScalarUnit {
    const char* name;
    double factor;   // multiply by this to reach the base unit
};

static const ScalarUnit LEN[] = {
    {"m",   1.0},
    {"cm",  0.01},
    {"mm",  0.001},
    {"km",  1000.0},
    {"in",  0.0254},
    {"ft",  0.3048},
    {"yd",  0.9144},
    {"mi",  1609.344},
};
static const ScalarUnit WT[] = {
    {"kg",  1000.0},
    {"g",   1.0},
    {"lb",  453.59237},
    {"oz",  28.349523125},
};
static const ScalarUnit VOL[] = {
    {"L",     1.0},
    {"mL",    0.001},
    {"gal",   3.785411784},   // US gal
    {"qt",    0.946352946},
    {"pt",    0.473176473},
    {"cup",   0.2365882365},
    {"floz",  0.0295735296},
};

enum UnCat { UN_LEN = 0, UN_WT, UN_TEMP, UN_VOL, UN_CAT_COUNT };
static const char* CAT_LABEL[UN_CAT_COUNT] = { "LEN", "WT", "TEMP", "VOL" };

// Return number of units in the active category.
static int cat_count(UnCat c) {
    switch (c) {
        case UN_LEN:  return sizeof(LEN) / sizeof(LEN[0]);
        case UN_WT:   return sizeof(WT)  / sizeof(WT[0]);
        case UN_VOL:  return sizeof(VOL) / sizeof(VOL[0]);
        case UN_TEMP: return 3;
        default:      return 0;
    }
}

static const char* unit_name(UnCat c, int idx) {
    switch (c) {
        case UN_LEN:  return LEN[idx].name;
        case UN_WT:   return WT[idx].name;
        case UN_VOL:  return VOL[idx].name;
        case UN_TEMP: { static const char* T[] = {"C","F","K"}; return T[idx]; }
        default:      return "?";
    }
}

// Convert value with given category from unit `from_idx` to `to_idx`.
static double convert(UnCat c, int from_idx, int to_idx, double v) {
    if (c == UN_TEMP) {
        // To Kelvin first.
        double kelvin = 0;
        if      (from_idx == 0) kelvin = v + 273.15;          // C → K
        else if (from_idx == 1) kelvin = (v - 32.0) * 5.0/9.0 + 273.15;  // F → K
        else                    kelvin = v;                   // K → K
        if      (to_idx == 0) return kelvin - 273.15;         // K → C
        else if (to_idx == 1) return (kelvin - 273.15) * 9.0/5.0 + 32.0;  // K → F
        else                  return kelvin;
    }
    const ScalarUnit* tbl = nullptr;
    if (c == UN_LEN)      tbl = LEN;
    else if (c == UN_WT)  tbl = WT;
    else if (c == UN_VOL) tbl = VOL;
    if (!tbl) return 0;
    double base = v * tbl[from_idx].factor;
    return base / tbl[to_idx].factor;
}

// Format a result for display — trims and keeps it human-readable.
static String fmt(double v) {
    if (isnan(v)) return "ERR";
    if (isinf(v)) return "INF";
    char buf[24];
    snprintf(buf, sizeof(buf), "%.6g", v);
    return String(buf);
}

// ─── Keypad layout ───
//
// We're tight on vertical real estate, so the pad is 2 rows × 5 cols:
//   Row 0: 7  8  9  ←  ↕  (backspace, swap)
//   Row 1: 4  5  6  .  ±
// Row 2 (in code, drawn below): 1 2 3 0 C
//
// 3 rows × 5 cols. Each cell = UN_W/5.
enum PadAction {
    PD_DIGIT,
    PD_DOT,
    PD_BKSP,
    PD_SWAP,
    PD_NEG,
    PD_CLR,
};

struct PadKey {
    uint8_t action;
    char    digit;
    const char* label;
};

static const PadKey PAD[3][5] = {
    { {PD_DIGIT,'7',"7"}, {PD_DIGIT,'8',"8"}, {PD_DIGIT,'9',"9"},
      {PD_BKSP,0,"<-"},   {PD_SWAP,0,"<>"} },
    { {PD_DIGIT,'4',"4"}, {PD_DIGIT,'5',"5"}, {PD_DIGIT,'6',"6"},
      {PD_DOT,0,"."},     {PD_NEG,0,"+/-"} },
    { {PD_DIGIT,'1',"1"}, {PD_DIGIT,'2',"2"}, {PD_DIGIT,'3',"3"},
      {PD_DIGIT,'0',"0"}, {PD_CLR,0,"C"} },
};

static const int UN_PAD_COLS = 5;
static const int UN_PAD_TOTAL_ROWS = 3;
static const int UN_PAD_CELL_W = UN_W / UN_PAD_COLS;

// ─── Drawing ───
static void un_draw_chrome() {
    gfx->fillScreen(COL_BG);
    gfx->fillRect(0, 0, UN_W, UN_EXIT_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, (UN_EXIT_H - 8) / 2);
    gfx->print("< EXIT");
    gfx->fillRect(0, UN_TITLE_Y, UN_W, UN_TITLE_H, COL_HEADER);
    gfx->setTextSize(UN_TITLE_TS);
    gfx->setTextColor(COL_ACC);
    gfx->setCursor(8, UN_TITLE_Y + (UN_TITLE_H - 8 * UN_TITLE_TS) / 2);
    gfx->print("UNITS");
}

static void un_draw_tabs(UnCat active) {
    gfx->fillRect(0, UN_TABS_Y, UN_W, UN_TABS_H, COL_BG);
    int tw = UN_W / UN_CAT_COUNT;
    for (int i = 0; i < UN_CAT_COUNT; i++) {
        bool sel = (i == active);
        uint16_t bg = sel ? COL_ACC : COL_TILE;
        uint16_t fg = sel ? COL_BG  : COL_DIM;
        gfx->fillRect(i * tw + 2, UN_TABS_Y + 2, tw - 4, UN_TABS_H - 4, bg);
        gfx->drawRect(i * tw + 2, UN_TABS_Y + 2, tw - 4, UN_TABS_H - 4, COL_DIM);
        gfx->setTextSize(UN_TAB_TS);
        gfx->setTextColor(fg);
        int twpx = (int)strlen(CAT_LABEL[i]) * 6 * UN_TAB_TS;
        gfx->setCursor(i * tw + (tw - twpx) / 2,
                       UN_TABS_Y + (UN_TABS_H - 8 * UN_TAB_TS) / 2);
        gfx->print(CAT_LABEL[i]);
    }
}

static void un_draw_selector(int y, const char* prefix, const char* unit_label) {
    gfx->fillRect(0, y, UN_W, UN_SEL_H, COL_TILE);
    gfx->drawRect(0, y, UN_W, UN_SEL_H, COL_DIM);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(8, y + 4);
    gfx->print(prefix);
    gfx->setTextSize(UN_SEL_TS);
    gfx->setTextColor(COL_TXT);
    int tw = (int)strlen(unit_label) * 6 * UN_SEL_TS;
    gfx->setCursor((UN_W - tw) / 2, y + (UN_SEL_H - 8 * UN_SEL_TS) / 2 + 6);
    gfx->print(unit_label);
    // Cycle arrow hint on the right
    gfx->setTextSize(1);
    gfx->setTextColor(COL_ACC);
    gfx->setCursor(UN_W - 18, y + UN_SEL_H - 12);
    gfx->print("tap");
}

static void un_draw_display(int y, const String& s, bool input) {
    gfx->fillRect(0, y, UN_W, UN_DISP_H, input ? 0x10A2 : COL_BG);
    gfx->drawRect(0, y, UN_W, UN_DISP_H, COL_DIM);
    int ts = UN_DISP_TS;
    int tw = (int)s.length() * 6 * ts;
    while (tw > UN_W - 16 && ts > 1) {
        ts--;
        tw = (int)s.length() * 6 * ts;
    }
    int x = UN_W - tw - 8;
    int ty = y + (UN_DISP_H - 8 * ts) / 2;
    gfx->setTextSize(ts);
    gfx->setTextColor(input ? COL_TXT : COL_ACC);
    gfx->setCursor(x, ty);
    gfx->print(s);
}

static uint16_t pad_color(const PadKey& key) {
    if (key.action == PD_BKSP || key.action == PD_CLR) return COL_DEL;
    if (key.action == PD_SWAP) return COL_ACC;
    if (key.action == PD_NEG)  return COL_OP;
    return COL_TXT;
}

static void un_draw_pad_key(int r, int c, bool pressed) {
    int x = c * UN_PAD_CELL_W;
    int y = UN_PAD_Y + r * UN_PAD_ROW_H;
    int w = UN_PAD_CELL_W;
    int h = UN_PAD_ROW_H - 2;
    const PadKey& key = PAD[r][c];
    uint16_t fg = pad_color(key);
    uint16_t bg = pressed ? fg : COL_TILE;
    uint16_t tx = pressed ? COL_BG : fg;
    gfx->fillRect(x + 1, y + 1, w - 2, h, bg);
    gfx->drawRect(x + 1, y + 1, w - 2, h, fg);
    gfx->setTextSize(UN_PAD_TS);
    gfx->setTextColor(tx);
    int tw = (int)strlen(key.label) * 6 * UN_PAD_TS;
    gfx->setCursor(x + (w - tw) / 2, y + (h - 8 * UN_PAD_TS) / 2);
    gfx->print(key.label);
}

static void un_draw_pad() {
    gfx->fillRect(0, UN_PAD_Y, UN_W, UN_PAD_TOTAL_ROWS * UN_PAD_ROW_H, COL_BG);
    for (int r = 0; r < UN_PAD_TOTAL_ROWS; r++) {
        for (int c = 0; c < UN_PAD_COLS; c++) {
            un_draw_pad_key(r, c, false);
        }
    }
}

static bool un_pad_hit(int tx, int ty, int* out_r, int* out_c) {
    int total_h = UN_PAD_TOTAL_ROWS * UN_PAD_ROW_H;
    if (ty < UN_PAD_Y || ty >= UN_PAD_Y + total_h) return false;
    int r = (ty - UN_PAD_Y) / UN_PAD_ROW_H;
    int c = tx / UN_PAD_CELL_W;
    if (r < 0 || r >= UN_PAD_TOTAL_ROWS) return false;
    if (c < 0 || c >= UN_PAD_COLS) return false;
    *out_r = r; *out_c = c;
    return true;
}

static void un_wait_release() {
    int16_t x, y;
    while (_un_touch(&x, &y)) { delay(20); yield(); }
}

// ─── Entry ───
void pm_run_units() {
    UnCat cat = UN_LEN;
    int from_idx = 0, to_idx = 1;
    String input_s = "0";

    auto refresh_all = [&]() {
        un_draw_chrome();
        un_draw_tabs(cat);
        un_draw_selector(UN_FROM_Y, "FROM", unit_name(cat, from_idx));
        un_draw_selector(UN_TO_Y,   "TO  ", unit_name(cat, to_idx));
        un_draw_display(UN_IN_Y,  input_s, true);
        double r = convert(cat, from_idx, to_idx, input_s.toDouble());
        un_draw_display(UN_OUT_Y, fmt(r), false);
        un_draw_pad();
    };

    auto refresh_result = [&]() {
        un_draw_display(UN_IN_Y, input_s, true);
        double r = convert(cat, from_idx, to_idx, input_s.toDouble());
        un_draw_display(UN_OUT_Y, fmt(r), false);
    };

    refresh_all();

    bool was_touched = false;
    int press_r = -1, press_c = -1;
    int press_zone = 0;   // 1=exit, 2=tab, 3=from, 4=to, 5=pad

    while (true) {
        int16_t tx, ty;
        bool touched = _un_touch(&tx, &ty);

        if (touched && !was_touched) {
            press_zone = 0; press_r = -1; press_c = -1;
            if (ty < UN_EXIT_H) {
                press_zone = 1;
            } else if (ty >= UN_TABS_Y && ty < UN_TABS_Y + UN_TABS_H) {
                press_zone = 2;
                press_c = tx / (UN_W / UN_CAT_COUNT);
                if (press_c < 0) press_c = 0;
                if (press_c >= UN_CAT_COUNT) press_c = UN_CAT_COUNT - 1;
            } else if (ty >= UN_FROM_Y && ty < UN_FROM_Y + UN_SEL_H) {
                press_zone = 3;
            } else if (ty >= UN_TO_Y && ty < UN_TO_Y + UN_SEL_H) {
                press_zone = 4;
            } else if (un_pad_hit(tx, ty, &press_r, &press_c)) {
                press_zone = 5;
                un_draw_pad_key(press_r, press_c, true);
            }
        } else if (!touched && was_touched) {
            if (press_zone == 1) { un_wait_release(); return; }

            if (press_zone == 2) {
                UnCat newcat = (UnCat)press_c;
                if (newcat != cat) {
                    cat = newcat;
                    from_idx = 0;
                    to_idx   = (cat_count(cat) > 1) ? 1 : 0;
                    input_s  = "0";
                    refresh_all();
                }
            } else if (press_zone == 3) {
                int n = cat_count(cat);
                from_idx = (from_idx + 1) % n;
                if (from_idx == to_idx) from_idx = (from_idx + 1) % n;
                un_draw_selector(UN_FROM_Y, "FROM", unit_name(cat, from_idx));
                refresh_result();
            } else if (press_zone == 4) {
                int n = cat_count(cat);
                to_idx = (to_idx + 1) % n;
                if (to_idx == from_idx) to_idx = (to_idx + 1) % n;
                un_draw_selector(UN_TO_Y, "TO  ", unit_name(cat, to_idx));
                refresh_result();
            } else if (press_zone == 5 && press_r >= 0 && press_c >= 0) {
                un_draw_pad_key(press_r, press_c, false);
                const PadKey& key = PAD[press_r][press_c];
                switch (key.action) {
                    case PD_DIGIT:
                        if (input_s == "0" || input_s == "-0") {
                            input_s = (input_s.startsWith("-") ? "-" : "") + String(key.digit);
                        } else if (input_s.length() < 14) {
                            input_s += key.digit;
                        }
                        break;
                    case PD_DOT:
                        if (input_s.indexOf('.') < 0) input_s += ".";
                        break;
                    case PD_BKSP:
                        if (input_s.length() > 1) input_s.remove(input_s.length() - 1);
                        else input_s = "0";
                        if (input_s == "-") input_s = "0";
                        break;
                    case PD_CLR:
                        input_s = "0";
                        break;
                    case PD_NEG:
                        if (input_s != "0") {
                            if (input_s.startsWith("-")) input_s = input_s.substring(1);
                            else input_s = "-" + input_s;
                        }
                        break;
                    case PD_SWAP: {
                        int tmp = from_idx;
                        from_idx = to_idx;
                        to_idx = tmp;
                        un_draw_selector(UN_FROM_Y, "FROM", unit_name(cat, from_idx));
                        un_draw_selector(UN_TO_Y,   "TO  ", unit_name(cat, to_idx));
                        break;
                    }
                }
                refresh_result();
            }
            press_zone = 0; press_r = -1; press_c = -1;
        }
        was_touched = touched;
        delay(15);
        yield();
    }
}

#endif  // DEVICE_C28P || DEVICE_MAXINE || DEVICE_C5
