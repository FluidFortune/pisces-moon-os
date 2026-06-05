// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  maxine_boot.cpp — Maxine (Sunton 8048S050) bring-up + launcher
//
//  Maxine is the same SOFTWARE class as the C28P: a touch-only
//  kiosk with no keyboard, no trackball, no LoRa/GPS. This file is
//  modeled directly on c28p_boot.cpp, scaled to the 480x800
//  portrait panel. All layout is expressed in terms of SCREEN_W /
//  SCREEN_H so it tracks the panel size rather than hardcoded
//  pixels.
//
//  Differences from the C28P:
//    - Touch is a GT911 (I2C) read via the TAMC_GT911 library
//      (already a common lib_dep), not the FT6336G register poke
//      the C28P uses.
//    - No microphone: audio-recording apps are not built for this
//      target, and the launcher's category set reflects that.
//    - Display is a parallel RGB panel; the gfx pointer is set up
//      in main.cpp. This file only draws through gfx and never
//      touches the panel hardware directly.
//
//  This is a FIRST-BOOT launcher: the GAMES category dispatches to
//  real games; other categories show a "coming soon" panel until
//  their apps are ported to this target. Explicit, honest scaffold.
// ─────────────────────────────────────────────

#ifdef DEVICE_MAXINE

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <WiFi.h>
#include <TAMC_GT911.h>
#include "hal_pins.h"
#include "maxine_boot.h"
#include "maxine_dpad.h"
#include "tetris.h"
#include "snake.h"
#include "pacman.h"
#include "galaga.h"
#include "pole_position.h"
#include "mario_bros.h"
#include "breakout.h"
#include "game_2048.h"
#include "minesweeper.h"
#include "connect4.h"
#include "simon.h"
#include "solitaire.h"
#include "asteroids.h"
#include "space_invaders.h"
#include "frogger.h"
#include "chess.h"
#include "ereader.h"
#include "maxine_apps.h"
#include "pm_clock.h"
#include "pm_tracker_scan.h"
#include "pm_notes.h"
#include "pm_contacts.h"
#include "pm_calendar.h"
#include "pm_calculator.h"
#include "pm_units.h"

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  TOUCH — GT911 capacitive via TAMC_GT911 (I2C)
//
//  The panel is physically 800x480 LANDSCAPE. We present it as
//  480x800 PORTRAIT via Arduino_RGB_Display rotation=3 in main.cpp.
//  The GT911 has no concept of our display rotation — it always
//  reports in its NATIVE panel coordinate system (0..799 along
//  the long axis, 0..479 along the short axis).
//
//  So we must rotate the touch coordinates ourselves to match the
//  portrait display. The transform below covers the four possible
//  orientations; if the first guess is wrong (taps land mirrored
//  or on the wrong axis), flip one of the three #defines below.
//
//  Mapping for rotation=3 (display rotated 270° CCW from native):
//      display_x = touch_y                      (short axis)
//      display_y = (NATIVE_LONG - 1) - touch_x  (long axis, mirrored)
//  This is the most common configuration for Sunton-style boards
//  mounted with the connector at the bottom.
//
//  If your taps land on the wrong horizontal half, flip
//  MAXINE_TOUCH_FLIP_X. If they land on the wrong vertical half,
//  flip MAXINE_TOUCH_FLIP_Y. If X/Y feel swapped (vertical drag
//  scrolls horizontally), flip MAXINE_TOUCH_SWAP_XY.
// ─────────────────────────────────────────────

// Native panel coordinate ranges (landscape — the GT911's frame).
#define MAXINE_TOUCH_NATIVE_W   800
#define MAXINE_TOUCH_NATIVE_H   480

// Calibration flags — flip these at first bring-up if needed.
#ifndef MAXINE_TOUCH_SWAP_XY
#define MAXINE_TOUCH_SWAP_XY    1   // swap native X/Y first
#endif
#ifndef MAXINE_TOUCH_FLIP_X
#define MAXINE_TOUCH_FLIP_X     0   // mirror final display X
#endif
#ifndef MAXINE_TOUCH_FLIP_Y
#define MAXINE_TOUCH_FLIP_Y     1   // mirror final display Y
#endif

