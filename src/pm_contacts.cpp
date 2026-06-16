// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_contacts.cpp — Address book for C28P + Maxine (touch kiosks)
//
//  Three screens, same shape as pm_notes:
//    1. LIST  — alphabetical contacts, NEW button
//    2. VIEW  — full record: name, phone, email, note
//    3. EDIT  — four sequential pm_text_input() calls; cancel
//               at any field aborts the whole edit
//
//  Storage: NoSQL "contacts" category. Per-entry layout:
//    title   = display name
//    content = "phone|email|note"   (pipe-delimited record)
//
//  Pipes inside fields are remapped to "/" on save (silent).
//  Newlines in the note field stay as-is — they'll be word-
//  wrapped by the view renderer.
//
//  Delete model: identical to pm_notes — a "contacts_tombstones"
//  category tracks abs_idx strings, refresh_cache() filters them
//  out. NoSQL has no per-entry remove API in v1.2.1.
// ─────────────────────────────────────────────

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE) || defined(DEVICE_C5)

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pm_contacts.h"
#include "pm_text_input.h"
#include "nosql_store.h"

extern Arduino_GFX *gfx;

#if defined(DEVICE_C28P) || defined(DEVICE_C5)
#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
static inline bool _ct_touch(int16_t* x, int16_t* y) { return c28p_touch_read(x, y); }
#else  // DEVICE_C5
extern bool c5_touch_read(int16_t* x, int16_t* y);
static inline bool _ct_touch(int16_t* x, int16_t* y) { return c5_touch_read(x, y); }
#endif
static const int CT_W            = 240;
static const int CT_H            = 320;
static const int CT_EXIT_H       = 14;
static const int CT_TITLE_Y      = 18;
static const int CT_TITLE_H      = 22;
static const int CT_LIST_Y       = 44;
static const int CT_LIST_ROW_H   = 36;
static const int CT_LIST_ROWS    = 6;
static const int CT_FOOTER_Y     = 268;
static const int CT_FOOTER_H     = 44;
static const int CT_TITLE_TS     = 2;
static const int CT_BODY_TS      = 1;
static const int CT_BTN_TS       = 2;
#else  // DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
static inline bool _ct_touch(int16_t* x, int16_t* y) { return maxine_touch_read(x, y); }
static const int CT_W            = 480;
static const int CT_H            = 800;
static const int CT_EXIT_H       = 32;
static const int CT_TITLE_Y      = 42;
static const int CT_TITLE_H      = 56;
static const int CT_LIST_Y       = 112;
static const int CT_LIST_ROW_H   = 72;
static const int CT_LIST_ROWS    = 8;
static const int CT_FOOTER_Y     = 700;
static const int CT_FOOTER_H     = 88;
static const int CT_TITLE_TS     = 4;
static const int CT_BODY_TS      = 2;
static const int CT_BTN_TS       = 3;
#endif

static const uint16_t COL_BG       = 0x0000;
static const uint16_t COL_HEADER   = 0x0841;
static const uint16_t COL_ACCENT   = 0xFD20;   // amber — contacts
static const uint16_t COL_RED      = 0xF800;
static const uint16_t COL_WHITE    = 0xFFFF;
static const uint16_t COL_DIM      = 0x4208;
static const uint16_t COL_TILE     = 0x18C3;
static const uint16_t COL_HILITE   = 0x07FF;
static const uint16_t COL_GREEN    = 0x07E0;

// ─── State ───
struct CtRow { String name; int abs_idx; };
static CtRow ct_cache[16];
static int   ct_cache_count = 0;
static int   ct_scroll_top  = 0;
static int   ct_visible_total = 0;

static const int CT_TOMBSTONE_CAP = 256;
static int ct_tombstones[CT_TOMBSTONE_CAP];
static int ct_tombstone_count = 0;

// ─── Tombstone helpers ───
static bool ct_is_tombstoned(int abs_idx) {
    for (int i = 0; i < ct_tombstone_count; i++) {
        if (ct_tombstones[i] == abs_idx) return true;
    }
    return false;
}

static void ct_load_tombstones() {
    ct_tombstone_count = 0;
    nosql_init("contacts_tombstones");
    int total = nosql_get_count("contacts_tombstones");
    String t, c;
    for (int i = 0; i < total && ct_tombstone_count < CT_TOMBSTONE_CAP; i++) {
        if (!nosql_get_entry("contacts_tombstones", i, t, c)) continue;
        int idx = t.toInt();
        if (idx > 0 || t == "0") {
            ct_tombstones[ct_tombstone_count++] = idx;
        }
    }
}

