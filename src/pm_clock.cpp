// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_clock.cpp — Clock / Timer / Stopwatch for C28P + Maxine
//
//  TOUCH KIOSK ONLY. The T-Deck/Pager/Cardputer clock.cpp file
//  already exists with keyboard-driven controls — we don't
//  collide with it. This file lives under DEVICE_C28P/MAXINE.
//
//  LAYOUT (portrait, 240×320 base):
//    y=  0..14   Top exit bar (tap to leave)
//    y= 16..56   Tab bar: [CLOCK] [TIMER] [STOPWATCH]
//    y= 60..256  Active panel (huge digit display)
//    y=260..316  Action buttons (per-tab)
//
//  Maxine doubles all dimensions and bumps text sizes — the
//  geometry block at top makes that automatic.
//
//  TIME SOURCE:
//    On boot, if WiFi is connected, we sntp_sync once. The wall
//    clock then runs off time(nullptr) which the IDF advances
//    internally. No RTC, so a reboot without WiFi loses the
//    sync — we show "NTP: --" in that case and the clock display
//    shows uptime hh:mm:ss instead of real time.
//
//  PERSISTENCE: none. Timer/stopwatch state is in-memory only,
//  intentionally — these are short-lived utilities, not journals.
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <time.h>
#include "pm_clock.h"

extern Arduino_GFX *gfx;

#if defined(DEVICE_C28P) || defined(DEVICE_C5)
#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
static inline bool _ck_touch(int16_t* x, int16_t* y) { return c28p_touch_read(x, y); }
#else  // DEVICE_C5
extern bool c5_touch_read(int16_t* x, int16_t* y);
static inline bool _ck_touch(int16_t* x, int16_t* y) { return c5_touch_read(x, y); }
#endif
static const int CK_W            = 240;
static const int CK_H            = 320;
static const int CK_EXIT_H       = 14;
static const int CK_TAB_Y        = 18;
static const int CK_TAB_H        = 38;
static const int CK_PANEL_Y      = 60;
static const int CK_PANEL_H      = 196;
static const int CK_BTN_Y        = 262;
static const int CK_BTN_H        = 48;
static const int CK_BIG_TEXT     = 5;     // big-digit display size
static const int CK_LBL_TEXT     = 1;
static const int CK_BTN_TEXT     = 2;
static const int CK_CHAR_W       = 6;
static const int CK_CHAR_H       = 8;
#else  // DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
static inline bool _ck_touch(int16_t* x, int16_t* y) { return maxine_touch_read(x, y); }
static const int CK_W            = 480;
static const int CK_H            = 800;
static const int CK_EXIT_H       = 36;
static const int CK_TAB_Y        = 44;
static const int CK_TAB_H        = 80;
static const int CK_PANEL_Y      = 140;
static const int CK_PANEL_H      = 540;
static const int CK_BTN_Y        = 690;
static const int CK_BTN_H        = 96;
static const int CK_BIG_TEXT     = 10;
static const int CK_LBL_TEXT     = 2;
static const int CK_BTN_TEXT     = 4;
static const int CK_CHAR_W       = 6;
static const int CK_CHAR_H       = 8;
#endif

// ─── Theme ───
static const uint16_t COL_BG       = 0x0000;
static const uint16_t COL_HEADER   = 0x0841;
static const uint16_t COL_ACCENT   = 0x07FF;   // cyan
static const uint16_t COL_GREEN    = 0x07E0;
static const uint16_t COL_AMBER    = 0xFD20;
static const uint16_t COL_RED      = 0xF800;
static const uint16_t COL_WHITE    = 0xFFFF;
static const uint16_t COL_DIM      = 0x4208;
static const uint16_t COL_TILE     = 0x18C3;

// ─── State ───
enum CkTab { CK_TAB_CLOCK = 0, CK_TAB_TIMER = 1, CK_TAB_STOPWATCH = 2 };
static CkTab    ck_tab = CK_TAB_CLOCK;
static bool     ck_ntp_synced = false;

// Timer state
static uint32_t timer_set_seconds  = 5 * 60;   // default 5:00
static uint32_t timer_remaining_ms = 0;
static uint32_t timer_last_tick_ms = 0;
static bool     timer_running      = false;
static bool     timer_alarming     = false;
static uint32_t timer_alarm_start  = 0;

// Stopwatch state
static uint32_t sw_elapsed_ms      = 0;
static uint32_t sw_last_start_ms   = 0;
static bool     sw_running         = false;
static uint32_t sw_laps[6]         = {0};
static int      sw_lap_count       = 0;

