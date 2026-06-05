// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_notes.cpp — Quick notes for C28P + Maxine (touch kiosks)
//
//  Three screens:
//    1. LIST  — scrollable list of saved notes, NEW button
//    2. VIEW  — read a note in full, EDIT / DELETE / BACK
//    3. EDIT  — pm_text_input() handles typing
//
//  Storage: NoSQL "notes" category, append-only entries (one per
//  save). Each entry's NoSQL "title" is the derived first line of
//  the body; "content" is the full body.
//
//  Delete model:
//    NoSQL 1.0 has no per-entry remove API (only category-wide
//    clear). To support delete from the UI we maintain a parallel
//    "notes_tombstones" category — each tombstone is an entry
//    whose TITLE is the integer string of a deleted note's
//    absolute index in the "notes" category. The list builder
//    reads all tombstones into an in-memory set at refresh time
//    and filters those indices out. Edit = save new + tombstone
//    old, so both operations route through the same path.
//
//    Tombstones survive reboots (they're on SD) and the FACTORY
//    RESET path's nosql_clear_category("notes_tombstones") would
//    need to be added if you want a clean wipe — for now, factory
//    reset clears "notes" but tombstones for those indices become
//    harmless (no note for them to hide).
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pm_notes.h"
#include "pm_text_input.h"
#include "nosql_store.h"

extern Arduino_GFX *gfx;

#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
static inline bool _nt_touch(int16_t* x, int16_t* y) { return c28p_touch_read(x, y); }
static const int NT_W            = 240;
static const int NT_H            = 320;
static const int NT_EXIT_H       = 14;
static const int NT_TITLE_Y      = 18;
static const int NT_TITLE_H      = 22;
static const int NT_LIST_Y       = 44;
static const int NT_LIST_ROW_H   = 36;
static const int NT_LIST_ROWS    = 6;
static const int NT_FOOTER_Y     = 268;
static const int NT_FOOTER_H     = 44;
static const int NT_TITLE_TS     = 2;
static const int NT_BODY_TS      = 1;
static const int NT_BTN_TS       = 2;
#else  // DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
static inline bool _nt_touch(int16_t* x, int16_t* y) { return maxine_touch_read(x, y); }
static const int NT_W            = 480;
static const int NT_H            = 800;
static const int NT_EXIT_H       = 32;
static const int NT_TITLE_Y      = 42;
static const int NT_TITLE_H      = 56;
static const int NT_LIST_Y       = 112;
static const int NT_LIST_ROW_H   = 72;
static const int NT_LIST_ROWS    = 8;
static const int NT_FOOTER_Y     = 700;
static const int NT_FOOTER_H     = 88;
static const int NT_TITLE_TS     = 4;
static const int NT_BODY_TS      = 2;
static const int NT_BTN_TS       = 3;
#endif

// ─── Theme ───
static const uint16_t COL_BG       = 0x0000;
static const uint16_t COL_HEADER   = 0x0841;
static const uint16_t COL_ACCENT   = 0x07E0;
static const uint16_t COL_AMBER    = 0xFD20;
static const uint16_t COL_RED      = 0xF800;
static const uint16_t COL_WHITE    = 0xFFFF;
static const uint16_t COL_DIM      = 0x4208;
static const uint16_t COL_TILE     = 0x18C3;
static const uint16_t COL_HILITE   = 0x07FF;

// ─── Caches ───
// In-list title + abs_idx pairs for the visible page. Re-filled
// on every refresh_cache(). Capacity is the per-page row count.
static String  nt_title_cache[16];
static int     nt_index_cache[16];
static int     nt_cache_count = 0;
static int     nt_scroll_top  = 0;
static int     nt_visible_total = 0;

// In-memory set of tombstoned indices, populated from the
// "notes_tombstones" category on every refresh. Cap at 256
// distinct deletions per session; if a user manages more than
// that, they should factory-reset.
static const int NT_TOMBSTONE_CAP = 256;
static int nt_tombstones[NT_TOMBSTONE_CAP];
static int nt_tombstone_count = 0;