// Construct TAMC_GT911 with NATIVE landscape dimensions so its
// internal clamping doesn't lose half the screen. We do our own
// mapping in maxine_touch_read(); ROTATION_NORMAL keeps the
// library out of the way.
static TAMC_GT911 ts = TAMC_GT911(PIN_TOUCH_SDA, PIN_TOUCH_SCL,
                                   PIN_TOUCH_INT, PIN_TOUCH_RST,
                                   MAXINE_TOUCH_NATIVE_W,
                                   MAXINE_TOUCH_NATIVE_H);
static bool maxine_touch_present = false;

static void maxine_touch_init() {
    ts.begin();
    ts.setRotation(ROTATION_NORMAL);
    maxine_touch_present = true;   // GT911 has no cheap presence ping
    Serial.println("[MAXINE] GT911 touch init (TAMC_GT911, native landscape)");
}

// Single-touch read. Returns true if a finger is down, writing
// LOGICAL PORTRAIT display coords (0..SCREEN_W-1, 0..SCREEN_H-1).
bool maxine_touch_read(int16_t* x, int16_t* y) {
    if (!maxine_touch_present) return false;
    ts.read();
    if (!ts.isTouched || ts.touches < 1) return false;

    // Raw GT911 coords — in the panel's native landscape frame.
    int rx = ts.points[0].x;
    int ry = ts.points[0].y;

    // Optional swap (X and Y axes exchanged at the controller level).
    if (MAXINE_TOUCH_SWAP_XY) {
        int tmp = rx; rx = ry; ry = tmp;
        // After swap, rx is in 0..NATIVE_H-1 and ry is in 0..NATIVE_W-1.
        // After the swap, the "natural" target is:
        //   display_x ← rx (mapped from 0..NATIVE_H-1 to 0..SCREEN_W-1)
        //   display_y ← ry (mapped from 0..NATIVE_W-1 to 0..SCREEN_H-1)
    }

    // Now scale into the logical portrait display rect.
    int src_w = MAXINE_TOUCH_SWAP_XY ? MAXINE_TOUCH_NATIVE_H : MAXINE_TOUCH_NATIVE_W;
    int src_h = MAXINE_TOUCH_SWAP_XY ? MAXINE_TOUCH_NATIVE_W : MAXINE_TOUCH_NATIVE_H;

    int dx = (int)((long)rx * SCREEN_W / src_w);
    int dy = (int)((long)ry * SCREEN_H / src_h);

    if (MAXINE_TOUCH_FLIP_X) dx = SCREEN_W - 1 - dx;
    if (MAXINE_TOUCH_FLIP_Y) dy = SCREEN_H - 1 - dy;

    if (dx < 0) dx = 0; if (dx >= SCREEN_W) dx = SCREEN_W - 1;
    if (dy < 0) dy = 0; if (dy >= SCREEN_H) dy = SCREEN_H - 1;

    // ── Calibration diagnostic: log raw -> mapped on each touch event.
    // Tap the four corners and the center; expected mapped values:
    //   top-left  : (~0,~0)        top-right : (~479,~0)
    //   bot-left  : (~0,~799)      bot-right : (~479,~799)
    //   center    : (~240,~400)
    // If mapped values don't match where you actually tapped, flip
    // the MAXINE_TOUCH_FLIP_X / FLIP_Y / SWAP_XY defines accordingly.
    // Remove once calibration is settled.
    static uint32_t _last_log = 0;
    if (millis() - _last_log > 100) {   // throttle to ~10 Hz so we don't flood
        Serial.printf("[TOUCH] raw=(%d,%d) -> map=(%d,%d)\n",
                       ts.points[0].x, ts.points[0].y, dx, dy);
        _last_log = millis();
    }

    *x = (int16_t)dx;
    *y = (int16_t)dy;
    return true;
}

