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

/**
 * PISCES MOON OS — data_reader.cpp
 * Generic offline NoSQL reference reader.
 *
 * Trackball (list view):
 *   Up/Down  — move cursor through list rows
 *   Left     — previous page
 *   Right    — next page
 *   Click    — open highlighted entry
 *
 * Trackball (entry detail view):
 *   Up/Down  — scroll one line
 *   Left     — scroll one page up  (or back to list if at top)
 *   Right    — scroll one page down
 *   Click    — back to list
 *   SPACE    — next page
 *   BKSP     — previous page
 *   ESC/Q    — back to list
 */

#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "keyboard.h"
#include "nosql_store.h"
#include "apps.h"

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  HARDWARE CONSTANTS
// ─────────────────────────────────────────────
#define SCREEN_W        320
#define SCREEN_H        218
#define HEADER_H        26
#define FOOTER_H        14
#define TEXT_START_X    5
#define TEXT_START_Y    32
#define CHAR_W          6
#define CHAR_H          10
#define ROW_H           18
#define MAX_LIST_ROWS   ((SCREEN_H - TEXT_START_Y) / ROW_H)  // ~10 rows

// Detail viewer geometry
#define DV_CONTENT_Y    (HEADER_H + 2)
#define DV_CONTENT_H    (240 - HEADER_H - FOOTER_H)
#define DV_LINE_H       9     // px per line at textSize(1)
#define DV_CHARS        52    // chars per line (320-10) / 6
#define DV_LINES_PER_PAGE   (DV_CONTENT_H / DV_LINE_H)  // ~20

// ─────────────────────────────────────────────
//  DRAW HELPERS
// ─────────────────────────────────────────────
void dr_draw_header(const char* left, const char* right = nullptr) {
    gfx->fillRect(0, 0, SCREEN_W, HEADER_H, C_DARK);
    gfx->drawFastHLine(0, HEADER_H - 2, SCREEN_W, C_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREEN);
    gfx->setCursor(10, 7);
    gfx->print(left);
    if (right) {
        int rx = SCREEN_W - (strlen(right) * CHAR_W) - 5;
        gfx->setTextColor(C_GREY);
        gfx->setCursor(rx, 7);
        gfx->print(right);
    }
}

// Legacy wrapper kept for callers outside this file
int dr_draw_wrapped_text(const String &text, int x, int y, uint16_t color) {
    gfx->setTextColor(color);
    gfx->setTextSize(1);
    int cx = x;
    int cy = y;

    for (int i = 0; i < (int)text.length(); i++) {
        char ch = text[i];
        if (ch == '\n') {
            cx = x;
            cy += CHAR_H;
        } else {
            if (cx + CHAR_W > SCREEN_W - 5) {
                cx = x;
                cy += CHAR_H;
            }
            if (cy + CHAR_H > SCREEN_H) break;
            gfx->setCursor(cx, cy);
            gfx->print(ch);
            cx += CHAR_W;
        }
    }
    return cy;
}

// ─────────────────────────────────────────────
//  WORD-WRAP ENGINE (detail viewer)
//  Returns line count; caller must delete[] lines.
// ─────────────────────────────────────────────
static int dv_wrap(const String& text, String*& lines) {
    int maxLines = max(8, (int)(text.length() / 8) + 4);
    lines = new String[maxLines];
    int lineCount = 0;
    int start = 0;
    int len   = text.length();

    while (start < len && lineCount < maxLines) {
        // Skip leading space (except first line)
        if (lineCount > 0) {
            while (start < len && text[start] == ' ') start++;
        }
        if (start >= len) break;

        // Natural newline within limit?
        int nlPos = text.indexOf('\n', start);
        int end;
        bool consumeNl = false;

        if (nlPos >= 0 && nlPos - start <= DV_CHARS) {
            end = nlPos;
            consumeNl = true;
        } else if (len - start <= DV_CHARS) {
            end = len;
        } else {
            // Find last space within limit
            end = start + DV_CHARS;
            int sp = -1;
            for (int i = end; i > start; i--) {
                if (text[i] == ' ') { sp = i; break; }
            }
            if (sp > start) end = sp;
        }

        lines[lineCount++] = text.substring(start, end);
        start = end + (consumeNl ? 1 : 0);
    }

    return lineCount;
}

