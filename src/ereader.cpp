// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// This program is free software: you can redistribute it
// and/or modify it under the terms of the GNU Affero General
// Public License as published by the Free Software Foundation,
// either version 3 of the License, or any later version.
//
// fluidfortune.com

// ─────────────────────────────────────────────
//  PISCES MOON E-READER v1.0
//
//  Architecture
//  ────────────
//  The reader works in two states: PICKER (file list) and READING
//  (paginated text view). Each book on the SD card maps to a
//  single bookmark file holding the byte offset of the user's
//  current page. Re-opening a book seeks straight to that offset.
//
//  Pagination is forward-streaming with a back-history stack:
//    - "Next page" calls layout_page(current_offset), pushes
//      current_offset onto a stack, then advances current_offset
//      to next_page_offset.
//    - "Previous page" pops the stack and seeks back.
//    - When the stack is empty, "previous" is a no-op (we don't
//      know precisely where the previous page started without
//      re-paginating from the file's beginning).
//
//  The wrap engine reads a 4KB chunk from the file at the current
//  offset, then walks it character-by-character producing wrapped
//  lines until the page is full. It never loads the whole file
//  into memory — books of any size work the same way.
//
//  Per-device input is dispatched at the top of the reader loop:
//    - Keyboard devices (T-Deck, T-LoRa Pager, Cardputer ADV):
//      SPC/Right = next, BKSP/Left = prev, B = bookmark, Q = exit
//    - Touch kiosks (C28P, Maxine): tap right half = next, tap
//      left half = prev, tap top strip = exit, tap bookmark
//      button = bookmark.
// ─────────────────────────────────────────────

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif

// ── SD backend (v1.3 — via pm_storage HAL) ──────────────────
//
// All filesystem access goes through pm_storage. Previously this
// file carried its own per-backend adapter (ErFile, ER_OPEN_READ,
// er_open_next, er_get_name); v1.3 collapses that into one HAL
// so the picker, paginator, and bookmark code reads as plain code.
//
// One naming convenience kept: `ErFile` is a local alias for
// pm_storage::File so the body of this file's loops and helpers
// continue to read with the historical type name.
#include "pm_storage.h"
using ErFile = pm_storage::File;

#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "pm_input.h"
#include "theme.h"
#include "ereader.h"

#ifdef DEVICE_C28P
extern bool c28p_touch_read(int16_t* x, int16_t* y);
#endif
#ifdef DEVICE_MAXINE
extern bool maxine_touch_read(int16_t* x, int16_t* y);
#endif

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif

// ═════════════════════════════════════════════
//  PER-DEVICE GEOMETRY
// ═════════════════════════════════════════════
#ifdef DEVICE_TLORAPAGER
static constexpr int SCREEN_W      = 480;
static constexpr int SCREEN_H      = 222;
static constexpr int TEXT_SZ       = 1;
static constexpr int HEADER_H      = 18;
static constexpr int FOOTER_H      = 14;
static constexpr int MARGIN_X      = 10;
static constexpr int LINE_SPACING  = 1;
static constexpr bool HAS_KEYBOARD = true;
static constexpr bool HAS_TOUCH    = false;

#elif defined(DEVICE_CARDPUTER_ADV)
static constexpr int SCREEN_W      = 240;
static constexpr int SCREEN_H      = 135;
static constexpr int TEXT_SZ       = 1;
static constexpr int HEADER_H      = 14;
static constexpr int FOOTER_H      = 12;
static constexpr int MARGIN_X      = 4;
static constexpr int LINE_SPACING  = 1;
static constexpr bool HAS_KEYBOARD = true;
static constexpr bool HAS_TOUCH    = false;

#elif defined(DEVICE_C28P)
// C28P: 240x320 portrait, touch-only. Full-screen reader.
// Bottom strip is a 3-button touch row.
static constexpr int SCREEN_W      = 240;
static constexpr int SCREEN_H      = 320;
static constexpr int TEXT_SZ       = 1;
static constexpr int HEADER_H      = 22;
static constexpr int FOOTER_H      = 30;
static constexpr int MARGIN_X      = 6;
static constexpr int LINE_SPACING  = 2;
static constexpr bool HAS_KEYBOARD = false;
static constexpr bool HAS_TOUCH    = true;