static bool nt_is_tombstoned(int abs_idx) {
    for (int i = 0; i < nt_tombstone_count; i++) {
        if (nt_tombstones[i] == abs_idx) return true;
    }
    return false;
}

static void nt_load_tombstones() {
    nt_tombstone_count = 0;
    nosql_init("notes_tombstones");
    int total = nosql_get_count("notes_tombstones");
    String t, c;
    for (int i = 0; i < total && nt_tombstone_count < NT_TOMBSTONE_CAP; i++) {
        if (!nosql_get_entry("notes_tombstones", i, t, c)) continue;
        int idx = t.toInt();   // NoSQL title is the abs_idx of the deleted note
        if (idx > 0 || t == "0") {   // toInt() returns 0 for non-numeric AND "0"
            nt_tombstones[nt_tombstone_count++] = idx;
        }
    }
}

static void nt_add_tombstone(int abs_idx) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", abs_idx);
    nosql_save_entry("notes_tombstones", buf, "1");
    if (nt_tombstone_count < NT_TOMBSTONE_CAP) {
        nt_tombstones[nt_tombstone_count++] = abs_idx;
    }
}

// Derive the title line from a note body — first chars up to the
// first newline or a width-limited cutoff.
static String derive_title(const String& body) {
    String head = body;
    int nl = head.indexOf('\n');
    if (nl >= 0) head = head.substring(0, nl);
    head.trim();
    int maxlen = (NT_W >= 480) ? 40 : 28;
    if (head.length() > (size_t)maxlen) head = head.substring(0, maxlen);
    if (head.length() == 0) head = "(empty)";
    return head;
}

// Build the page cache: newest-first iteration, skipping tombstoned
// indices and empty bodies.
static void refresh_cache() {
    nt_load_tombstones();
    nosql_init("notes");
    int total = nosql_get_count("notes");
    nt_cache_count = 0;
    int seen_visible = 0;
    String t, c;
    nt_visible_total = 0;

    for (int abs_idx = total - 1; abs_idx >= 0; abs_idx--) {
        if (nt_is_tombstoned(abs_idx)) continue;
        if (!nosql_get_entry("notes", abs_idx, t, c)) continue;
        if (c.length() == 0) continue;
        if (seen_visible >= nt_scroll_top &&
            nt_cache_count < NT_LIST_ROWS) {
            nt_title_cache[nt_cache_count] = derive_title(c);
            nt_index_cache[nt_cache_count] = abs_idx;
            nt_cache_count++;
        }
        seen_visible++;
    }
    nt_visible_total = seen_visible;
}

// ─── Chrome ───
static void nt_draw_exit_strip() {
    gfx->fillRect(0, 0, NT_W, NT_EXIT_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, (NT_EXIT_H - 8) / 2);
    gfx->print("< EXIT");
}

static void nt_draw_title_bar(const char* label, const char* hint) {
    gfx->fillRect(0, NT_TITLE_Y, NT_W, NT_TITLE_H, COL_HEADER);
    gfx->setTextSize(NT_TITLE_TS);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, NT_TITLE_Y + (NT_TITLE_H - 8 * NT_TITLE_TS) / 2);
    gfx->print(label);
    if (hint && hint[0]) {
        gfx->setTextSize(1);
        gfx->setTextColor(COL_DIM);
        int hw = (int)strlen(hint) * 6;
        gfx->setCursor(NT_W - hw - 8, NT_TITLE_Y + (NT_TITLE_H - 8) / 2);
        gfx->print(hint);
    }
}

static void nt_wait_release() {
    int16_t x, y;
    while (_nt_touch(&x, &y)) { delay(20); yield(); }
}