// ─────────────────────────────────────────────
//  SETUP — called from main.cpp after gfx is alive.
//  I2C bus is already up in main.cpp; we init touch here.
// ─────────────────────────────────────────────
void maxine_setup() {
    Serial.println("[MAXINE] setup() — touch init entering");
    maxine_touch_init();
    Serial.println("[MAXINE] setup() — done");
}

// ─────────────────────────────────────────────
//  SPLASH — portrait, scaled for 480x800.
// ─────────────────────────────────────────────
void maxine_splash() {
    gfx->fillScreen(0x0000);
    const int CX = SCREEN_W / 2;

    const char* title = "Pisces Moon.";
    gfx->setTextSize(5);
    gfx->setTextColor(0x07FF);
    int tw = (int)strlen(title) * 30;   // size-5 ~30px/char
    gfx->setCursor((SCREEN_W - tw) / 2, SCREEN_H / 2 - 80);
    gfx->print(title);

    gfx->setTextSize(2);
    gfx->setTextColor(0xC618);
    const char* sub = "Powered by Gemini.";
    int sw = (int)strlen(sub) * 12;
    gfx->setCursor((SCREEN_W - sw) / 2, SCREEN_H / 2);
    gfx->print(sub);

    gfx->setTextSize(2);
    gfx->setTextColor(0x0480);
    const char* version = "v1.2.1 MAXINE";
    int vw = (int)strlen(version) * 12;
    gfx->setCursor((SCREEN_W - vw) / 2, SCREEN_H - 60);
    gfx->print(version);

    delay(2000);
}

// ─────────────────────────────────────────────
//  LAUNCHER — portrait touch grid (scaled from C28P 2x4 grid)
// ─────────────────────────────────────────────

struct MaxineTile {
    const char* label;
    uint16_t    color;
    uint16_t    color_dim;
};

static const MaxineTile MAXINE_CATEGORIES[] = {
    { "GAMES",    0xFC00, 0x3100 },   // orange    — 5 games
    { "TOOLS",    0xFFFF, 0x4208 },   // white     — reference reader
    { "WEATHER",  0xFFE0, 0x3300 },   // yellow    — lazy wifi
    { "RSS",      0x07E0, 0x0220 },   // green     — lazy wifi
    { "WARDRIVE", 0xFC00, 0x3100 },   // orange    — RF + anomaly
    { "WIFI",     0x07FF, 0x021F },   // cyan      — connection setup
    { "SYSTEM",   0x8410, 0x2104 },   // grey      — settings
    { "INFO",     0x07FF, 0x021F },   // cyan      — about
};
static const int MAXINE_NUM_CATEGORIES =
    sizeof(MAXINE_CATEGORIES) / sizeof(MaxineTile);

struct TileRect { int x, y, w, h; };
static TileRect maxine_tile_rects[MAXINE_NUM_CATEGORIES];

static void maxine_compute_layout() {
    const int header_h = SCREEN_H / 14;   // ~57px on 800
    const int footer_h = SCREEN_H / 20;   // ~40px
    const int margin_x = 16;
    const int gutter_x = 12;
    const int gutter_y = 12;

    int cols = 2;
    int rows = (MAXINE_NUM_CATEGORIES + cols - 1) / cols;   // 4

    int avail_w = SCREEN_W - 2 * margin_x;
    int avail_h = SCREEN_H - header_h - footer_h;
    int tile_w  = (avail_w - (cols - 1) * gutter_x) / cols;
    int tile_h  = (avail_h - (rows - 1) * gutter_y) / rows;

    for (int i = 0; i < MAXINE_NUM_CATEGORIES; i++) {
        int col = i % cols;
        int row = i / cols;
        maxine_tile_rects[i].x = margin_x + col * (tile_w + gutter_x);
        maxine_tile_rects[i].y = header_h + row * (tile_h + gutter_y);
        maxine_tile_rects[i].w = tile_w;
        maxine_tile_rects[i].h = tile_h;
    }
}