#elif defined(DEVICE_MAXINE)
// Maxine: 480x800 portrait, touch-only. Treated as a
// maxine_apps-class app (full-screen, no dpad chrome).
// Text rendered at size 2 for legibility on the 5" panel.
static constexpr int SCREEN_W      = 480;
static constexpr int SCREEN_H      = 800;
static constexpr int TEXT_SZ       = 2;
static constexpr int HEADER_H      = 56;
static constexpr int FOOTER_H      = 96;
static constexpr int MARGIN_X      = 20;
static constexpr int LINE_SPACING  = 6;
static constexpr bool HAS_KEYBOARD = false;
static constexpr bool HAS_TOUCH    = true;

#else
// T-Deck Plus 320x240 landscape: trackball + keyboard + touch.
static constexpr int SCREEN_W      = 320;
static constexpr int SCREEN_H      = 240;
static constexpr int TEXT_SZ       = 1;
static constexpr int HEADER_H      = 20;
static constexpr int FOOTER_H      = 14;
static constexpr int MARGIN_X      = 8;
static constexpr int LINE_SPACING  = 1;
static constexpr bool HAS_KEYBOARD = true;
static constexpr bool HAS_TOUCH    = true;
#endif

// Derived layout constants
static constexpr int CHAR_W       = 6  * TEXT_SZ;
static constexpr int CHAR_H       = 8  * TEXT_SZ;
static constexpr int LINE_H       = CHAR_H + LINE_SPACING;
static constexpr int CONTENT_X    = MARGIN_X;
static constexpr int CONTENT_Y    = HEADER_H + 4;
static constexpr int CONTENT_W    = SCREEN_W - 2 * MARGIN_X;
static constexpr int CONTENT_H    = SCREEN_H - HEADER_H - FOOTER_H - 8;
static constexpr int MAX_CHARS    = CONTENT_W / CHAR_W;
static constexpr int LINES_MAX    = CONTENT_H / LINE_H;
static constexpr int LINE_BUF_CAP = 96;

// ═════════════════════════════════════════════
//  STATE
// ═════════════════════════════════════════════
static constexpr int MAX_BOOKS = 32;
static char book_paths[MAX_BOOKS][96];
static int  book_count    = 0;
static int  picker_cursor = 0;
static int  picker_top    = 0;

static char     active_path[96];
static uint32_t file_size      = 0;
static uint32_t current_offset = 0;
static uint32_t prev_offsets[64];
static int      prev_top       = 0;

struct WrappedLine {
    uint32_t byte_off;
    uint16_t len;
    char     text[LINE_BUF_CAP];
};
static WrappedLine page_lines[40];
static int         page_line_count = 0;
static uint32_t    next_page_offset = 0;

