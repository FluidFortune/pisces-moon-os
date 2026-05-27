// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_data_reader.cpp — touch-driven NoSQL data reader for C28P
//
//  Replaces data_reader.cpp's keyboard-driven UI with a tap/swipe
//  interface for the C28P kiosk. Same backing NoSQL store, just
//  a different presentation layer.
//
//  Used for the SURVIVAL, MEDICAL, and HISTORY reference categories
//  in the TOOLS sub-launcher.
//
//  Layout: 240×320 portrait
//    Top (32px):   Title bar with category name
//    Middle:       Browse list (10 visible rows) OR detail view
//    Bottom (40px): Scroll/back/search controls
//
//  Tapping a row opens the entry detail view (full content with
//  paginated scrolling). Tap BACK to return to list. Tap BACK
//  again to return to launcher.
//
//  This is the OVERRIDE for C28P only — other devices continue
//  to use data_reader.cpp's keyboard-driven UI. The data layer
//  (nosql_store.cpp) is shared and unchanged.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "nosql_store.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t* x, int16_t* y);

#define ROW_H       24
#define LIST_TOP    44
#define LIST_ROWS   9
#define CTRL_TOP    280

static String load_entry_content(const char* category, int idx) {
    String title;
    String content;
    if (nosql_get_entry(category, idx, title, content)) {
        return content;
    }
    return String("(entry not found)");
}

static void draw_list_chrome(const char* display_name, int total) {
    gfx->fillScreen(0x0000);
    // Title bar
    gfx->fillRect(0, 0, 240, 36, 0x18C3);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(12, 10);
    gfx->print(display_name);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(180, 16);
    gfx->printf("%d entries", total);

    // Controls
    gfx->fillRect(4, CTRL_TOP, 70, 36, 0x18C3);
    gfx->drawRect(4, CTRL_TOP, 70, 36, 0xFFE0);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(24, CTRL_TOP + 14);
    gfx->print("UP");

    gfx->fillRect(86, CTRL_TOP, 70, 36, 0x18C3);
    gfx->drawRect(86, CTRL_TOP, 70, 36, 0xFFE0);
    gfx->setCursor(102, CTRL_TOP + 14);
    gfx->print("DOWN");

    gfx->fillRect(168, CTRL_TOP, 68, 36, 0x3000);
    gfx->drawRect(168, CTRL_TOP, 68, 36, 0xF800);
    gfx->setTextColor(0xF800);
    gfx->setCursor(186, CTRL_TOP + 14);
    gfx->print("BACK");
}

static void draw_list_page(const char* category, int scroll_top, int total) {
    // Clear the list area
    gfx->fillRect(0, LIST_TOP, 240, LIST_ROWS * ROW_H, 0x0000);

    String title;
    String content;
    for (int i = 0; i < LIST_ROWS; i++) {
        int idx = scroll_top + i;
        if (idx >= total) break;
        if (!nosql_get_entry(category, idx, title, content)) continue;
        int y = LIST_TOP + i * ROW_H;

        // Alternating row background for readability
        gfx->fillRect(0, y, 240, ROW_H - 2, (i % 2) ? 0x1082 : 0x0841);

        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(8, y + 8);
        // Truncate to 36 chars
        char buf[37];
        strncpy(buf, title.c_str(), 36);
        buf[36] = 0;
        gfx->print(buf);
    }
}

static void draw_detail(const char* title, const String& content,
                        int line_offset, int total_lines) {
    gfx->fillScreen(0x0000);

    // Title bar
    gfx->fillRect(0, 0, 240, 36, 0x18C3);
    gfx->setTextSize(1);
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, 6);
    char tbuf[37];
    strncpy(tbuf, title, 36);
    tbuf[36] = 0;
    gfx->print(tbuf);

    // Content area
    gfx->fillRect(0, 40, 240, 232, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);

    // Word-wrap content at 38 chars per line, render lines
    // [line_offset .. line_offset + 25] (one screen of text)
    int char_w = 6;
    int line_h = 10;
    int max_cols = 38;
    int max_lines_visible = 23;

    int line = 0;
    int rendered = 0;
    int start = 0;
    for (int i = 0; i <= content.length() && rendered < max_lines_visible; i++) {
        if (i - start >= max_cols || (i < content.length() && content[i] == '\n') || i == content.length()) {
            if (line >= line_offset) {
                String chunk = content.substring(start, i);
                gfx->setCursor(4, 44 + rendered * line_h);
                gfx->print(chunk);
                rendered++;
            }
            line++;
            start = (i < content.length() && content[i] == '\n') ? i + 1 : i;
        }
    }

    // Set the title bar subtitle to show position
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(180, 22);
    gfx->printf("L %d", line_offset);

    // Controls
    gfx->fillRect(4, CTRL_TOP, 70, 36, 0x18C3);
    gfx->drawRect(4, CTRL_TOP, 70, 36, 0xFFE0);
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(24, CTRL_TOP + 14);
    gfx->print("UP");

    gfx->fillRect(86, CTRL_TOP, 70, 36, 0x18C3);
    gfx->drawRect(86, CTRL_TOP, 70, 36, 0xFFE0);
    gfx->setCursor(102, CTRL_TOP + 14);
    gfx->print("DOWN");

    gfx->fillRect(168, CTRL_TOP, 68, 36, 0x3000);
    gfx->drawRect(168, CTRL_TOP, 68, 36, 0xF800);
    gfx->setTextColor(0xF800);
    gfx->setCursor(186, CTRL_TOP + 14);
    gfx->print("BACK");
}