static void ct_add_tombstone(int abs_idx) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", abs_idx);
    nosql_save_entry("contacts_tombstones", buf, "1");
    if (ct_tombstone_count < CT_TOMBSTONE_CAP) {
        ct_tombstones[ct_tombstone_count++] = abs_idx;
    }
}

// ─── Record encoding/decoding ───
static void sanitize_field(String& s) {
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] == '|') s.setCharAt(i, '/');
    }
}

// content = "phone|email|note" — split on first two pipes.
static void decode_record(const String& content,
                          String& phone, String& email, String& note) {
    int p1 = content.indexOf('|');
    int p2 = (p1 >= 0) ? content.indexOf('|', p1 + 1) : -1;
    if (p1 < 0) {
        phone = content; email = ""; note = "";
    } else if (p2 < 0) {
        phone = content.substring(0, p1);
        email = content.substring(p1 + 1);
        note = "";
    } else {
        phone = content.substring(0, p1);
        email = content.substring(p1 + 1, p2);
        note  = content.substring(p2 + 1);
    }
}

static String encode_record(String phone, String email, String note) {
    sanitize_field(phone);
    sanitize_field(email);
    // Note field allows newlines but not pipes.
    sanitize_field(note);
    return phone + "|" + email + "|" + note;
}

// ─── Cache build ───
//
// We want alphabetical-by-name display, but NoSQL is insertion-
// ordered. Read every (non-tombstoned, non-empty) entry into a
// scratch buffer, sort by name, then paginate. 200-entry cap
// keeps the sort cheap — beyond that the kiosk isn't the right
// tool.
static void refresh_cache() {
    ct_load_tombstones();
    nosql_init("contacts");
    int total = nosql_get_count("contacts");
    ct_cache_count = 0;
    ct_visible_total = 0;

    static const int SCRATCH_CAP = 200;
    static CtRow scratch[SCRATCH_CAP];
    int scratch_count = 0;
    String t, c;
    for (int abs_idx = 0; abs_idx < total && scratch_count < SCRATCH_CAP; abs_idx++) {
        if (ct_is_tombstoned(abs_idx)) continue;
        if (!nosql_get_entry("contacts", abs_idx, t, c)) continue;
        if (t.length() == 0) continue;
        scratch[scratch_count].name = t;
        scratch[scratch_count].abs_idx = abs_idx;
        scratch_count++;
    }

    // Simple insertion sort by name (case-insensitive). N <= 200, fine.
    for (int i = 1; i < scratch_count; i++) {
        CtRow key = scratch[i];
        int j = i - 1;
        while (j >= 0) {
            String a = scratch[j].name; a.toLowerCase();
            String b = key.name;        b.toLowerCase();
            if (a <= b) break;
            scratch[j + 1] = scratch[j];
            j--;
        }
        scratch[j + 1] = key;
    }

    ct_visible_total = scratch_count;
    int start = ct_scroll_top;
    int end = start + CT_LIST_ROWS;
    if (end > scratch_count) end = scratch_count;
    for (int i = start; i < end && ct_cache_count < CT_LIST_ROWS; i++) {
        ct_cache[ct_cache_count++] = scratch[i];
    }
}

// ─── Chrome ───
static void ct_draw_exit_strip() {
    gfx->fillRect(0, 0, CT_W, CT_EXIT_H, COL_HEADER);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, (CT_EXIT_H - 8) / 2);
    gfx->print("< EXIT");
}

static void ct_draw_title_bar(const char* label, const char* hint) {
    gfx->fillRect(0, CT_TITLE_Y, CT_W, CT_TITLE_H, COL_HEADER);
    gfx->setTextSize(CT_TITLE_TS);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, CT_TITLE_Y + (CT_TITLE_H - 8 * CT_TITLE_TS) / 2);
    gfx->print(label);
    if (hint && hint[0]) {
        gfx->setTextSize(1);
        gfx->setTextColor(COL_DIM);
        int hw = (int)strlen(hint) * 6;
        gfx->setCursor(CT_W - hw - 8, CT_TITLE_Y + (CT_TITLE_H - 8) / 2);
        gfx->print(hint);
    }
}