// ═════════════════════════════════════════════
//  HELPERS
// ═════════════════════════════════════════════
static const char* er_basename(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void er_strip_ext(char* name) {
    char* dot = strrchr(name, '.');
    if (dot && dot != name) *dot = '\0';
}

static bool er_is_supported(const char* name) {
    int len = strlen(name);
    if (len < 4) return false;
    if (strcasecmp(name + len - 4, ".txt") == 0) return true;
    if (len >= 3 && strcasecmp(name + len - 3, ".md") == 0) return true;
    return false;
}

// ═════════════════════════════════════════════
//  BOOKSHELF SCAN
//
//  Walks /books/, /Books/, /BOOKS/ in that order. First folder
//  with at least one .txt or .md file wins. Matches the
//  audio_player.cpp convention so casing is forgiving.
// ═════════════════════════════════════════════
static void er_scan_folder() {
    book_count = 0;
    static const char* folders[] = { "/books", "/Books", "/BOOKS" };

    for (int f = 0; f < 3 && book_count == 0; f++) {
        ErFile dir = pm_storage::openDir(folders[f]);
        if (!dir) continue;
        while (book_count < MAX_BOOKS) {
            ErFile entry = dir.openNextEntry();
            if (!entry) break;
            // pm_storage::File::name() returns the full path on both
            // backends. Extract the leaf for our basename checks.
            const char* full = entry.name();
            const char* slash = strrchr(full, '/');
            const char* name = slash ? slash + 1 : full;
            // Skip dotfiles — our .<name>.bm bookmarks live here.
            if (!entry.isDirectory() && name[0] != '.' && er_is_supported(name)) {
                snprintf(book_paths[book_count], sizeof(book_paths[0]),
                         "%s/%s", folders[f], name);
                book_count++;
            }
        }
    }
}

// ═════════════════════════════════════════════
//  BOOKMARK PERSISTENCE
//
//  /books/Pride.txt    -> /books/.Pride.txt.bm  (text-encoded int)
// ═════════════════════════════════════════════
static void er_bm_path(const char* book_path, char* out, size_t out_sz) {
    const char* slash = strrchr(book_path, '/');
    if (!slash) { snprintf(out, out_sz, ".%s.bm", book_path); return; }
    int dir_len = slash - book_path;
    const char* fname = slash + 1;
    snprintf(out, out_sz, "%.*s/.%s.bm", dir_len, book_path, fname);
}

static uint32_t er_bookmark_load(const char* book_path) {
    char bm_path[112];
    er_bm_path(book_path, bm_path, sizeof(bm_path));
    ErFile f = pm_storage::open(bm_path, pm_storage::Mode::Read);
    if (!f) return 0;
    char buf[16] = {0};
    int n = f.read((uint8_t*)buf, sizeof(buf) - 1);
    f.close();
    if (n <= 0) return 0;
    return (uint32_t)strtoul(buf, nullptr, 10);
}

static bool er_bookmark_save(const char* book_path, uint32_t offset) {
    char bm_path[112];
    er_bm_path(book_path, bm_path, sizeof(bm_path));
    ErFile f = pm_storage::open(bm_path, pm_storage::Mode::Write);
    if (!f) return false;
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%lu", (unsigned long)offset);
    f.write((const uint8_t*)buf, n);
    f.close();
    return true;
}

static bool er_has_bookmark(const char* book_path) {
    char bm_path[112];
    er_bm_path(book_path, bm_path, sizeof(bm_path));
    return pm_storage::exists(bm_path);
}

// ═════════════════════════════════════════════
//  WORD WRAP / PAGINATE
//
//  Reads up to 4KB from the file at start_offset, walks
//  character-by-character producing wrapped lines into
//  page_lines[]. Stops when we have LINES_MAX lines or hit EOF.
//  Sets next_page_offset to where the page ended.
//
//  Wrap rules:
//    - \n forces a line break.
//    - When a line reaches MAX_CHARS, back off to the last space
//      within the line and break there. Resume after the space.
//    - If a single word exceeds MAX_CHARS, hard-break at the
//      limit (rare — typically long URLs).
//    - \r is silently consumed (Windows line endings).
// ═════════════════════════════════════════════
static constexpr int READ_CHUNK = 4096;
static uint8_t read_buf[READ_CHUNK];

static bool layout_page(ErFile& file, uint32_t start_offset) {
    page_line_count = 0;
    next_page_offset = start_offset;

    if (!file) return false;
    if (start_offset >= file_size) return false;

    file.seek(start_offset);
    int chunk_len = file.read(read_buf, sizeof(read_buf));
    if (chunk_len <= 0) return false;

    int i = 0;
    uint32_t base = start_offset;

    while (page_line_count < LINES_MAX &&
           page_line_count < (int)(sizeof(page_lines)/sizeof(page_lines[0]))) {

        WrappedLine& line = page_lines[page_line_count];
        line.byte_off = base + i;
        line.len      = 0;

        int last_space_buf_i  = -1;
        int last_space_line_i = -1;

        while (line.len < MAX_CHARS - 1 && line.len < LINE_BUF_CAP - 1) {
            // Refill chunk if near end and not at EOF.
            if (i >= chunk_len) {
                if (base + chunk_len >= file_size) break;
                base += chunk_len;
                file.seek(base);
                chunk_len = file.read(read_buf, sizeof(read_buf));
                i = 0;
                if (chunk_len <= 0) break;
            }

            uint8_t c = read_buf[i];

            if (c == '\r') { i++; continue; }
            if (c == '\n') { i++; break; }

            if (c == ' ') {
                last_space_buf_i  = i;
                last_space_line_i = line.len;
            }

            line.text[line.len++] = (char)c;
            i++;
        }

        // Word-break: if we filled the line mid-word and have a
        // space to fall back to, break at the space and rewind.
        if (line.len >= MAX_CHARS - 1 && last_space_buf_i >= 0) {
            int consumed_after_space = line.len - last_space_line_i - 1;
            line.len = last_space_line_i;
            i -= consumed_after_space;
            i = last_space_buf_i + 1;  // skip the space itself
        }

        line.text[line.len] = '\0';
        page_line_count++;
    }

    next_page_offset = base + i;
    if (next_page_offset > file_size) next_page_offset = file_size;
    return true;
}

// ═════════════════════════════════════════════
//  PICKER VIEW
// ═════════════════════════════════════════════
static void picker_draw_header() {
    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_DARK);
    gfx->drawFastHLine(0, HEADER_H, SCREEN_W, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(MARGIN_X, (HEADER_H - 8) / 2);
    gfx->print("E-READER");
    const char* hint =
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
        "TAP TOP TO EXIT";
#else
        "Q EXIT";
#endif
    int hint_w = (int)strlen(hint) * 6;
    gfx->setTextColor(C_GREY);
    gfx->setCursor(SCREEN_W - hint_w - MARGIN_X, (HEADER_H - 8) / 2);
    gfx->print(hint);
}

static void picker_draw_footer(const char* msg) {
    gfx->fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, C_DARK);
    gfx->drawFastHLine(0, SCREEN_H - FOOTER_H, SCREEN_W, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREY);
    gfx->setCursor(MARGIN_X, SCREEN_H - FOOTER_H + (FOOTER_H - 8) / 2);
    gfx->print(msg);
}

