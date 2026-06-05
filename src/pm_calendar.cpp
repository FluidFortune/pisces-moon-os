// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_calendar.cpp — Touch-kiosk calendar for C28P + Maxine
//
//  TWO SCREENS:
//    1. MONTH VIEW — 7-column × 6-row grid, NTP-derived current
//       date highlighted. Days with notes get a small indicator
//       dot. Tap a day to open the day detail.
//    2. DAY DETAIL — read the note for that day (if any), with
//       ADD/EDIT NOTE and (when present) DELETE buttons.
//
//  STORAGE:
//    NoSQL category "calendar". One entry per day with a note.
//    Entry title is the date key "YYYY-MM-DD"; entry content is
//    the free-form note body. Append-only — edit creates a new
//    entry and tombstones the old (calendar_tombstones), exactly
//    like pm_notes and pm_contacts.
//
//    Loading the indicator dots for a month does ONE pass over
//    the entries: for each non-tombstoned, non-empty entry whose
//    title parses as YYYY-MM-DD within the currently displayed
//    month, set the corresponding bit. The result is a 32-bit
//    "days with notes" mask we re-render each repaint.
//
//  TIME:
//    Uses time(nullptr). If the device hasn't NTP-synced, that
//    returns ~0 (1970-01-01 UTC) — we still let the user navigate
//    months but show "NO NTP" in the chrome and don't highlight a
//    "today" cell.
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <time.h>
#include "pm_calendar.h"
#include "pm_text_input.h"
#include "nosql_store.h"

extern Arduino_GFX *gfx;

#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
static inline bool _cal_touch(int16_t* x, int16_t* y) { return c28p_touch_read(x, y); }
static const int CAL_W           = 240;
static const int CAL_H           = 320;
static const int CAL_EXIT_H      = 14;
static const int CAL_TITLE_Y     = 18;
static const int CAL_TITLE_H     = 24;
static const int CAL_GRID_Y      = 56;
static const int CAL_GRID_H      = 240;  // 6 rows × 40px = 240
static const int CAL_DAY_NAME_H  = 14;
static const int CAL_CELL_W      = 240 / 7;  // 34
static const int CAL_CELL_H      = (240 - CAL_DAY_NAME_H) / 6;  // ~37
static const int CAL_FOOTER_Y    = 300;
static const int CAL_FOOTER_H    = 20;
static const int CAL_TITLE_TS    = 2;
static const int CAL_CELL_TS     = 1;
static const int CAL_BODY_TS     = 1;
static const int CAL_BTN_TS      = 2;
#else  // DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
static inline bool _cal_touch(int16_t* x, int16_t* y) { return maxine_touch_read(x, y); }
static const int CAL_W           = 480;
static const int CAL_H           = 800;
static const int CAL_EXIT_H      = 32;
static const int CAL_TITLE_Y     = 42;
static const int CAL_TITLE_H     = 56;
static const int CAL_GRID_Y      = 108;
static const int CAL_GRID_H      = 564;  // 6 rows × 94px
static const int CAL_DAY_NAME_H  = 28;
static const int CAL_CELL_W      = 480 / 7;  // 68
static const int CAL_CELL_H      = (564 - CAL_DAY_NAME_H) / 6;  // ~89
static const int CAL_FOOTER_Y    = 680;
static const int CAL_FOOTER_H    = 64;
static const int CAL_TITLE_TS    = 4;
static const int CAL_CELL_TS     = 2;
static const int CAL_BODY_TS     = 2;
static const int CAL_BTN_TS      = 3;
#endif

// ─── Theme ───
static const uint16_t COL_BG       = 0x0000;
static const uint16_t COL_HEADER   = 0x0841;
static const uint16_t COL_ACCENT   = 0x07FF;   // cyan — calendar
static const uint16_t COL_TODAY    = 0xFD20;   // amber for "today" cell
static const uint16_t COL_HAS_NOTE = 0x07E0;   // green dot
static const uint16_t COL_RED      = 0xF800;
static const uint16_t COL_WHITE    = 0xFFFF;
static const uint16_t COL_DIM      = 0x4208;
static const uint16_t COL_TILE     = 0x18C3;

// ─── State ───
static int cal_year  = 2026;
static int cal_month = 1;       // 1..12
static int cal_today_year  = 0;
static int cal_today_month = 0;
static int cal_today_day   = 0;
static bool cal_ntp_synced = false;