static void ct_wait_release() {
    int16_t x, y;
    while (_ct_touch(&x, &y)) { delay(20); yield(); }
}

// ─── List screen ───
static void ct_draw_list() {
    gfx->fillScreen(COL_BG);
    ct_draw_exit_strip();
    char hint[32];
    snprintf(hint, sizeof(hint), "%d / %d", ct_cache_count, ct_visible_total);
    ct_draw_title_bar("CONTACTS", hint);

    if (ct_cache_count == 0) {
        gfx->setTextSize(CT_BTN_TS);
        gfx->setTextColor(COL_DIM);
        const char* msg = "No contacts yet";
        int w = (int)strlen(msg) * 6 * CT_BTN_TS;
        gfx->setCursor((CT_W - w) / 2, CT_LIST_Y + 60);
        gfx->print(msg);
    } else {
        for (int i = 0; i < ct_cache_count; i++) {
            int y = CT_LIST_Y + i * CT_LIST_ROW_H;
            gfx->fillRect(8, y, CT_W - 16, CT_LIST_ROW_H - 6, COL_TILE);
            gfx->drawRect(8, y, CT_W - 16, CT_LIST_ROW_H - 6, COL_DIM);
            gfx->setTextSize(CT_BODY_TS);
            gfx->setTextColor(COL_WHITE);
            gfx->setCursor(16, y + (CT_LIST_ROW_H - 6 - 8 * CT_BODY_TS) / 2);
            gfx->print(ct_cache[i].name);
        }
    }

    int seg_w = CT_W / 3;
    gfx->fillRect(0, CT_FOOTER_Y, CT_W, CT_FOOTER_H, COL_BG);

    bool can_prev = ct_scroll_top > 0;
    uint16_t prev_col = can_prev ? COL_HILITE : COL_DIM;
    gfx->drawRect(4, CT_FOOTER_Y + 4, seg_w - 8, CT_FOOTER_H - 8, prev_col);
    gfx->setTextSize(CT_BTN_TS);
    gfx->setTextColor(prev_col);
    gfx->setCursor(seg_w / 2 - (4 * 6 * CT_BTN_TS) / 2,
                   CT_FOOTER_Y + (CT_FOOTER_H - 8 * CT_BTN_TS) / 2);
    gfx->print("PREV");

    gfx->fillRect(seg_w + 4, CT_FOOTER_Y + 4, seg_w - 8, CT_FOOTER_H - 8, 0x2104);
    gfx->drawRect(seg_w + 4, CT_FOOTER_Y + 4, seg_w - 8, CT_FOOTER_H - 8, COL_ACCENT);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(seg_w + seg_w / 2 - (3 * 6 * CT_BTN_TS) / 2,
                   CT_FOOTER_Y + (CT_FOOTER_H - 8 * CT_BTN_TS) / 2);
    gfx->print("NEW");

    bool can_next = (ct_scroll_top + ct_cache_count) < ct_visible_total;
    uint16_t next_col = can_next ? COL_HILITE : COL_DIM;
    gfx->drawRect(seg_w * 2 + 4, CT_FOOTER_Y + 4, seg_w - 8, CT_FOOTER_H - 8, next_col);
    gfx->setTextColor(next_col);
    gfx->setCursor(seg_w * 2 + seg_w / 2 - (4 * 6 * CT_BTN_TS) / 2,
                   CT_FOOTER_Y + (CT_FOOTER_H - 8 * CT_BTN_TS) / 2);
    gfx->print("NEXT");
}