static void picker_draw_empty() {
    gfx->fillRect(0, HEADER_H + 1, SCREEN_W, SCREEN_H - HEADER_H - FOOTER_H, C_BLACK);
    gfx->setTextSize(1);
    gfx->setTextColor(C_RED);
    gfx->setCursor(MARGIN_X, HEADER_H + 16);
    gfx->print("No books found.");
    gfx->setTextColor(C_GREY);
    gfx->setCursor(MARGIN_X, HEADER_H + 32);
    gfx->print("Drop .txt or .md files into");
    gfx->setCursor(MARGIN_X, HEADER_H + 44);
    gfx->print("/books/ on your microSD card");
    gfx->setCursor(MARGIN_X, HEADER_H + 60);
    gfx->print("then return to this app.");
}

static void picker_draw_list() {
    gfx->fillRect(0, HEADER_H + 1, SCREEN_W,
                  SCREEN_H - HEADER_H - FOOTER_H - 1, C_BLACK);

    int row_h = CHAR_H + 6;
    int max_visible = (SCREEN_H - HEADER_H - FOOTER_H - 8) / row_h;
    if (max_visible < 1) max_visible = 1;
    if (picker_top > picker_cursor) picker_top = picker_cursor;
    if (picker_cursor - picker_top >= max_visible)
        picker_top = picker_cursor - max_visible + 1;

    int end = picker_top + max_visible;
    if (end > book_count) end = book_count;

    int max_name_chars = (CONTENT_W - 24) / CHAR_W;
    if (max_name_chars > LINE_BUF_CAP - 1) max_name_chars = LINE_BUF_CAP - 1;

    for (int i = picker_top; i < end; i++) {
        int row = i - picker_top;
        int y = HEADER_H + 4 + row * row_h;

        bool is_cursor = (i == picker_cursor);
        if (is_cursor) {
            gfx->fillRect(2, y - 2, SCREEN_W - 4, row_h, C_DARK);
        }

        gfx->setTextSize(TEXT_SZ);
        if (is_cursor) {
            gfx->setTextColor(C_GREEN);
            gfx->setCursor(MARGIN_X, y);
            gfx->print(">");
        } else if (er_has_bookmark(book_paths[i])) {
            gfx->setTextColor(C_CYAN);
            gfx->setCursor(MARGIN_X, y);
            gfx->print("*");
        }

        char display[LINE_BUF_CAP];
        strncpy(display, er_basename(book_paths[i]), sizeof(display) - 1);
        display[sizeof(display) - 1] = '\0';
        er_strip_ext(display);
        if ((int)strlen(display) > max_name_chars) {
            display[max_name_chars - 1] = '.';
            display[max_name_chars]     = '.';
            display[max_name_chars + 1] = '\0';
        }
        gfx->setTextColor(is_cursor ? C_WHITE : C_GREY);
        gfx->setCursor(MARGIN_X + 14, y);
        gfx->print(display);
    }

    char footer[64];
    snprintf(footer, sizeof(footer),
             "%d/%d  %s", picker_cursor + 1, book_count,
#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
             "TAP A BOOK TO READ"
#elif defined(DEVICE_TLORAPAGER)
             "UP/DN: SELECT  ENTER: OPEN"
#else
             "TRACKBALL UP/DN  CLICK: OPEN"
#endif
             );
    picker_draw_footer(footer);
}