// One bit per day-of-month (1..31). Bit (day-1) is set if that
// day has a non-tombstoned non-empty calendar entry.
static uint32_t cal_note_mask = 0;

// Tombstones for the calendar category, same model as pm_notes.
static const int CAL_TOMBSTONE_CAP = 256;
static int cal_tombstones[CAL_TOMBSTONE_CAP];
static int cal_tombstone_count = 0;

// ─── Date helpers ───
static int days_in_month(int m, int y) {
    static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    if (m < 1 || m > 12) return 30;
    return dim[m - 1];
}

// Zeller-style: returns 0=Sunday..6=Saturday for the 1st of the month.
static int first_dow(int m, int y) {
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100;
    int J = y / 100;
    int h = (1 + (13 * (m + 1)) / 5 + K + K/4 + J/4 + 5*J) % 7;
    // h: 0=Saturday, 1=Sunday … remap so 0=Sunday
    return (h + 6) % 7;
}

static void cal_load_today() {
    time_t now = time(nullptr);
    cal_ntp_synced = (now > 1700000000L);   // arbitrary "after Nov 2023" check
    if (!cal_ntp_synced) {
        cal_today_year  = 0;
        cal_today_month = 0;
        cal_today_day   = 0;
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    cal_today_year  = tm.tm_year + 1900;
    cal_today_month = tm.tm_mon + 1;
    cal_today_day   = tm.tm_mday;
}

// "YYYY-MM-DD" for the given y/m/d. Caller-supplied buffer ≥ 12.
static void format_key(int y, int m, int d, char* out, size_t outlen) {
    snprintf(out, outlen, "%04d-%02d-%02d", y, m, d);
}

// Parse "YYYY-MM-DD". Returns true on success.
static bool parse_key(const String& key, int& y, int& m, int& d) {
    if (key.length() != 10) return false;
    if (key[4] != '-' || key[7] != '-') return false;
    y = key.substring(0, 4).toInt();
    m = key.substring(5, 7).toInt();
    d = key.substring(8, 10).toInt();
    return (y >= 1900 && y <= 2999 && m >= 1 && m <= 12 && d >= 1 && d <= 31);
}

// ─── Tombstone helpers ───
static bool cal_is_tombstoned(int abs_idx) {
    for (int i = 0; i < cal_tombstone_count; i++) {
        if (cal_tombstones[i] == abs_idx) return true;
    }
    return false;
}

static void cal_load_tombstones() {
    cal_tombstone_count = 0;
    nosql_init("calendar_tombstones");
    int total = nosql_get_count("calendar_tombstones");
    String t, c;
    for (int i = 0; i < total && cal_tombstone_count < CAL_TOMBSTONE_CAP; i++) {
        if (!nosql_get_entry("calendar_tombstones", i, t, c)) continue;
        int idx = t.toInt();
        if (idx > 0 || t == "0") {
            cal_tombstones[cal_tombstone_count++] = idx;
        }
    }
}

static void cal_add_tombstone(int abs_idx) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", abs_idx);
    nosql_save_entry("calendar_tombstones", buf, "1");
    if (cal_tombstone_count < CAL_TOMBSTONE_CAP) {
        cal_tombstones[cal_tombstone_count++] = abs_idx;
    }
}

// ─── Note mask for displayed month ───
//
// Walk ALL entries and set the bit for any that fall in cal_year/cal_month.
// For each day, we want the NEWEST non-tombstoned entry — but for the
// month-grid indicator dot, "has any" is enough. Get-by-date uses a
// separate walk in cal_lookup_day() to return the newest live entry.
static void cal_refresh_note_mask() {
    cal_load_tombstones();
    nosql_init("calendar");
    cal_note_mask = 0;
    int total = nosql_get_count("calendar");
    String t, c;
    for (int abs_idx = 0; abs_idx < total; abs_idx++) {
        if (cal_is_tombstoned(abs_idx)) continue;
        if (!nosql_get_entry("calendar", abs_idx, t, c)) continue;
        if (c.length() == 0) continue;
        int y, m, d;
        if (!parse_key(t, y, m, d)) continue;
        if (y != cal_year || m != cal_month) continue;
        if (d < 1 || d > 31) continue;
        cal_note_mask |= (1UL << (d - 1));
    }
}