// ─── View screen ───
static void ct_draw_view(const String& name, const String& phone,
                         const String& email, const String& note) {
    gfx->fillScreen(COL_BG);
    ct_draw_exit_strip();
    ct_draw_title_bar("CONTACT", "");

    int y = CT_TITLE_Y + CT_TITLE_H + 12;

    // Name — big
    gfx->setTextSize(CT_TITLE_TS);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, y);
    gfx->print(name);
    y += 8 * CT_TITLE_TS + 12;

    gfx->drawFastHLine(8, y, CT_W - 16, COL_DIM);
    y += 12;

    // Phone
    gfx->setTextSize(CT_BODY_TS);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(8, y);
    gfx->print("PHONE");
    y += 8 * CT_BODY_TS + 2;
    gfx->setTextColor(COL_GREEN);
    gfx->setCursor(8, y);
    gfx->print(phone.length() ? phone : String("(none)"));
    y += 8 * CT_BODY_TS + 10;

    // Email
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(8, y);
    gfx->print("EMAIL");
    y += 8 * CT_BODY_TS + 2;
    gfx->setTextColor(COL_HILITE);
    gfx->setCursor(8, y);
    gfx->print(email.length() ? email : String("(none)"));
    y += 8 * CT_BODY_TS + 10;

    // Note — wrap
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(8, y);
    gfx->print("NOTE");
    y += 8 * CT_BODY_TS + 4;
    gfx->setTextColor(COL_WHITE);
    int line_h = 8 * CT_BODY_TS + 4;
    int chars_per_line = (CT_W - 16) / (6 * CT_BODY_TS);
    int col = 0;
    int body_bottom = CT_FOOTER_Y - 8;
    gfx->setCursor(8, y);
    for (size_t i = 0; i < note.length() && y < body_bottom; i++) {
        char ch = note[i];
        if (ch == '\n' || col >= chars_per_line) {
            y += line_h;
            col = 0;
            gfx->setCursor(8, y);
            if (ch == '\n') continue;
        }
        gfx->write(ch);
        col++;
    }

    // Footer
    int seg_w = CT_W / 3;
    gfx->fillRect(0, CT_FOOTER_Y, CT_W, CT_FOOTER_H, COL_BG);
    gfx->drawRect(4, CT_FOOTER_Y + 4, seg_w - 8, CT_FOOTER_H - 8, COL_RED);
    gfx->setTextSize(CT_BTN_TS);
    gfx->setTextColor(COL_RED);
    gfx->setCursor(seg_w / 2 - (6 * 6 * CT_BTN_TS) / 2,
                   CT_FOOTER_Y + (CT_FOOTER_H - 8 * CT_BTN_TS) / 2);
    gfx->print("DELETE");

    gfx->drawRect(seg_w + 4, CT_FOOTER_Y + 4, seg_w - 8, CT_FOOTER_H - 8, COL_ACCENT);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(seg_w + seg_w / 2 - (4 * 6 * CT_BTN_TS) / 2,
                   CT_FOOTER_Y + (CT_FOOTER_H - 8 * CT_BTN_TS) / 2);
    gfx->print("EDIT");

    gfx->drawRect(seg_w * 2 + 4, CT_FOOTER_Y + 4, seg_w - 8, CT_FOOTER_H - 8, COL_HILITE);
    gfx->setTextColor(COL_HILITE);
    gfx->setCursor(seg_w * 2 + seg_w / 2 - (4 * 6 * CT_BTN_TS) / 2,
                   CT_FOOTER_Y + (CT_FOOTER_H - 8 * CT_BTN_TS) / 2);
    gfx->print("BACK");
}

// ─── Editor: four sequential field prompts ───
//
// Order: name, phone, email, note. Cancel at any step aborts the
// entire edit (returns false). On confirm we save a new entry and
// tombstone the old one (if editing existing).
static bool ct_open_editor(int old_abs_idx) {
    String i_name = "", i_phone = "", i_email = "", i_note = "";
    if (old_abs_idx >= 0) {
        String t, c;
        if (nosql_get_entry("contacts", old_abs_idx, t, c)) {
            i_name = t;
            decode_record(c, i_phone, i_email, i_note);
        }
    }

    char buf[256];

    // Name (required)
    if (!pm_text_input("NAME:", buf, sizeof(buf), i_name.c_str())) return false;
    if (buf[0] == 0) return false;
    String name(buf);

    if (!pm_text_input("PHONE:", buf, sizeof(buf), i_phone.c_str())) return false;
    String phone(buf);

    if (!pm_text_input("EMAIL:", buf, sizeof(buf), i_email.c_str())) return false;
    String email(buf);

    // Note: bigger buffer for free-form text.
    char nbuf[768];
    if (!pm_text_input("NOTE:", nbuf, sizeof(nbuf), i_note.c_str())) return false;
    String note(nbuf);

    String content = encode_record(phone, email, note);
    if (!nosql_save_entry("contacts", name.c_str(), content.c_str())) return false;
    if (old_abs_idx >= 0) ct_add_tombstone(old_abs_idx);
    return true;
}