// ─────────────────────────────────────────────
//  DETAIL VIEW
// ─────────────────────────────────────────────
static void detail_view(const char* category, int entry_idx) {
    String title;
    String content;
    if (!nosql_get_entry(category, entry_idx, title, content)) {
        Serial.printf("[C28P-DR] entry %d not found\n", entry_idx);
        return;
    }
    int line_offset = 0;
    int total_lines = (content.length() / 38) + 1;
    draw_detail(title.c_str(), content, line_offset, total_lines);

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            if (ty >= CTRL_TOP && ty < CTRL_TOP + 36) {
                if (tx < 74) {
                    // UP
                    if (line_offset > 0) {
                        line_offset = max(0, line_offset - 20);
                        draw_detail(title.c_str(), content, line_offset, total_lines);
                    }
                } else if (tx < 156) {
                    // DOWN
                    if (line_offset < total_lines - 20) {
                        line_offset += 20;
                        draw_detail(title.c_str(), content, line_offset, total_lines);
                    }
                } else {
                    // BACK
                    while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                    return;
                }
            }
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  PUBLIC ENTRY — override for run_data_reader on C28P
//  This is what run_data_reader() resolves to when DEVICE_C28P
//  is defined. The signature matches data_reader.h's declaration.
// ─────────────────────────────────────────────
void run_data_reader(const char* category, const char* display_name) {
    Serial.printf("[C28P-DR] %s starting\n", display_name);

    nosql_init(category);
    int total = nosql_get_count(category);

    if (total == 0) {
        gfx->fillScreen(0x0000);
        gfx->setTextSize(2);
        gfx->setTextColor(0xF800);
        gfx->setCursor(20, 100);
        gfx->print("EMPTY");
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(20, 140);
        gfx->printf("No %s entries.", display_name);
        gfx->setCursor(20, 156);
        gfx->print("Add JSON files to");
        gfx->setCursor(20, 172);
        gfx->printf("/data/%s/ on SD.", category);
        gfx->setTextColor(0x8410);
        gfx->setCursor(40, 280);
        gfx->print("Tap to return");

        bool was_touched = false;
        while (true) {
            int16_t tx, ty;
            bool touched = c28p_touch_read(&tx, &ty);
            if (touched && !was_touched) {
                while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                return;
            }
            was_touched = touched;
            delay(20);
            yield();
        }
    }

    int scroll_top = 0;
    draw_list_chrome(display_name, total);
    draw_list_page(category, scroll_top, total);

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            // Tap on a row → open detail
            if (ty >= LIST_TOP && ty < LIST_TOP + LIST_ROWS * ROW_H) {
                int row = (ty - LIST_TOP) / ROW_H;
                int idx = scroll_top + row;
                if (idx < total) {
                    detail_view(category, idx);
                    // Repaint after returning from detail
                    draw_list_chrome(display_name, total);
                    draw_list_page(category, scroll_top, total);
                }
            }
            // Controls
            else if (ty >= CTRL_TOP && ty < CTRL_TOP + 36) {
                if (tx < 74) {
                    // UP
                    if (scroll_top > 0) {
                        scroll_top = max(0, scroll_top - LIST_ROWS);
                        draw_list_page(category, scroll_top, total);
                    }
                } else if (tx < 156) {
                    // DOWN
                    if (scroll_top + LIST_ROWS < total) {
                        scroll_top += LIST_ROWS;
                        draw_list_page(category, scroll_top, total);
                    }
                } else {
                    // BACK
                    while (c28p_touch_read(&tx, &ty)) { delay(20); yield(); }
                    return;
                }
            }
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

#endif // DEVICE_C28P