// ─────────────────────────────────────────────
//  ROW HIGHLIGHT HELPER
// ─────────────────────────────────────────────
static void dr_draw_row(int i, int entryIdx, const String &title,
                        bool highlighted) {
    int rowY = TEXT_START_Y + (i * ROW_H);

    if (highlighted) {
        gfx->fillRect(0, rowY, SCREEN_W, ROW_H - 2, C_DARK);
    } else {
        gfx->fillRect(0, rowY, SCREEN_W, ROW_H - 2, 0x0000);
    }

    gfx->setCursor(TEXT_START_X, rowY + 4);
    gfx->setTextColor(highlighted ? C_GREEN : C_GREY);
    gfx->setTextSize(1);
    gfx->printf("%3d ", entryIdx + 1);

    gfx->setTextColor(highlighted ? C_WHITE : 0xC618);
    String displayTitle = title.substring(0, 38);
    gfx->print(displayTitle);
}

// ─────────────────────────────────────────────
//  DETAIL VIEWER FOOTER
// ─────────────────────────────────────────────
static void dv_draw_footer(int firstLine, int totalLines) {
    int fy = 240 - FOOTER_H;
    gfx->fillRect(0, fy, 320, FOOTER_H, C_DARK);
    gfx->drawFastHLine(0, fy, 320, 0x0421);
    gfx->setTextSize(1);

    if (totalLines <= DV_LINES_PER_PAGE) {
        gfx->setTextColor(0x4208);
        gfx->setCursor(40, fy + 3);
        gfx->print("CLK/BKSP/Q: back to list");
    } else {
        int lastLine = min(firstLine + DV_LINES_PER_PAGE, totalLines);
        int pct = (lastLine * 100) / totalLines;

        // Mini progress bar
        int bx = 90, bw = 130, by = fy + 5;
        gfx->drawRect(bx, by, bw, 4, 0x2104);
        gfx->fillRect(bx + 1, by + 1, (bw - 2) * pct / 100, 2, C_GREEN);

        gfx->setTextColor(0x4208);
        gfx->setCursor(4, fy + 3);
        gfx->printf("%d/%d", lastLine, totalLines);

        if (lastLine < totalLines) {
            gfx->setTextColor(0xFD20);
            gfx->setCursor(235, fy + 3);
            gfx->print("SPC:more");
        } else {
            gfx->setTextColor(C_GREEN);
            gfx->setCursor(228, fy + 3);
            gfx->print("END  Q:back");
        }
    }
}

// ─────────────────────────────────────────────
//  DETAIL VIEWER PAGE RENDERER
// ─────────────────────────────────────────────
static void dv_render_page(String* lines, int lineCount, int firstLine) {
    gfx->fillRect(0, DV_CONTENT_Y, 320, DV_CONTENT_H, 0x0000);
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);

    int y = DV_CONTENT_Y + 2;
    for (int i = firstLine; i < lineCount && i < firstLine + DV_LINES_PER_PAGE; i++) {
        gfx->setCursor(TEXT_START_X, y);
        gfx->print(lines[i]);
        y += DV_LINE_H;
    }

    dv_draw_footer(firstLine, lineCount);
}

// ─────────────────────────────────────────────
//  ENTRY VIEWER (replaces old dr_view_entry)
//  Full scroll support — trackball and keyboard.
// ─────────────────────────────────────────────
void dr_view_entry(const char* category, int entryIndex) {
    String title, content;
    if (!nosql_get_entry(category, entryIndex, title, content)) {
        gfx->fillScreen(0x0000);
        dr_draw_header("ERROR");
        gfx->setCursor(10, 40);
        gfx->setTextColor(0xF800);
        gfx->print("Failed to load entry.");
        delay(2000);
        return;
    }

    // Build header string — truncate long titles
    String hdrTitle = title;
    if (hdrTitle.length() > 30) hdrTitle = hdrTitle.substring(0, 27) + "...";

    // Prepend title as first lines of content
    String fullText = title + "\n" + String('-', min((int)title.length(), DV_CHARS)) + "\n" + content;

    String* lines = nullptr;
    int lineCount = dv_wrap(fullText, lines);
    int firstLine = 0;

    gfx->fillScreen(0x0000);
    dr_draw_header(hdrTitle.c_str(), "Q/CLK:back");
    dv_render_page(lines, lineCount, firstLine);

    while (true) {
        // Header tap = back to list
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            if (ty < HEADER_H) break;
        }

        // Trackball
        TrackballState tb = update_trackball();
        if (tb.clicked) break;

        if (tb.y == -1 && firstLine > 0) {
            firstLine--;
            dv_render_page(lines, lineCount, firstLine);
        } else if (tb.y == 1 && firstLine + DV_LINES_PER_PAGE < lineCount) {
            firstLine++;
            dv_render_page(lines, lineCount, firstLine);
        } else if (tb.x == 1) {
            int next = firstLine + DV_LINES_PER_PAGE;
            if (next < lineCount) { firstLine = next; dv_render_page(lines, lineCount, firstLine); }
        } else if (tb.x == -1) {
            if (firstLine == 0) break;  // At top — go back to list
            firstLine = max(0, firstLine - DV_LINES_PER_PAGE);
            dv_render_page(lines, lineCount, firstLine);
        }

        char c = get_keypress();
        if (c == 27 || c == 'q' || c == 'Q') break;
        if (c == ' ') {
            int next = firstLine + DV_LINES_PER_PAGE;
            if (next < lineCount) { firstLine = next; dv_render_page(lines, lineCount, firstLine); }
            else break;
        } else if (c == 8 || c == 127) {
            if (firstLine == 0) break;
            firstLine = max(0, firstLine - DV_LINES_PER_PAGE);
            dv_render_page(lines, lineCount, firstLine);
        }

        delay(20);
        yield();
    }

    delete[] lines;
}