// View an existing contact. Returns 0 = back, 1 = edited, 2 = deleted.
static int ct_open_view(int abs_idx) {
    String name, content, phone, email, note;
    if (!nosql_get_entry("contacts", abs_idx, name, content)) return 0;
    decode_record(content, phone, email, note);
    ct_draw_view(name, phone, email, note);

    bool was_touched = false;
    int pressed = -2;
    int seg_w = CT_W / 3;
    while (true) {
        int16_t tx, ty;
        bool touched = _ct_touch(&tx, &ty);
        if (touched && !was_touched) {
            if (ty < CT_EXIT_H) {
                pressed = -1;
            } else if (ty >= CT_FOOTER_Y && ty < CT_FOOTER_Y + CT_FOOTER_H) {
                if      (tx < seg_w)       pressed = 0;
                else if (tx < seg_w * 2)   pressed = 1;
                else                       pressed = 2;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1 || pressed == 2) { ct_wait_release(); return 0; }
            if (pressed == 0) {
                // Inline DELETE confirm
                gfx->fillRect(0, CT_FOOTER_Y - 60, CT_W, 56, COL_BG);
                gfx->fillRect(8, CT_FOOTER_Y - 56, CT_W - 16, 48, 0x3000);
                gfx->drawRect(8, CT_FOOTER_Y - 56, CT_W - 16, 48, COL_RED);
                gfx->setTextSize(CT_BTN_TS);
                gfx->setTextColor(COL_RED);
                gfx->setCursor(16, CT_FOOTER_Y - 56 + 16);
                gfx->print("Tap DELETE to confirm");
                ct_wait_release();
                while (true) {
                    int16_t cx, cy;
                    if (_ct_touch(&cx, &cy)) {
                        if (cy >= CT_FOOTER_Y && cy < CT_FOOTER_Y + CT_FOOTER_H &&
                            cx < seg_w) {
                            ct_add_tombstone(abs_idx);
                            ct_wait_release();
                            return 2;
                        }
                        ct_wait_release();
                        ct_draw_view(name, phone, email, note);
                        break;
                    }
                    delay(20); yield();
                }
            } else if (pressed == 1) {
                if (ct_open_editor(abs_idx)) {
                    ct_wait_release();
                    return 1;
                }
                ct_draw_view(name, phone, email, note);
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

void pm_run_contacts() {
    nosql_init("contacts");
    nosql_init("contacts_tombstones");
    ct_scroll_top = 0;
    refresh_cache();
    ct_draw_list();

    bool was_touched = false;
    int pressed = -2;
    int seg_w = CT_W / 3;
    while (true) {
        int16_t tx, ty;
        bool touched = _ct_touch(&tx, &ty);

        if (touched && !was_touched) {
            if (ty < CT_EXIT_H) {
                pressed = -1;
            } else if (ty >= CT_LIST_Y &&
                       ty < CT_LIST_Y + CT_LIST_ROWS * CT_LIST_ROW_H) {
                int row = (ty - CT_LIST_Y) / CT_LIST_ROW_H;
                if (row >= 0 && row < ct_cache_count) pressed = 1000 + row;
            } else if (ty >= CT_FOOTER_Y && ty < CT_FOOTER_Y + CT_FOOTER_H) {
                if      (tx < seg_w)       pressed = -10;
                else if (tx < seg_w * 2)   pressed = -11;
                else                       pressed = -12;
            }
        } else if (!touched && was_touched) {
            if (pressed == -1) { ct_wait_release(); return; }
            else if (pressed == -10) {
                if (ct_scroll_top > 0) {
                    ct_scroll_top -= CT_LIST_ROWS;
                    if (ct_scroll_top < 0) ct_scroll_top = 0;
                    refresh_cache();
                    ct_draw_list();
                }
            } else if (pressed == -11) {
                if (ct_open_editor(-1)) {
                    ct_scroll_top = 0;
                    refresh_cache();
                }
                ct_draw_list();
            } else if (pressed == -12) {
                if (ct_scroll_top + ct_cache_count < ct_visible_total) {
                    ct_scroll_top += CT_LIST_ROWS;
                    refresh_cache();
                    ct_draw_list();
                }
            } else if (pressed >= 1000) {
                int row = pressed - 1000;
                if (row < ct_cache_count) {
                    int abs_idx = ct_cache[row].abs_idx;
                    int act = ct_open_view(abs_idx);
                    if (act != 0) refresh_cache();
                    ct_draw_list();
                }
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20); yield();
    }
}

#endif  // DEVICE_C28P || DEVICE_MAXINE || DEVICE_C5