// Forward decls
static void ck_draw_tabs();
static void ck_draw_panel();
static void ck_draw_buttons();
static void ck_full_redraw();

// ─── NTP sync ───
static void ck_try_ntp() {
    if (ck_ntp_synced) return;
    if (WiFi.status() != WL_CONNECTED) return;
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    // Don't block — the next time(nullptr) call returns the synced
    // value once SNTP completes. Just mark optimistic.
    ck_ntp_synced = true;
    Serial.println("[CLOCK] NTP sync initiated");
}

// ─── Drawing helpers ───
static void ck_draw_centered(const char* s, int y, int tsize, uint16_t color) {
    gfx->setTextSize(tsize);
    gfx->setTextColor(color);
    int tw = (int)strlen(s) * CK_CHAR_W * tsize;
    gfx->setCursor((CK_W - tw) / 2, y);
    gfx->print(s);
}

static void ck_draw_exit_bar() {
    gfx->fillRect(0, 0, CK_W, CK_EXIT_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(CK_W - 50, (CK_EXIT_H - 8) / 2);
    gfx->print("< EXIT");
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, (CK_EXIT_H - 8) / 2);
    gfx->print("CLOCK");
}

static void ck_draw_tabs() {
    gfx->fillRect(0, CK_TAB_Y, CK_W, CK_TAB_H, COL_BG);
    const char* labels[3] = { "CLOCK", "TIMER", "STOPWATCH" };
    int tab_w = CK_W / 3;
    for (int i = 0; i < 3; i++) {
        int x = i * tab_w;
        bool active = (i == (int)ck_tab);
        uint16_t bg = active ? COL_ACCENT : COL_TILE;
        uint16_t fg = active ? 0x0000 : COL_ACCENT;
        gfx->fillRect(x + 2, CK_TAB_Y + 2, tab_w - 4, CK_TAB_H - 4, bg);
        gfx->drawRect(x + 2, CK_TAB_Y + 2, tab_w - 4, CK_TAB_H - 4, COL_ACCENT);
        gfx->setTextSize(CK_LBL_TEXT);
        gfx->setTextColor(fg);
        int tw = (int)strlen(labels[i]) * CK_CHAR_W * CK_LBL_TEXT;
        gfx->setCursor(x + (tab_w - tw) / 2,
                       CK_TAB_Y + (CK_TAB_H - CK_CHAR_H * CK_LBL_TEXT) / 2);
        gfx->print(labels[i]);
    }
}

// Format helpers — single-buffer return so callers can chain prints
static const char* fmt_hms(uint32_t total_seconds, char* buf) {
    uint32_t h = total_seconds / 3600;
    uint32_t m = (total_seconds % 3600) / 60;
    uint32_t s = total_seconds % 60;
    snprintf(buf, 16, "%02lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)s);
    return buf;
}
static const char* fmt_mss(uint32_t total_ms, char* buf) {
    uint32_t m = total_ms / 60000;
    uint32_t s = (total_ms / 1000) % 60;
    uint32_t cs = (total_ms % 1000) / 10;
    snprintf(buf, 16, "%02lu:%02lu.%02lu",
             (unsigned long)m, (unsigned long)s, (unsigned long)cs);
    return buf;
}

// ─── Panels ───
static void ck_draw_clock_panel() {
    gfx->fillRect(0, CK_PANEL_Y, CK_W, CK_PANEL_H, COL_BG);

    time_t now = time(nullptr);
    char buf[16];
    if (now > 100000UL) {
        // We have a real time. Convert to local broken-down.
        struct tm tinfo;
        localtime_r(&now, &tinfo);
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
        ck_draw_centered(buf, CK_PANEL_Y + CK_PANEL_H / 2 - (CK_CHAR_H * CK_BIG_TEXT) / 2,
                         CK_BIG_TEXT, COL_GREEN);
        // Date below
        char dbuf[32];
        snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d (UTC)",
                 tinfo.tm_year + 1900, tinfo.tm_mon + 1, tinfo.tm_mday);
        ck_draw_centered(dbuf, CK_PANEL_Y + CK_PANEL_H / 2 + (CK_CHAR_H * CK_BIG_TEXT) / 2 + 12,
                         CK_LBL_TEXT, COL_WHITE);
        ck_draw_centered("NTP synced", CK_PANEL_Y + 8, CK_LBL_TEXT, COL_DIM);
    } else {
        // No NTP — show uptime instead
        uint32_t up = millis() / 1000;
        fmt_hms(up, buf);
        ck_draw_centered(buf, CK_PANEL_Y + CK_PANEL_H / 2 - (CK_CHAR_H * CK_BIG_TEXT) / 2,
                         CK_BIG_TEXT, COL_AMBER);
        ck_draw_centered("UPTIME (no NTP)",
                         CK_PANEL_Y + CK_PANEL_H / 2 + (CK_CHAR_H * CK_BIG_TEXT) / 2 + 12,
                         CK_LBL_TEXT, COL_WHITE);
        const char* hint = (WiFi.status() == WL_CONNECTED)
                           ? "Waiting for NTP..."
                           : "Connect WiFi for real time";
        ck_draw_centered(hint, CK_PANEL_Y + 8, CK_LBL_TEXT, COL_DIM);
    }
}