// Find the newest live entry for a specific date. Returns abs_idx or -1
// and writes the body into *out if found.
static int cal_lookup_day(int y, int m, int d, String* out) {
    char key[12];
    format_key(y, m, d, key, sizeof(key));
    int total = nosql_get_count("calendar");
    String t, c;
    int newest = -1;
    String newest_body;
    for (int abs_idx = 0; abs_idx < total; abs_idx++) {
        if (cal_is_tombstoned(abs_idx)) continue;
        if (!nosql_get_entry("calendar", abs_idx, t, c)) continue;
        if (t != key) continue;
        if (c.length() == 0) continue;
        // Latest abs_idx wins (NoSQL is append-ordered).
        newest = abs_idx;
        newest_body = c;
    }
    if (newest >= 0 && out) *out = newest_body;
    return newest;
}

// ─── Chrome ───
static void cal_draw_exit_strip() {
    gfx->fillRect(0, 0, CAL_W, CAL_EXIT_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, (CAL_EXIT_H - 8) / 2);
    gfx->print("< EXIT");
}

static const char* month_name(int m) {
    static const char* names[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    if (m < 1 || m > 12) return "???";
    return names[m - 1];
}

static void cal_wait_release() {
    int16_t x, y;
    while (_cal_touch(&x, &y)) { delay(20); yield(); }
}

// ─── Month view ───
//
// Layout:
//    Title bar: [<]  MONTH YEAR  [>]   (plus NO-NTP badge if applicable)
//    Grid:      S M T W T F S day names
//               6 rows × 7 cols of day numbers
//    Footer:    "Tap a day"
static void cal_draw_month() {
    gfx->fillScreen(COL_BG);
    cal_draw_exit_strip();

    // Title bar with prev/next chevrons.
    gfx->fillRect(0, CAL_TITLE_Y, CAL_W, CAL_TITLE_H, COL_HEADER);
    gfx->setTextSize(CAL_TITLE_TS);
    gfx->setTextColor(COL_ACCENT);
    // Left chevron
    gfx->setCursor(8, CAL_TITLE_Y + (CAL_TITLE_H - 8 * CAL_TITLE_TS) / 2);
    gfx->print("<");
    // Right chevron
    gfx->setCursor(CAL_W - 8 - 6 * CAL_TITLE_TS,
                   CAL_TITLE_Y + (CAL_TITLE_H - 8 * CAL_TITLE_TS) / 2);
    gfx->print(">");
    // Centered month + year
    char title[16];
    snprintf(title, sizeof(title), "%s %d", month_name(cal_month), cal_year);
    int tw = (int)strlen(title) * 6 * CAL_TITLE_TS;
    gfx->setCursor((CAL_W - tw) / 2,
                   CAL_TITLE_Y + (CAL_TITLE_H - 8 * CAL_TITLE_TS) / 2);
    gfx->print(title);

    // Day-name row
    int dn_y = CAL_GRID_Y;
    gfx->fillRect(0, dn_y, CAL_W, CAL_DAY_NAME_H, 0x0841);
    gfx->setTextSize(CAL_CELL_TS);
    gfx->setTextColor(COL_DIM);
    const char* dn[] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int i = 0; i < 7; i++) {
        int cx = i * CAL_CELL_W + (CAL_CELL_W - 6 * CAL_CELL_TS) / 2;
        gfx->setCursor(cx, dn_y + (CAL_DAY_NAME_H - 8 * CAL_CELL_TS) / 2);
        gfx->print(dn[i]);
    }

    // Day cells
    int grid_top = CAL_GRID_Y + CAL_DAY_NAME_H;
    int dom = days_in_month(cal_month, cal_year);
    int dow0 = first_dow(cal_month, cal_year);   // 0=Sun..6=Sat

    for (int day = 1; day <= dom; day++) {
        int cell_idx = dow0 + day - 1;
        int col = cell_idx % 7;
        int row = cell_idx / 7;
        if (row >= 6) break;
        int cx = col * CAL_CELL_W;
        int cy = grid_top + row * CAL_CELL_H;

        bool is_today = (cal_ntp_synced &&
                         day == cal_today_day &&
                         cal_month == cal_today_month &&
                         cal_year == cal_today_year);
        bool has_note = (cal_note_mask & (1UL << (day - 1))) != 0;

        // Cell background — slight tile so taps register visually.
        gfx->drawRect(cx + 1, cy + 1, CAL_CELL_W - 2, CAL_CELL_H - 2,
                      is_today ? COL_TODAY : 0x10A2);

        // Day number
        gfx->setTextSize(CAL_CELL_TS);
        gfx->setTextColor(is_today ? COL_TODAY : COL_WHITE);
        char dbuf[4];
        snprintf(dbuf, sizeof(dbuf), "%d", day);
        int dw = (int)strlen(dbuf) * 6 * CAL_CELL_TS;
        gfx->setCursor(cx + 4, cy + 4);
        gfx->print(dbuf);
        (void)dw;

        // Indicator dot bottom-right
        if (has_note) {
            int dot_r = (CAL_W >= 480) ? 5 : 3;
            gfx->fillCircle(cx + CAL_CELL_W - 6 - dot_r,
                            cy + CAL_CELL_H - 6 - dot_r,
                            dot_r, COL_HAS_NOTE);
        }
    }

    // Footer
    gfx->fillRect(0, CAL_FOOTER_Y, CAL_W, CAL_FOOTER_H, COL_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    if (!cal_ntp_synced) {
        gfx->setTextColor(COL_RED);
        gfx->setCursor(8, CAL_FOOTER_Y + (CAL_FOOTER_H - 8) / 2);
        gfx->print("NO NTP — clock not synced");
    } else {
        gfx->setCursor(8, CAL_FOOTER_Y + (CAL_FOOTER_H - 8) / 2);
        gfx->print("Tap a day to add or read a note");
    }
}

// Returns -1 if no day was hit, otherwise 1..31.
static int cal_hit_day(int tx, int ty) {
    int grid_top = CAL_GRID_Y + CAL_DAY_NAME_H;
    if (ty < grid_top) return -1;
    if (ty >= grid_top + 6 * CAL_CELL_H) return -1;
    if (tx < 0 || tx >= 7 * CAL_CELL_W) return -1;
    int col = tx / CAL_CELL_W;
    int row = (ty - grid_top) / CAL_CELL_H;
    int cell_idx = row * 7 + col;
    int dow0 = first_dow(cal_month, cal_year);
    int day = cell_idx - dow0 + 1;
    int dom = days_in_month(cal_month, cal_year);
    if (day < 1 || day > dom) return -1;
    return day;
}

// ─── Day detail screen ───
static void cal_draw_day(int year, int month, int day, const String& note) {
    gfx->fillScreen(COL_BG);
    cal_draw_exit_strip();

    // Title bar — date
    gfx->fillRect(0, CAL_TITLE_Y, CAL_W, CAL_TITLE_H, COL_HEADER);
    gfx->setTextSize(CAL_TITLE_TS);
    gfx->setTextColor(COL_ACCENT);
    char dbuf[24];
    snprintf(dbuf, sizeof(dbuf), "%s %d, %d", month_name(month), day, year);
    int dw = (int)strlen(dbuf) * 6 * CAL_TITLE_TS;
    gfx->setCursor((CAL_W - dw) / 2,
                   CAL_TITLE_Y + (CAL_TITLE_H - 8 * CAL_TITLE_TS) / 2);
    gfx->print(dbuf);

    // Body
    int body_top = CAL_TITLE_Y + CAL_TITLE_H + 16;
    int body_bottom = CAL_FOOTER_Y - 8;
    int line_h = 8 * CAL_BODY_TS + 4;
    int chars_per_line = (CAL_W - 16) / (6 * CAL_BODY_TS);

    if (note.length() == 0) {
        gfx->setTextSize(CAL_BODY_TS);
        gfx->setTextColor(COL_DIM);
        const char* msg = "No note for this day.";
        int w = (int)strlen(msg) * 6 * CAL_BODY_TS;
        gfx->setCursor((CAL_W - w) / 2, body_top + 40);
        gfx->print(msg);
    } else {
        gfx->setTextSize(CAL_BODY_TS);
        gfx->setTextColor(COL_WHITE);
        int x = 8;
        int y = body_top;
        int col = 0;
        gfx->setCursor(x, y);
        for (size_t i = 0; i < note.length() && y < body_bottom; i++) {
            char ch = note[i];
            if (ch == '\n' || col >= chars_per_line) {
                y += line_h;
                col = 0;
                gfx->setCursor(x, y);
                if (ch == '\n') continue;
            }
            gfx->write(ch);
            col++;
        }
    }

    // Footer: depends on whether note exists.
    //   If empty: [ADD NOTE] [BACK]
    //   If present: [DELETE] [EDIT] [BACK]
    int seg_w, seg_count;
    if (note.length() == 0) {
        seg_count = 2;
        seg_w = CAL_W / 2;
    } else {
        seg_count = 3;
        seg_w = CAL_W / 3;
    }
    gfx->fillRect(0, CAL_FOOTER_Y - CAL_FOOTER_H, CAL_W, CAL_FOOTER_H, COL_BG);

    int fy = CAL_FOOTER_Y - CAL_FOOTER_H + 4;
    int fh = CAL_FOOTER_H - 8;
    if (seg_count == 2) {
        gfx->fillRect(4, fy, seg_w - 8, fh, 0x0260);
        gfx->drawRect(4, fy, seg_w - 8, fh, COL_HAS_NOTE);
        gfx->setTextSize(CAL_BTN_TS);
        gfx->setTextColor(COL_HAS_NOTE);
        gfx->setCursor(seg_w / 2 - (3 * 6 * CAL_BTN_TS) / 2,
                       fy + (fh - 8 * CAL_BTN_TS) / 2);
        gfx->print("ADD");

        gfx->drawRect(seg_w + 4, fy, seg_w - 8, fh, COL_ACCENT);
        gfx->setTextColor(COL_ACCENT);
        gfx->setCursor(seg_w + seg_w / 2 - (4 * 6 * CAL_BTN_TS) / 2,
                       fy + (fh - 8 * CAL_BTN_TS) / 2);
        gfx->print("BACK");
    } else {
        gfx->drawRect(4, fy, seg_w - 8, fh, COL_RED);
        gfx->setTextSize(CAL_BTN_TS);
        gfx->setTextColor(COL_RED);
        gfx->setCursor(seg_w / 2 - (6 * 6 * CAL_BTN_TS) / 2,
                       fy + (fh - 8 * CAL_BTN_TS) / 2);
        gfx->print("DELETE");

        gfx->drawRect(seg_w + 4, fy, seg_w - 8, fh, COL_TODAY);
        gfx->setTextColor(COL_TODAY);
        gfx->setCursor(seg_w + seg_w / 2 - (4 * 6 * CAL_BTN_TS) / 2,
                       fy + (fh - 8 * CAL_BTN_TS) / 2);
        gfx->print("EDIT");

        gfx->drawRect(seg_w * 2 + 4, fy, seg_w - 8, fh, COL_ACCENT);
        gfx->setTextColor(COL_ACCENT);
        gfx->setCursor(seg_w * 2 + seg_w / 2 - (4 * 6 * CAL_BTN_TS) / 2,
                       fy + (fh - 8 * CAL_BTN_TS) / 2);
        gfx->print("BACK");
    }
}

// Open editor for a day's note. If old_abs_idx >= 0 we're editing
// (tombstone the old entry on save). Returns true if saved.
static bool cal_open_editor(int year, int month, int day, int old_abs_idx,
                            const String& initial) {
    char buf[768];
    char prompt[24];
    snprintf(prompt, sizeof(prompt), "%s %d:", month_name(month), day);
    if (!pm_text_input(prompt, buf, sizeof(buf), initial.c_str())) return false;
    if (buf[0] == 0) return false;

    char key[12];
    format_key(year, month, day, key, sizeof(key));
    if (!nosql_save_entry("calendar", key, buf)) return false;
    if (old_abs_idx >= 0) cal_add_tombstone(old_abs_idx);
    return true;
}

// Day-detail loop. Returns to caller when the user taps BACK or exit.
static void cal_open_day(int year, int month, int day) {
    String note;
    int abs_idx = cal_lookup_day(year, month, day, &note);
    cal_draw_day(year, month, day, note);

    bool was_touched = false;
    int pressed = -2;
    int seg_count = (note.length() == 0) ? 2 : 3;
    int seg_w = CAL_W / seg_count;
    int fy = CAL_FOOTER_Y - CAL_FOOTER_H + 4;
    int fh = CAL_FOOTER_H - 8;

    while (true) {
        int16_t tx, ty;
        bool touched = _cal_touch(&tx, &ty);
        if (touched && !was_touched) {
            if (ty < CAL_EXIT_H) {
                pressed = -1;
            } else if (ty >= fy && ty < fy + fh) {
                int col = tx / seg_w;
                if (col < 0) col = 0;
                if (col >= seg_count) col = seg_count - 1;
                if (seg_count == 2) {
                    pressed = (col == 0) ? 100 : 200;   // ADD / BACK
                } else {
                    if      (col == 0) pressed = 300;   // DELETE
                    else if (col == 1) pressed = 400;   // EDIT
                    else               pressed = 200;   // BACK
                }
            }
        } else if (!touched && was_touched) {
            if (pressed == -1 || pressed == 200) { cal_wait_release(); return; }

            if (pressed == 100) {
                // ADD
                if (cal_open_editor(year, month, day, -1, "")) {
                    cal_refresh_note_mask();
                    abs_idx = cal_lookup_day(year, month, day, &note);
                    seg_count = 3;
                    seg_w = CAL_W / 3;
                }
                cal_draw_day(year, month, day, note);
            } else if (pressed == 400) {
                // EDIT
                if (cal_open_editor(year, month, day, abs_idx, note)) {
                    cal_refresh_note_mask();
                    abs_idx = cal_lookup_day(year, month, day, &note);
                }
                cal_draw_day(year, month, day, note);
            } else if (pressed == 300) {
                // DELETE — inline confirm above the footer
                int cy0 = fy - CAL_FOOTER_H - 8;
                gfx->fillRect(8, cy0, CAL_W - 16, CAL_FOOTER_H, 0x3000);
                gfx->drawRect(8, cy0, CAL_W - 16, CAL_FOOTER_H, COL_RED);
                gfx->setTextSize(CAL_BTN_TS);
                gfx->setTextColor(COL_RED);
                gfx->setCursor(16, cy0 + (CAL_FOOTER_H - 8 * CAL_BTN_TS) / 2);
                gfx->print("Tap DELETE again");
                cal_wait_release();
                while (true) {
                    int16_t cx, cy;
                    if (_cal_touch(&cx, &cy)) {
                        if (cy >= fy && cy < fy + fh && cx < seg_w) {
                            cal_add_tombstone(abs_idx);
                            cal_refresh_note_mask();
                            note = "";
                            abs_idx = -1;
                            seg_count = 2;
                            seg_w = CAL_W / 2;
                            cal_wait_release();
                            cal_draw_day(year, month, day, note);
                            break;
                        }
                        cal_wait_release();
                        cal_draw_day(year, month, day, note);
                        break;
                    }
                    delay(20); yield();
                }
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

// ─── Month-view loop ───
void pm_run_calendar() {
    nosql_init("calendar");
    nosql_init("calendar_tombstones");
    cal_load_today();

    // Default to today's month if NTP synced, else stay on
    // whatever the previous session was on (statics persist).
    if (cal_ntp_synced) {
        cal_year  = cal_today_year;
        cal_month = cal_today_month;
    } else if (cal_year < 1900 || cal_year > 2999) {
        cal_year  = 2026;
        cal_month = 1;
    }

    cal_refresh_note_mask();
    cal_draw_month();

    bool was_touched = false;
    int pressed = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = _cal_touch(&tx, &ty);
        if (touched && !was_touched) {
            if (ty < CAL_EXIT_H) {
                pressed = -1;
            } else if (ty >= CAL_TITLE_Y && ty < CAL_TITLE_Y + CAL_TITLE_H) {
                // Title bar: < on left third, > on right third
                int third = CAL_W / 3;
                if (tx < third)            pressed = -2;   // prev month
                else if (tx >= 2 * third)  pressed = -3;   // next month
            } else {
                int day = cal_hit_day(tx, ty);
                if (day >= 1) pressed = 1000 + day;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) { cal_wait_release(); return; }
            else if (pressed == -2) {
                cal_month--;
                if (cal_month < 1) { cal_month = 12; cal_year--; }
                cal_refresh_note_mask();
                cal_draw_month();
            } else if (pressed == -3) {
                cal_month++;
                if (cal_month > 12) { cal_month = 1; cal_year++; }
                cal_refresh_note_mask();
                cal_draw_month();
            } else if (pressed >= 1000) {
                int day = pressed - 1000;
                cal_open_day(cal_year, cal_month, day);
                cal_refresh_note_mask();
                cal_draw_month();
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

#endif  // DEVICE_C28P || DEVICE_MAXINE