static void maxine_draw_header() {
    int h = SCREEN_H / 14;
    gfx->fillRect(0, 0, SCREEN_W, h, 0x0841);
    gfx->setTextColor(0x07E0);
    gfx->setTextSize(3);
    gfx->setCursor(16, (h - 24) / 2);
    gfx->print("PISCES MOON");
    gfx->setTextColor(WiFi.status() == WL_CONNECTED ? 0x07E0 : 0x4208);
    gfx->setTextSize(2);
    const char* wifi = (WiFi.status() == WL_CONNECTED) ? "WIFI:ON" : "WIFI:--";
    gfx->setCursor(SCREEN_W - (int)strlen(wifi) * 12 - 16, (h - 16) / 2);
    gfx->print(wifi);
}

static void maxine_draw_footer() {
    int fh = SCREEN_H / 20;
    int fy = SCREEN_H - fh;
    gfx->fillRect(0, fy, SCREEN_W, fh, 0x0841);
    gfx->setTextColor(0x4208);
    gfx->setTextSize(2);
    gfx->setCursor(16, fy + (fh - 16) / 2);
    gfx->print("Tap a category");
}

static void maxine_draw_tile(int idx, bool pressed) {
    const TileRect& r = maxine_tile_rects[idx];
    const MaxineTile& t = MAXINE_CATEGORIES[idx];
    uint16_t bg = pressed ? t.color : t.color_dim;
    uint16_t fg = pressed ? 0x0000 : t.color;

    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 12, bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 12, t.color);

    gfx->setTextSize(4);
    gfx->setTextColor(fg);
    int text_w = (int)strlen(t.label) * 24;   // size-4 ~24px/char
    gfx->setCursor(r.x + (r.w - text_w) / 2, r.y + (r.h - 32) / 2);
    gfx->print(t.label);
}

static void maxine_draw_all_tiles() {
    for (int i = 0; i < MAXINE_NUM_CATEGORIES; i++) maxine_draw_tile(i, false);
}

static int maxine_hit_test(int16_t tx, int16_t ty) {
    for (int i = 0; i < MAXINE_NUM_CATEGORIES; i++) {
        const TileRect& r = maxine_tile_rects[i];
        if (tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h)
            return i;
    }
    return -1;
}

// ─────────────────────────────────────────────
//  SUB-LAUNCHER — vertical list picker (scaled, paginated).
//  Same model as the C28P sub-launcher; 4 visible rows/page so
//  long lists paginate cleanly (the C28P lesson).
// ─────────────────────────────────────────────
struct MaxineSubItem {
    const char* label;
    void (*action)();
};