static void ck_draw_timer_panel() {
    gfx->fillRect(0, CK_PANEL_Y, CK_W, CK_PANEL_H, COL_BG);

    char buf[16];
    uint32_t show_sec = timer_running ? (timer_remaining_ms / 1000) : timer_set_seconds;
    fmt_hms(show_sec, buf);
    uint16_t color = timer_alarming ? COL_RED :
                     (timer_running ? COL_GREEN : COL_AMBER);
    ck_draw_centered(buf, CK_PANEL_Y + CK_PANEL_H / 2 - (CK_CHAR_H * CK_BIG_TEXT) / 2,
                     CK_BIG_TEXT, color);

    if (timer_alarming) {
        ck_draw_centered("TIME'S UP — tap RESET",
                         CK_PANEL_Y + CK_PANEL_H / 2 + (CK_CHAR_H * CK_BIG_TEXT) / 2 + 12,
                         CK_LBL_TEXT, COL_RED);
    } else if (!timer_running) {
        ck_draw_centered("tap +/- to set",
                         CK_PANEL_Y + 8, CK_LBL_TEXT, COL_DIM);
        // +/- buttons below the digits for adjust
        int row_y = CK_PANEL_Y + CK_PANEL_H - 48;
        // -1m  -10s  +10s  +1m
        const char* lbl[4] = { "-1m", "-10s", "+10s", "+1m" };
        int bw = (CK_W - 40) / 4;
        for (int i = 0; i < 4; i++) {
            int x = 16 + i * (bw + 4);
            gfx->fillRect(x, row_y, bw, 36, COL_TILE);
            gfx->drawRect(x, row_y, bw, 36, COL_ACCENT);
            gfx->setTextSize(CK_LBL_TEXT);
            gfx->setTextColor(COL_ACCENT);
            int tw = (int)strlen(lbl[i]) * CK_CHAR_W * CK_LBL_TEXT;
            gfx->setCursor(x + (bw - tw) / 2,
                           row_y + (36 - CK_CHAR_H * CK_LBL_TEXT) / 2);
            gfx->print(lbl[i]);
        }
    } else {
        ck_draw_centered("running", CK_PANEL_Y + 8, CK_LBL_TEXT, COL_GREEN);
    }
}

static void ck_draw_stopwatch_panel() {
    gfx->fillRect(0, CK_PANEL_Y, CK_W, CK_PANEL_H, COL_BG);

    uint32_t now = sw_running ? (sw_elapsed_ms + (millis() - sw_last_start_ms))
                              : sw_elapsed_ms;
    char buf[16];
    fmt_mss(now, buf);
    ck_draw_centered(buf, CK_PANEL_Y + 12,
                     CK_BIG_TEXT, sw_running ? COL_GREEN : COL_AMBER);

    // Laps
    gfx->setTextSize(CK_LBL_TEXT);
    int lap_y = CK_PANEL_Y + CK_PANEL_H - (sw_lap_count * (CK_CHAR_H * CK_LBL_TEXT + 4)) - 8;
    if (lap_y < CK_PANEL_Y + 12 + CK_CHAR_H * CK_BIG_TEXT + 8) {
        lap_y = CK_PANEL_Y + 12 + CK_CHAR_H * CK_BIG_TEXT + 8;
    }
    for (int i = 0; i < sw_lap_count; i++) {
        char lbuf[24];
        char tbuf[16];
        fmt_mss(sw_laps[i], tbuf);
        snprintf(lbuf, sizeof(lbuf), "LAP %d  %s", i + 1, tbuf);
        gfx->setTextColor(COL_WHITE);
        gfx->setCursor(20, lap_y + i * (CK_CHAR_H * CK_LBL_TEXT + 4));
        gfx->print(lbuf);
    }
}

static void ck_draw_panel() {
    if      (ck_tab == CK_TAB_CLOCK)     ck_draw_clock_panel();
    else if (ck_tab == CK_TAB_TIMER)     ck_draw_timer_panel();
    else                                 ck_draw_stopwatch_panel();
}