static int picker_run() {
    picker_draw_header();
    if (book_count == 0) {
        picker_draw_empty();
        picker_draw_footer("PRESS ANY KEY / TAP TO EXIT");
        while (true) {
            char k = get_keypress();
            if (k) return -1;
            int16_t tx, ty;
#ifdef DEVICE_C28P
            if (c28p_touch_read(&tx, &ty)) {
                while (c28p_touch_read(&tx, &ty)) { delay(10); yield(); }
                return -1;
            }
#elif defined(DEVICE_MAXINE)
            if (maxine_touch_read(&tx, &ty)) {
                while (maxine_touch_read(&tx, &ty)) { delay(10); yield(); }
                return -1;
            }
#else
            if (get_touch(&tx, &ty)) {
                while (get_touch(&tx, &ty)) delay(10);
                return -1;
            }
#endif
            delay(30);
            yield();
        }
    }

    picker_draw_list();

    int row_h = CHAR_H + 6;

    while (true) {
        char k = get_keypress();
        TrackballState tb;
        tb.x = 0; tb.y = 0; tb.clicked = false;
#if !defined(DEVICE_C28P) && !defined(DEVICE_MAXINE)
        tb = update_trackball();
#endif

        if (k == 'q' || k == 'Q' || k == PM_KEY_ESC || pm_is_exit_key(k)) {
            return -1;
        }
        if (k == PM_KEY_UP || tb.y == -1) {
            if (picker_cursor > 0) { picker_cursor--; picker_draw_list(); }
        }
        if (k == PM_KEY_DOWN || tb.y == 1) {
            if (picker_cursor < book_count - 1) { picker_cursor++; picker_draw_list(); }
        }
        if (k == PM_KEY_ENTER || k == '\n' || k == '\r' || k == ' ' || tb.clicked) {
            return picker_cursor;
        }

        int16_t tx, ty;
        bool touched = false;
#ifdef DEVICE_C28P
        touched = c28p_touch_read(&tx, &ty);
#elif defined(DEVICE_MAXINE)
        touched = maxine_touch_read(&tx, &ty);
#elif !defined(DEVICE_TLORAPAGER) && !defined(DEVICE_CARDPUTER_ADV)
        touched = get_touch(&tx, &ty);
#endif
        if (touched) {
            if (ty < HEADER_H) {
#ifdef DEVICE_C28P
                while (c28p_touch_read(&tx, &ty)) { delay(10); yield(); }
#elif defined(DEVICE_MAXINE)
                while (maxine_touch_read(&tx, &ty)) { delay(10); yield(); }
#else
                while (get_touch(&tx, &ty)) delay(10);
#endif
                return -1;
            }
            int rel_y = ty - (HEADER_H + 4);
            if (rel_y >= 0) {
                int row = rel_y / row_h;
                int idx = picker_top + row;
                if (idx >= 0 && idx < book_count) {
#ifdef DEVICE_C28P
                    while (c28p_touch_read(&tx, &ty)) { delay(10); yield(); }
#elif defined(DEVICE_MAXINE)
                    while (maxine_touch_read(&tx, &ty)) { delay(10); yield(); }
#else
                    while (get_touch(&tx, &ty)) delay(10);
#endif
                    picker_cursor = idx;
                    return idx;
                }
            }
        }

        delay(20);
        yield();
    }
}

// ═════════════════════════════════════════════
//  READER VIEW
// ═════════════════════════════════════════════
static void reader_draw_header(const char* book_path) {
    char title[64];
    strncpy(title, er_basename(book_path), sizeof(title) - 1);
    title[sizeof(title) - 1] = '\0';
    er_strip_ext(title);
    int max_title_chars = (SCREEN_W - 2 * MARGIN_X - 60) / 6;
    if ((int)strlen(title) > max_title_chars && max_title_chars > 3) {
        title[max_title_chars - 1] = '.';
        title[max_title_chars]     = '.';
        title[max_title_chars + 1] = '\0';
    }

    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_DARK);
    gfx->drawFastHLine(0, HEADER_H, SCREEN_W, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(MARGIN_X, (HEADER_H - 8) / 2);
    gfx->print(title);
}