static void maxine_sub_launcher(const char* title,
                                const MaxineSubItem* items, int count) {
    const int title_h      = SCREEN_H / 10;          // ~80px
    const int back_y       = title_h + 8;
    const int back_h       = 56;
    const int item_y0      = back_y + back_h + 16;
    const int item_h       = 88;
    const int item_visible = 4;                       // paginate beyond 4
    const int margin       = 20;
    const int nav_h        = 56;
    const int nav_y        = SCREEN_H - nav_h - 16;

    int page = 0;
    int pages = (count + item_visible - 1) / item_visible;

    auto draw_chrome = [&]() {
        gfx->fillScreen(0x0000);
        gfx->setTextSize(4);
        gfx->setTextColor(0x07FF);
        gfx->setCursor(margin, 20);
        gfx->print(title);
        if (pages > 1) {
            gfx->setTextSize(2);
            gfx->setTextColor(0x8410);
            gfx->setCursor(SCREEN_W - 120, 30);
            gfx->printf("%d/%d", page + 1, pages);
        }
        gfx->drawFastHLine(0, title_h - 4, SCREEN_W, 0x4208);

        // Back button
        gfx->fillRect(margin, back_y, SCREEN_W - 2 * margin, back_h, 0x18C3);
        gfx->drawRect(margin, back_y, SCREEN_W - 2 * margin, back_h, 0x8410);
        gfx->setTextSize(3);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(margin + 20, back_y + (back_h - 24) / 2);
        gfx->print("< BACK");

        // Items (current page)
        int start = page * item_visible;
        int end = min(count, start + item_visible);
        for (int i = start; i < end; i++) {
            int row = i - start;
            int y = item_y0 + row * item_h;
            gfx->fillRect(margin, y, SCREEN_W - 2 * margin, item_h - 8, 0x2104);
            gfx->drawRect(margin, y, SCREEN_W - 2 * margin, item_h - 8, 0x4208);
            gfx->setTextSize(4);
            gfx->setTextColor(0xFFE0);
            gfx->setCursor(margin + 28, y + (item_h - 8 - 32) / 2);
            gfx->print(items[i].label);
        }

        if (pages > 1) {
            uint16_t prev_color = (page > 0) ? 0xFFE0 : 0x4208;
            gfx->fillRect(margin, nav_y, 180, nav_h, 0x18C3);
            gfx->drawRect(margin, nav_y, 180, nav_h, prev_color);
            gfx->setTextSize(3);
            gfx->setTextColor(prev_color);
            gfx->setCursor(margin + 50, nav_y + (nav_h - 24) / 2);
            gfx->print("PREV");

            uint16_t next_color = (page < pages - 1) ? 0xFFE0 : 0x4208;
            gfx->fillRect(SCREEN_W - margin - 180, nav_y, 180, nav_h, 0x18C3);
            gfx->drawRect(SCREEN_W - margin - 180, nav_y, 180, nav_h, next_color);
            gfx->setTextColor(next_color);
            gfx->setCursor(SCREEN_W - margin - 180 + 50, nav_y + (nav_h - 24) / 2);
            gfx->print("NEXT");
        }
    };

    draw_chrome();

    bool was_touched = false;
    int pressed_idx = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            if (ty >= back_y && ty < back_y + back_h &&
                tx >= margin && tx < SCREEN_W - margin) {
                pressed_idx = -1;
            } else if (pages > 1 && ty >= nav_y && ty < nav_y + nav_h) {
                if (tx >= margin && tx < margin + 180 && page > 0) {
                    pressed_idx = -3;
                } else if (tx >= SCREEN_W - margin - 180 && tx < SCREEN_W - margin &&
                           page < pages - 1) {
                    pressed_idx = -4;
                }
            } else if (tx >= margin && tx < SCREEN_W - margin) {
                int start = page * item_visible;
                int end = min(count, start + item_visible);
                for (int i = start; i < end; i++) {
                    int row = i - start;
                    int y = item_y0 + row * item_h;
                    if (ty >= y && ty < y + item_h - 8) { pressed_idx = i; break; }
                }
            }
        } else if (!touched && was_touched) {
            if (pressed_idx == -1) {
                return;
            } else if (pressed_idx == -3) {
                if (page > 0) { page--; draw_chrome(); }
            } else if (pressed_idx == -4) {
                if (page < pages - 1) { page++; draw_chrome(); }
            } else if (pressed_idx >= 0 && pressed_idx < count) {
                items[pressed_idx].action();
                draw_chrome();   // repaint after the app returns
            }
            pressed_idx = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  COMING SOON — honest placeholder for unported categories.
// ─────────────────────────────────────────────
static void maxine_coming_soon(const char* category) {
    gfx->fillScreen(0x0000);
    gfx->setTextSize(5);
    gfx->setTextColor(0xFC00);
    int tw = (int)strlen(category) * 30;
    gfx->setCursor((SCREEN_W - tw) / 2, SCREEN_H / 2 - 80);
    gfx->print(category);
    gfx->setTextSize(2);
    gfx->setTextColor(0x8410);
    gfx->setCursor((SCREEN_W - 14 * 12) / 2, SCREEN_H / 2);
    gfx->print("Coming in v1.3");
    gfx->setTextColor(0x4208);
    gfx->setCursor((SCREEN_W - 22 * 12) / 2, SCREEN_H - 80);
    gfx->print("Tap anywhere to return");

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);
        if (touched && !was_touched) return;
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  CATEGORY DISPATCH
// ─────────────────────────────────────────────
static void maxine_open_games() {
    static const MaxineSubItem items[] = {
        { "TETRIS",     run_tetris },
        { "SNAKE",      run_snake },
        { "PAC-MAN",    run_pacman },
        { "GALAGA",     run_galaga },
        { "POLE POS",   run_pole_position },
        { "MARIO BROS", run_mario_bros },
        { "BREAKOUT",   run_breakout },
        { "2048",       run_2048 },
        { "MINESWEEPER",run_minesweeper },
        { "CONNECT 4",  run_connect4 },
        { "SIMON",      run_simon },
        { "SOLITAIRE",  run_solitaire },
        { "ASTEROIDS",  run_asteroids },
        { "INVADERS",   run_space_invaders },
        { "FROGGER",    run_frogger },
        { "CHESS",      run_chess },
    };
    maxine_sub_launcher("GAMES", items, sizeof(items) / sizeof(items[0]));
}

static void maxine_tools_survival() { maxine_run_data_reader("survival", "SURVIVAL"); }
static void maxine_tools_medical()  { maxine_run_data_reader("medical",  "MEDICAL");  }
static void maxine_tools_history()  { maxine_run_data_reader("history",  "HISTORY"); }
extern void run_wifi_filemgr();

static void maxine_open_tools() {
    static const MaxineSubItem items[] = {
        { "CLOCK",     pm_run_clock          },
        { "TIMER",     pm_run_timer          },
        { "STOPWATCH", pm_run_stopwatch      },
        { "CALC",      pm_run_calculator     },
        { "UNITS",     pm_run_units          },
        { "NOTES",     pm_run_notes          },
        { "CONTACTS",  pm_run_contacts       },
        { "CALENDAR",  pm_run_calendar       },
        { "TRACKERS",  pm_run_tracker_scan   },
        { "SURVIVAL",  maxine_tools_survival },
        { "MEDICAL",   maxine_tools_medical  },
        { "HISTORY",   maxine_tools_history  },
        { "E-READER",  run_ereader            },
        { "SD FILES",  run_wifi_filemgr       },
    };
    maxine_sub_launcher("TOOLS", items, sizeof(items) / sizeof(items[0]));
}

// ─────────────────────────────────────────────
//  LAUNCHER — main category grid. Never returns.
// ─────────────────────────────────────────────
void maxine_launcher() {
    maxine_compute_layout();
    gfx->fillScreen(0x0000);
    maxine_draw_header();
    maxine_draw_footer();
    maxine_draw_all_tiles();

    bool was_touched = false;
    int pressed = -1;
    while (true) {
        int16_t tx, ty;
        bool touched = maxine_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            int hit = maxine_hit_test(tx, ty);
            if (hit >= 0) { pressed = hit; maxine_draw_tile(hit, true); }
        } else if (!touched && was_touched) {
            if (pressed >= 0) {
                maxine_draw_tile(pressed, false);
                const char* label = MAXINE_CATEGORIES[pressed].label;
                if      (strcmp(label, "GAMES")    == 0) maxine_open_games();
                else if (strcmp(label, "TOOLS")    == 0) maxine_open_tools();
                else if (strcmp(label, "WEATHER")  == 0) maxine_run_weather();
                else if (strcmp(label, "RSS")      == 0) maxine_run_rss();
                else if (strcmp(label, "WARDRIVE") == 0) maxine_run_wardrive();
                else if (strcmp(label, "WIFI")     == 0) maxine_run_wifi_setup();
                else if (strcmp(label, "SYSTEM")   == 0) maxine_run_system();
                else if (strcmp(label, "INFO")     == 0) maxine_run_about();
                else                                     maxine_coming_soon(label);
                // Repaint the grid after returning from a category.
                gfx->fillScreen(0x0000);
                maxine_draw_header();
                maxine_draw_footer();
                maxine_draw_all_tiles();
            }
            pressed = -1;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

#endif // DEVICE_MAXINE