// ─── Button rows ───
// Returns the labels for the current tab's bottom action row.
// Empty string means "no button at this slot".
static void ck_button_labels(const char* out[3]) {
    out[0] = ""; out[1] = ""; out[2] = "";
    if (ck_tab == CK_TAB_CLOCK) {
        out[0] = "RESYNC NTP";
    } else if (ck_tab == CK_TAB_TIMER) {
        if (timer_alarming) {
            out[1] = "RESET";
        } else if (timer_running) {
            out[0] = "PAUSE";
            out[2] = "RESET";
        } else {
            out[0] = "START";
            out[2] = "RESET";
        }
    } else {  // stopwatch
        if (sw_running) {
            out[0] = "PAUSE";
            out[1] = "LAP";
            out[2] = "RESET";
        } else {
            out[0] = sw_elapsed_ms > 0 ? "RESUME" : "START";
            out[2] = "RESET";
        }
    }
}

static void ck_draw_buttons() {
    gfx->fillRect(0, CK_BTN_Y, CK_W, CK_BTN_H, COL_BG);
    const char* labels[3];
    ck_button_labels(labels);
    int bw = (CK_W - 24) / 3;
    for (int i = 0; i < 3; i++) {
        if (!labels[i][0]) continue;
        int x = 8 + i * (bw + 4);
        uint16_t color = COL_ACCENT;
        if (strcmp(labels[i], "RESET") == 0)  color = COL_RED;
        if (strcmp(labels[i], "START") == 0 ||
            strcmp(labels[i], "RESUME") == 0) color = COL_GREEN;
        if (strcmp(labels[i], "LAP") == 0)    color = COL_AMBER;
        gfx->fillRect(x, CK_BTN_Y, bw, CK_BTN_H, COL_TILE);
        gfx->drawRect(x, CK_BTN_Y, bw, CK_BTN_H, color);
        gfx->setTextSize(CK_BTN_TEXT);
        gfx->setTextColor(color);
        int tw = (int)strlen(labels[i]) * CK_CHAR_W * CK_BTN_TEXT;
        gfx->setCursor(x + (bw - tw) / 2,
                       CK_BTN_Y + (CK_BTN_H - CK_CHAR_H * CK_BTN_TEXT) / 2);
        gfx->print(labels[i]);
    }
}

static void ck_full_redraw() {
    gfx->fillScreen(COL_BG);
    ck_draw_exit_bar();
    ck_draw_tabs();
    ck_draw_panel();
    ck_draw_buttons();
}

// ─── Action dispatch ───
static void ck_handle_button(int slot) {
    if (ck_tab == CK_TAB_CLOCK) {
        if (slot == 0) {
            ck_ntp_synced = false;
            ck_try_ntp();
        }
    } else if (ck_tab == CK_TAB_TIMER) {
        if (timer_alarming) {
            if (slot == 1) {
                timer_alarming = false;
                timer_remaining_ms = timer_set_seconds * 1000UL;
            }
        } else if (timer_running) {
            if (slot == 0) {
                timer_running = false;
            } else if (slot == 2) {
                timer_running = false;
                timer_remaining_ms = timer_set_seconds * 1000UL;
            }
        } else {
            if (slot == 0) {
                if (timer_remaining_ms == 0) {
                    timer_remaining_ms = timer_set_seconds * 1000UL;
                }
                timer_last_tick_ms = millis();
                timer_running = true;
            } else if (slot == 2) {
                timer_remaining_ms = 0;
                timer_set_seconds = 5 * 60;
            }
        }
    } else {  // stopwatch
        if (sw_running) {
            if (slot == 0) {
                sw_elapsed_ms += millis() - sw_last_start_ms;
                sw_running = false;
            } else if (slot == 1) {
                if (sw_lap_count < 6) {
                    sw_laps[sw_lap_count++] =
                        sw_elapsed_ms + (millis() - sw_last_start_ms);
                }
            } else if (slot == 2) {
                sw_running = false;
                sw_elapsed_ms = 0;
                sw_lap_count = 0;
            }
        } else {
            if (slot == 0) {
                sw_last_start_ms = millis();
                sw_running = true;
            } else if (slot == 2) {
                sw_elapsed_ms = 0;
                sw_lap_count = 0;
            }
        }
    }
}