// ─── List screen ───
static void nt_draw_list() {
    gfx->fillScreen(COL_BG);
    nt_draw_exit_strip();
    char hint[24];
    snprintf(hint, sizeof(hint), "%d notes", nt_visible_total);
    nt_draw_title_bar("NOTES", hint);

    if (nt_cache_count == 0) {
        gfx->setTextSize(NT_BTN_TS);
        gfx->setTextColor(COL_DIM);
        const char* msg = "No notes yet";
        int w = (int)strlen(msg) * 6 * NT_BTN_TS;
        gfx->setCursor((NT_W - w) / 2, NT_LIST_Y + 60);
        gfx->print(msg);

        gfx->setTextSize(1);
        gfx->setTextColor(COL_DIM);
        const char* msg2 = "Tap NEW to create one.";
        int w2 = (int)strlen(msg2) * 6;
        gfx->setCursor((NT_W - w2) / 2, NT_LIST_Y + 60 + 20 * NT_BTN_TS);
        gfx->print(msg2);
    } else {
        for (int i = 0; i < nt_cache_count; i++) {
            int y = NT_LIST_Y + i * NT_LIST_ROW_H;
            gfx->fillRect(8, y, NT_W - 16, NT_LIST_ROW_H - 6, COL_TILE);
            gfx->drawRect(8, y, NT_W - 16, NT_LIST_ROW_H - 6, COL_DIM);
            gfx->setTextSize(NT_BODY_TS);
            gfx->setTextColor(COL_WHITE);
            gfx->setCursor(16, y + (NT_LIST_ROW_H - 6 - 8 * NT_BODY_TS) / 2);
            gfx->print(nt_title_cache[i]);
        }
    }

    // Footer: [PREV] [NEW] [NEXT]
    int seg_w = NT_W / 3;
    gfx->fillRect(0, NT_FOOTER_Y, NT_W, NT_FOOTER_H, COL_BG);

    bool can_prev = nt_scroll_top > 0;
    uint16_t prev_col = can_prev ? COL_HILITE : COL_DIM;
    gfx->drawRect(4, NT_FOOTER_Y + 4, seg_w - 8, NT_FOOTER_H - 8, prev_col);
    gfx->setTextSize(NT_BTN_TS);
    gfx->setTextColor(prev_col);
    gfx->setCursor(seg_w / 2 - (4 * 6 * NT_BTN_TS) / 2,
                   NT_FOOTER_Y + (NT_FOOTER_H - 8 * NT_BTN_TS) / 2);
    gfx->print("PREV");

    gfx->fillRect(seg_w + 4, NT_FOOTER_Y + 4, seg_w - 8, NT_FOOTER_H - 8, 0x0260);
    gfx->drawRect(seg_w + 4, NT_FOOTER_Y + 4, seg_w - 8, NT_FOOTER_H - 8, COL_ACCENT);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(seg_w + seg_w / 2 - (3 * 6 * NT_BTN_TS) / 2,
                   NT_FOOTER_Y + (NT_FOOTER_H - 8 * NT_BTN_TS) / 2);
    gfx->print("NEW");

    bool can_next = (nt_scroll_top + nt_cache_count) < nt_visible_total;
    uint16_t next_col = can_next ? COL_HILITE : COL_DIM;
    gfx->drawRect(seg_w * 2 + 4, NT_FOOTER_Y + 4, seg_w - 8, NT_FOOTER_H - 8, next_col);
    gfx->setTextColor(next_col);
    gfx->setCursor(seg_w * 2 + seg_w / 2 - (4 * 6 * NT_BTN_TS) / 2,
                   NT_FOOTER_Y + (NT_FOOTER_H - 8 * NT_BTN_TS) / 2);
    gfx->print("NEXT");
}