// ─────────────────────────────────────────────
//  SEARCH SCREEN
// ─────────────────────────────────────────────
void dr_search_screen(const char* category, const char* display_name) {
    gfx->fillScreen(0x0000);
    dr_draw_header("SEARCH | HDR: BACK");

    gfx->setCursor(TEXT_START_X, TEXT_START_Y);
    gfx->setTextColor(C_WHITE);
    gfx->setTextSize(1);
    gfx->print("Keyword: ");

    String keyword = get_text_input(TEXT_START_X + 54, TEXT_START_Y);

    if (keyword == "##EXIT##" || keyword.length() == 0) return;

    gfx->fillRect(0, TEXT_START_Y + CHAR_H + 4, SCREEN_W, 20, 0x0000);
    gfx->setCursor(TEXT_START_X, TEXT_START_Y + CHAR_H + 8);
    gfx->setTextColor(C_GREY);
    gfx->print("Searching...");

    String result_title, result_content;
    bool found = nosql_search(category, keyword.c_str(),
                              result_title, result_content);

    if (!found) {
        gfx->fillRect(0, TEXT_START_Y + CHAR_H + 4, SCREEN_W, SCREEN_H, 0x0000);
        gfx->setCursor(TEXT_START_X, TEXT_START_Y + CHAR_H + 8);
        gfx->setTextColor(0xF800);
        gfx->print("No results for: ");
        gfx->print(keyword);
        delay(2000);
        return;
    }

    // Reuse the entry viewer with the search result
    // Pack title + content the same way dr_view_entry does
    String fullText = result_title + "\n" + String('-', min((int)result_title.length(), DV_CHARS)) + "\n" + result_content;
    String* lines = nullptr;
    int lineCount = dv_wrap(fullText, lines);
    int firstLine = 0;

    gfx->fillScreen(0x0000);
    String hdr = "SEARCH: " + keyword.substring(0, 20);
    dr_draw_header(hdr.c_str(), "Q/CLK:back");
    dv_render_page(lines, lineCount, firstLine);

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            if (ty < HEADER_H) break;
        }

        TrackballState tb = update_trackball();
        if (tb.clicked) break;

        if (tb.y == -1 && firstLine > 0) {
            firstLine--;
            dv_render_page(lines, lineCount, firstLine);
        } else if (tb.y == 1 && firstLine + DV_LINES_PER_PAGE < lineCount) {
            firstLine++;
            dv_render_page(lines, lineCount, firstLine);
        } else if (tb.x == 1) {
            int next = firstLine + DV_LINES_PER_PAGE;
            if (next < lineCount) { firstLine = next; dv_render_page(lines, lineCount, firstLine); }
        } else if (tb.x == -1) {
            if (firstLine == 0) break;
            firstLine = max(0, firstLine - DV_LINES_PER_PAGE);
            dv_render_page(lines, lineCount, firstLine);
        }

        char c = get_keypress();
        if (c == 27 || c == 'q' || c == 'Q') break;
        if (c == ' ') {
            int next = firstLine + DV_LINES_PER_PAGE;
            if (next < lineCount) { firstLine = next; dv_render_page(lines, lineCount, firstLine); }
            else break;
        } else if (c == 8 || c == 127) {
            if (firstLine == 0) break;
            firstLine = max(0, firstLine - DV_LINES_PER_PAGE);
            dv_render_page(lines, lineCount, firstLine);
        }

        delay(20);
        yield();
    }

    delete[] lines;
}