static void ck_handle_timer_adjust(int kind) {
    // kind: 0=-1m 1=-10s 2=+10s 3=+1m
    int32_t s = (int32_t)timer_set_seconds;
    if      (kind == 0) s -= 60;
    else if (kind == 1) s -= 10;
    else if (kind == 2) s += 10;
    else if (kind == 3) s += 60;
    if (s < 10) s = 10;
    if (s > 99 * 3600 + 59 * 60 + 59) s = 99 * 3600 + 59 * 60 + 59;
    timer_set_seconds = (uint32_t)s;
}

// ─── Public entry ───
//
// Three entry points all funnel into ck_main_loop(). Each one sets
// the initial tab so a launcher item for TIMER/STOPWATCH opens the
// user straight onto that tab — the alternative (always opening on
// CLOCK and making them swipe over) added friction with no benefit.
static void ck_main_loop() {
    Serial.println("[CLOCK] entry");
    ck_try_ntp();
    ck_full_redraw();

    bool was_touched = false;
    int  press_zone  = -1;  // -1 nothing, -2 exit, -3 tab0, -4 tab1, -5 tab2,
                            // 0..2 button slot, 10..13 timer-adjust slot
    uint32_t last_clock_refresh = 0;

    while (true) {
        uint32_t now = millis();

        // Refresh the live display every 200ms-ish (10cs precision
        // on stopwatch needs ~50ms; clock face is 1Hz; timer is 1Hz).
        uint32_t refresh_period = (ck_tab == CK_TAB_STOPWATCH) ? 80 : 250;
        if (now - last_clock_refresh > refresh_period) {
            ck_draw_panel();
            ck_draw_buttons();   // labels may change when timer fires
            last_clock_refresh = now;
        }

        // Tick the timer
        if (timer_running && !timer_alarming) {
            uint32_t dt = now - timer_last_tick_ms;
            timer_last_tick_ms = now;
            if (dt >= timer_remaining_ms) {
                timer_remaining_ms = 0;
                timer_running = false;
                timer_alarming = true;
                timer_alarm_start = now;
                Serial.println("[CLOCK] timer fired");
            } else {
                timer_remaining_ms -= dt;
            }
        }
        // Auto-stop alarm visual after 60s so it doesn't strobe forever
        if (timer_alarming && (now - timer_alarm_start > 60000)) {
            timer_alarming = false;
            timer_remaining_ms = timer_set_seconds * 1000UL;
        }

        // Touch
        int16_t tx, ty;
        bool touched = _ck_touch(&tx, &ty);

        if (touched && !was_touched) {
            press_zone = -1;
            if (ty < CK_EXIT_H) {
                press_zone = -2;
            } else if (ty >= CK_TAB_Y && ty < CK_TAB_Y + CK_TAB_H) {
                int tab_w = CK_W / 3;
                int t = tx / tab_w;
                if (t < 0) t = 0; if (t > 2) t = 2;
                press_zone = -3 - t;
            } else if (ty >= CK_BTN_Y && ty < CK_BTN_Y + CK_BTN_H) {
                int bw = (CK_W - 24) / 3;
                if      (tx < 8 + bw)            press_zone = 0;
                else if (tx < 8 + bw + 4 + bw)   press_zone = 1;
                else                              press_zone = 2;
            } else if (ck_tab == CK_TAB_TIMER && !timer_running && !timer_alarming) {
                int row_y = CK_PANEL_Y + CK_PANEL_H - 48;
                if (ty >= row_y && ty < row_y + 36) {
                    int bw2 = (CK_W - 40) / 4;
                    int slot = (tx - 16) / (bw2 + 4);
                    if (slot >= 0 && slot < 4) press_zone = 10 + slot;
                }
            }
        } else if (!touched && was_touched) {
            int z = press_zone;
            press_zone = -1;
            if (z == -2) {
                gfx->fillScreen(COL_BG);
                return;
            } else if (z <= -3 && z >= -5) {
                ck_tab = (CkTab)(-3 - z);
                ck_full_redraw();
            } else if (z >= 0 && z <= 2) {
                ck_handle_button(z);
                ck_full_redraw();
            } else if (z >= 10 && z <= 13) {
                ck_handle_timer_adjust(z - 10);
                ck_draw_panel();
            }
        }
        was_touched = touched;
        delay(15);
        yield();
    }
}

void pm_run_clock()     { ck_tab = CK_TAB_CLOCK;     ck_main_loop(); }
void pm_run_timer()     { ck_tab = CK_TAB_TIMER;     ck_main_loop(); }
void pm_run_stopwatch() { ck_tab = CK_TAB_STOPWATCH; ck_main_loop(); }

#endif // DEVICE_C28P || DEVICE_MAXINE || DEVICE_C5