// ─── View screen ───
static void nt_draw_view(const String& body) {
    gfx->fillScreen(COL_BG);
    nt_draw_exit_strip();
    nt_draw_title_bar("READ", "");

    int body_top = NT_TITLE_Y + NT_TITLE_H + 8;
    int body_bottom = NT_FOOTER_Y - 8;
    int line_h = 8 * NT_BODY_TS + 4;
    int chars_per_line = (NT_W - 16) / (6 * NT_BODY_TS);
    int x = 8;
    int y = body_top;
    int col = 0;
    gfx->setTextSize(NT_BODY_TS);
    gfx->setTextColor(COL_WHITE);
    gfx->setCursor(x, y);
    for (size_t i = 0; i < body.length(); i++) {
        char c = body[i];
        if (y > body_bottom) break;
        if (c == '\n' || col >= chars_per_line) {
            y += line_h;
            col = 0;
            gfx->setCursor(x, y);
            if (c == '\n') continue;
        }
        gfx->write(c);
        col++;
    }

    // Footer: [DELETE] [EDIT] [BACK]
    int seg_w = NT_W / 3;
    gfx->fillRect(0, NT_FOOTER_Y, NT_W, NT_FOOTER_H, COL_BG);
    gfx->drawRect(4, NT_FOOTER_Y + 4, seg_w - 8, NT_FOOTER_H - 8, COL_RED);
    gfx->setTextSize(NT_BTN_TS);
    gfx->setTextColor(COL_RED);
    gfx->setCursor(seg_w / 2 - (6 * 6 * NT_BTN_TS) / 2,
                   NT_FOOTER_Y + (NT_FOOTER_H - 8 * NT_BTN_TS) / 2);
    gfx->print("DELETE");

    gfx->drawRect(seg_w + 4, NT_FOOTER_Y + 4, seg_w - 8, NT_FOOTER_H - 8, COL_AMBER);
    gfx->setTextColor(COL_AMBER);
    gfx->setCursor(seg_w + seg_w / 2 - (4 * 6 * NT_BTN_TS) / 2,
                   NT_FOOTER_Y + (NT_FOOTER_H - 8 * NT_BTN_TS) / 2);
    gfx->print("EDIT");

    gfx->drawRect(seg_w * 2 + 4, NT_FOOTER_Y + 4, seg_w - 8, NT_FOOTER_H - 8, COL_HILITE);
    gfx->setTextColor(COL_HILITE);
    gfx->setCursor(seg_w * 2 + seg_w / 2 - (4 * 6 * NT_BTN_TS) / 2,
                   NT_FOOTER_Y + (NT_FOOTER_H - 8 * NT_BTN_TS) / 2);
    gfx->print("BACK");
}

// Open the text input editor for a new or existing note.
// If old_abs_idx >= 0, the existing entry is tombstoned and the
// edited body is saved as a new entry. Returns true if anything
// was saved (including a fresh create); false on cancel.
static bool nt_open_editor(int old_abs_idx) {
    char buf[1024];
    String initial = "";
    if (old_abs_idx >= 0) {
        String t, c;
        if (nosql_get_entry("notes", old_abs_idx, t, c)) initial = c;
    }
    bool confirmed = pm_text_input("NOTE:", buf, sizeof(buf), initial.c_str());
    if (!confirmed) return false;
    if (buf[0] == 0) return false;
    String body(buf);
    String title = derive_title(body);
    if (!nosql_save_entry("notes", title.c_str(), body.c_str())) return false;
    if (old_abs_idx >= 0) nt_add_tombstone(old_abs_idx);
    return true;
}