static void reader_draw_footer(uint32_t current, uint32_t total) {
    int fy = SCREEN_H - FOOTER_H;
    gfx->fillRect(0, fy, SCREEN_W, FOOTER_H, C_DARK);
    gfx->drawFastHLine(0, fy, SCREEN_W, C_GREEN);

#if defined(DEVICE_C28P) || defined(DEVICE_MAXINE)
    int seg_w = SCREEN_W / 3;
    gfx->drawFastVLine(seg_w,     fy, FOOTER_H, 0x4208);
    gfx->drawFastVLine(seg_w * 2, fy, FOOTER_H, 0x4208);

    gfx->setTextColor(C_WHITE);
    gfx->setTextSize(TEXT_SZ);
    int label_y = fy + (FOOTER_H - CHAR_H) / 2;

    int prev_x = (seg_w   - 6 * CHAR_W) / 2;
    int bm_x   = seg_w   + (seg_w - 8 * CHAR_W) / 2;
    int next_x = 2*seg_w + (seg_w - 6 * CHAR_W) / 2;
    gfx->setCursor(prev_x, label_y); gfx->print("< PREV");
    gfx->setCursor(bm_x,   label_y); gfx->print("BOOKMARK");
    gfx->setCursor(next_x, label_y); gfx->print("NEXT >");
#else
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREY);
    gfx->setCursor(MARGIN_X, fy + (FOOTER_H - 8) / 2);
#ifdef DEVICE_TLORAPAGER
    gfx->print("SPC:NEXT  BKSP:PREV  B:BOOKMARK  Q:EXIT");
#else
    gfx->print("SPC/RIGHT NEXT  BKSP/LEFT PREV  B BM  Q EXIT");
#endif
#endif

    if (total > 0) {
        char pct[16];
        int percent = (int)((current * 100ULL) / total);
        if (percent > 100) percent = 100;
        snprintf(pct, sizeof(pct), "%d%%", percent);
        gfx->setTextSize(1);
        gfx->setTextColor(C_CYAN);
        int pw = (int)strlen(pct) * 6;
        gfx->setCursor(SCREEN_W - pw - MARGIN_X, fy - 10);
        gfx->print(pct);
    }
}

static void reader_paint_page() {
    int cy = CONTENT_Y;
    int ch = SCREEN_H - HEADER_H - FOOTER_H - 8;
    gfx->fillRect(0, cy, SCREEN_W, ch, C_BLACK);

    gfx->setTextSize(TEXT_SZ);
    gfx->setTextColor(C_WHITE);
    for (int i = 0; i < page_line_count; i++) {
        int y = cy + i * LINE_H;
        if (y + CHAR_H > SCREEN_H - FOOTER_H) break;
        gfx->setCursor(CONTENT_X, y);
        gfx->print(page_lines[i].text);
    }
}

static void reader_flash_msg(const char* msg, uint16_t color) {
    int box_w = (int)strlen(msg) * CHAR_W + 20;
    int box_h = CHAR_H + 16;
    int box_x = (SCREEN_W - box_w) / 2;
    int box_y = CONTENT_Y + (SCREEN_H - HEADER_H - FOOTER_H) / 2 - box_h / 2;
    gfx->fillRect(box_x, box_y, box_w, box_h, C_DARK);
    gfx->drawRect(box_x, box_y, box_w, box_h, color);
    gfx->setTextSize(TEXT_SZ);
    gfx->setTextColor(color);
    gfx->setCursor(box_x + 10, box_y + 8);
    gfx->print(msg);
    delay(800);
    reader_paint_page();
}