// ─────────────────────────────────────────────
//  ENTRY BROWSER  (trackball-enabled)
// ─────────────────────────────────────────────
void dr_browse(const char* category, const char* display_name) {
    int total = nosql_get_count(category);

    if (total == 0) {
        gfx->fillScreen(0x0000);
        dr_draw_header(display_name);
        gfx->setCursor(TEXT_START_X, 50);
        gfx->setTextColor(C_GREY);
        gfx->print("No entries yet.");
        gfx->setCursor(TEXT_START_X, 70);
        gfx->setTextColor(C_DARK);
        gfx->print("Use Gemini terminal to populate.");
        delay(3000);
        return;
    }

    int pageStart    = 0;
    int cursorRow    = 0;

    const int MAX_ROWS = MAX_LIST_ROWS;
    String titles[MAX_ROWS];
    int    rowsOnPage = 0;

    auto loadPage = [&]() {
        rowsOnPage = 0;
        for (int i = 0; i < MAX_ROWS; i++) {
            int entryIdx = pageStart + i;
            if (entryIdx >= total) break;
            String content;
            nosql_get_entry(category, entryIdx, titles[i], content);
            rowsOnPage++;
        }
    };

    auto drawList = [&]() {
        gfx->fillRect(0, TEXT_START_Y, SCREEN_W, SCREEN_H - TEXT_START_Y - 12, 0x0000);
        for (int i = 0; i < rowsOnPage; i++) {
            dr_draw_row(i, pageStart + i, titles[i], (i == cursorRow));
        }
    };

    auto drawPageHeader = [&]() {
        char hdr[48];
        int pg    = (pageStart / MAX_LIST_ROWS) + 1;
        int pgMax = ((total - 1) / MAX_LIST_ROWS) + 1;
        snprintf(hdr, sizeof(hdr), "%s  [%d/%d]", display_name, pg, pgMax);
        dr_draw_header(hdr, "HDR:EXIT");

        gfx->setTextColor(C_GREY);
        gfx->setTextSize(1);
        gfx->setCursor(TEXT_START_X, SCREEN_H - 10);
        gfx->print("N/\x1A:next  P/\x1B:prev  S:search  CLK:open");
    };

    gfx->fillScreen(0x0000);
    loadPage();
    drawPageHeader();
    drawList();

    while (true) {
        bool redraw = false;

        TrackballState tb = update_trackball();

        if (tb.y == -1) {
            if (cursorRow > 0) {
                int old = cursorRow;
                cursorRow--;
                dr_draw_row(old,       pageStart + old,       titles[old],       false);
                dr_draw_row(cursorRow, pageStart + cursorRow,  titles[cursorRow], true);
            } else if (pageStart > 0) {
                pageStart -= MAX_LIST_ROWS;
                if (pageStart < 0) pageStart = 0;
                loadPage();
                cursorRow = rowsOnPage - 1;
                redraw = true;
            }
        } else if (tb.y == 1) {
            if (cursorRow < rowsOnPage - 1) {
                int old = cursorRow;
                cursorRow++;
                dr_draw_row(old,       pageStart + old,       titles[old],       false);
                dr_draw_row(cursorRow, pageStart + cursorRow,  titles[cursorRow], true);
            } else if (pageStart + MAX_LIST_ROWS < total) {
                pageStart += MAX_LIST_ROWS;
                loadPage();
                cursorRow = 0;
                redraw = true;
            }
        } else if (tb.x == 1) {
            if (pageStart + MAX_LIST_ROWS < total) {
                pageStart += MAX_LIST_ROWS;
                loadPage();
                cursorRow = 0;
                redraw = true;
            }
        } else if (tb.x == -1) {
            if (pageStart >= MAX_LIST_ROWS) {
                pageStart -= MAX_LIST_ROWS;
                loadPage();
                cursorRow = 0;
                redraw = true;
            }
        } else if (tb.clicked) {
            dr_view_entry(category, pageStart + cursorRow);
            redraw = true;
        }

        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < HEADER_H) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
                return;
            }
            int row = (ty - TEXT_START_Y) / ROW_H;
            if (row >= 0 && row < rowsOnPage) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
                int old = cursorRow;
                cursorRow = row;
                dr_draw_row(old,       pageStart + old,       titles[old],       false);
                dr_draw_row(cursorRow, pageStart + cursorRow,  titles[cursorRow], true);
                dr_view_entry(category, pageStart + cursorRow);
                redraw = true;
            }
        }

        char c = get_keypress();
        if (c == 'n' || c == 'N') {
            if (pageStart + MAX_LIST_ROWS < total) {
                pageStart += MAX_LIST_ROWS;
                loadPage(); cursorRow = 0; redraw = true;
            }
        } else if (c == 'p' || c == 'P') {
            if (pageStart >= MAX_LIST_ROWS) {
                pageStart -= MAX_LIST_ROWS;
                loadPage(); cursorRow = 0; redraw = true;
            }
        } else if (c == 's' || c == 'S') {
            dr_search_screen(category, display_name);
            redraw = true;
        } else if (c == 27) {
            return;
        }

        if (redraw) {
            gfx->fillScreen(0x0000);
            drawPageHeader();
            drawList();
        }

        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_data_reader(const char* category, const char* display_name) {
    nosql_init(category);
    dr_browse(category, display_name);
}