// View an existing note. Returns:
//   0 = back (no change), 1 = edited (refresh), 2 = deleted (refresh)
static int nt_open_view(int abs_idx) {
    String title, body;
    if (!nosql_get_entry("notes", abs_idx, title, body)) return 0;
    nt_draw_view(body);

    bool was_touched = false;
    int pressed = -2;
    int seg_w = NT_W / 3;
    while (true) {
        int16_t tx, ty;
        bool touched = _nt_touch(&tx, &ty);
        if (touched && !was_touched) {
            if (ty < NT_EXIT_H) {
                pressed = -1;
            } else if (ty >= NT_FOOTER_Y && ty < NT_FOOTER_Y + NT_FOOTER_H) {
                if      (tx < seg_w)       pressed = 0;
                else if (tx < seg_w * 2)   pressed = 1;
                else                       pressed = 2;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1 || pressed == 2) { nt_wait_release(); return 0; }
            if (pressed == 0) {
                // Inline confirm step
                gfx->fillRect(0, NT_FOOTER_Y - 60, NT_W, 56, COL_BG);
                gfx->fillRect(8, NT_FOOTER_Y - 56, NT_W - 16, 48, 0x3000);
                gfx->drawRect(8, NT_FOOTER_Y - 56, NT_W - 16, 48, COL_RED);
                gfx->setTextSize(NT_BTN_TS);
                gfx->setTextColor(COL_RED);
                gfx->setCursor(16, NT_FOOTER_Y - 56 + 16);
                gfx->print("Tap DELETE to confirm");
                nt_wait_release();
                while (true) {
                    int16_t cx, cy;
                    if (_nt_touch(&cx, &cy)) {
                        if (cy >= NT_FOOTER_Y && cy < NT_FOOTER_Y + NT_FOOTER_H &&
                            cx < seg_w) {
                            nt_add_tombstone(abs_idx);
                            nt_wait_release();
                            return 2;
                        }
                        nt_wait_release();
                        nt_draw_view(body);
                        break;
                    }
                    delay(20); yield();
                }
            } else if (pressed == 1) {
                if (nt_open_editor(abs_idx)) {
                    nt_wait_release();
                    return 1;
                }
                nt_draw_view(body);
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

// ─── Entry point ───
void pm_run_notes() {
    nosql_init("notes");
    nosql_init("notes_tombstones");
    nt_scroll_top = 0;
    refresh_cache();
    nt_draw_list();

    bool was_touched = false;
    int pressed = -2;
    int seg_w = NT_W / 3;
    while (true) {
        int16_t tx, ty;
        bool touched = _nt_touch(&tx, &ty);

        if (touched && !was_touched) {
            if (ty < NT_EXIT_H) {
                pressed = -1;
            } else if (ty >= NT_LIST_Y &&
                       ty < NT_LIST_Y + NT_LIST_ROWS * NT_LIST_ROW_H) {
                int row = (ty - NT_LIST_Y) / NT_LIST_ROW_H;
                if (row >= 0 && row < nt_cache_count) pressed = 1000 + row;
            } else if (ty >= NT_FOOTER_Y && ty < NT_FOOTER_Y + NT_FOOTER_H) {
                if      (tx < seg_w)       pressed = -10;
                else if (tx < seg_w * 2)   pressed = -11;
                else                       pressed = -12;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) { nt_wait_release(); return; }
            else if (pressed == -10) {
                if (nt_scroll_top > 0) {
                    nt_scroll_top -= NT_LIST_ROWS;
                    if (nt_scroll_top < 0) nt_scroll_top = 0;
                    refresh_cache();
                    nt_draw_list();
                }
            } else if (pressed == -11) {
                if (nt_open_editor(-1)) {
                    nt_scroll_top = 0;
                    refresh_cache();
                }
                nt_draw_list();
            } else if (pressed == -12) {
                if (nt_scroll_top + nt_cache_count < nt_visible_total) {
                    nt_scroll_top += NT_LIST_ROWS;
                    refresh_cache();
                    nt_draw_list();
                }
            } else if (pressed >= 1000) {
                int row = pressed - 1000;
                if (row < nt_cache_count) {
                    int abs_idx = nt_index_cache[row];
                    int act = nt_open_view(abs_idx);
                    if (act != 0) refresh_cache();
                    nt_draw_list();
                }
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

#endif  // DEVICE_C28P || DEVICE_MAXINE