static void reader_run(const char* book_path) {
    strncpy(active_path, book_path, sizeof(active_path) - 1);
    active_path[sizeof(active_path) - 1] = '\0';

    ErFile file = pm_storage::open(active_path, pm_storage::Mode::Read);
    if (!file) {
        gfx->fillScreen(C_BLACK);
        gfx->setTextSize(1);
        gfx->setTextColor(C_RED);
        gfx->setCursor(MARGIN_X, SCREEN_H / 2 - CHAR_H);
        gfx->print("Failed to open book.");
        delay(1500);
        return;
    }
    file_size = (uint32_t)file.size();
    current_offset = er_bookmark_load(active_path);
    if (current_offset >= file_size) current_offset = 0;
    prev_top = 0;

    gfx->fillScreen(C_BLACK);
    reader_draw_header(active_path);

    layout_page(file, current_offset);
    reader_draw_footer(current_offset, file_size);
    reader_paint_page();

    bool quit = false;
    while (!quit) {
        char k = get_keypress();
        bool want_next = false, want_prev = false, want_bm = false;

        if (HAS_KEYBOARD) {
            if (k == 'q' || k == 'Q' || k == PM_KEY_ESC || pm_is_exit_key(k)) {
                quit = true;
            }
            if (k == ' ' || k == PM_KEY_RIGHT || k == PM_KEY_PGDN ||
                k == PM_KEY_ENTER || k == '\n' || k == '\r') {
                want_next = true;
            }
            if (k == PM_KEY_BACKSPACE || k == PM_KEY_LEFT || k == PM_KEY_PGUP) {
                want_prev = true;
            }
            if (k == 'b' || k == 'B') {
                want_bm = true;
            }
        }

#if !defined(DEVICE_C28P) && !defined(DEVICE_MAXINE) && !defined(DEVICE_TLORAPAGER) && !defined(DEVICE_CARDPUTER_ADV)
        // T-Deck Plus: trackball as a secondary nav source.
        TrackballState tb = update_trackball();
        if (tb.x == 1)  want_next = true;
        if (tb.x == -1) want_prev = true;
        if (tb.clicked) want_next = true;
#endif

        if (HAS_TOUCH) {
            int16_t tx, ty;
            bool touched = false;
#ifdef DEVICE_C28P
            touched = c28p_touch_read(&tx, &ty);
#elif defined(DEVICE_MAXINE)
            touched = maxine_touch_read(&tx, &ty);
#else
            touched = get_touch(&tx, &ty);
#endif
            if (touched) {
                int  press_x = tx;
                bool still_in_top = (ty < HEADER_H);
                bool in_footer    = (ty >= SCREEN_H - FOOTER_H);

                int16_t rx, ry;
#ifdef DEVICE_C28P
                while (c28p_touch_read(&rx, &ry)) { delay(10); yield(); }
#elif defined(DEVICE_MAXINE)
                while (maxine_touch_read(&rx, &ry)) { delay(10); yield(); }
#else
                while (get_touch(&rx, &ry)) delay(10);
#endif

                if (still_in_top) {
                    quit = true;
                } else if (in_footer) {
                    int seg_w = SCREEN_W / 3;
                    if      (press_x <  seg_w)        want_prev = true;
                    else if (press_x <  seg_w * 2)    want_bm   = true;
                    else                              want_next = true;
                } else {
                    if (press_x < SCREEN_W / 2)       want_prev = true;
                    else                              want_next = true;
                }
            }
        }

        if (quit) {
            // Save offset so user resumes here next time, even
            // without explicit bookmarking.
            er_bookmark_save(active_path, current_offset);
            break;
        }
        if (want_next) {
            if (next_page_offset < file_size) {
                if (prev_top < (int)(sizeof(prev_offsets)/sizeof(prev_offsets[0]))) {
                    prev_offsets[prev_top++] = current_offset;
                }
                current_offset = next_page_offset;
                layout_page(file, current_offset);
                reader_draw_footer(current_offset, file_size);
                reader_paint_page();
            } else {
                reader_flash_msg("END OF BOOK", C_CYAN);
            }
        } else if (want_prev) {
            if (prev_top > 0) {
                current_offset = prev_offsets[--prev_top];
                layout_page(file, current_offset);
                reader_draw_footer(current_offset, file_size);
                reader_paint_page();
            }
        } else if (want_bm) {
            if (er_bookmark_save(active_path, current_offset)) {
                reader_flash_msg("BOOKMARKED", C_GREEN);
            } else {
                reader_flash_msg("BOOKMARK FAILED", C_RED);
            }
        }

        delay(20);
        yield();
    }

    file.close();
}

// ═════════════════════════════════════════════
//  ENTRY POINT
// ═════════════════════════════════════════════
void run_ereader() {
    picker_cursor = 0;
    picker_top    = 0;

    er_scan_folder();

    while (true) {
        gfx->fillScreen(C_BLACK);
        int chosen = picker_run();
        if (chosen < 0) break;
        reader_run(book_paths[chosen]);
    }

    gfx->fillScreen(C_BLACK);
}
