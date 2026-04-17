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
 * PISCES MOON OS — GEMINI TERMINAL v2.0
 *
 * Changes from v1.x:
 *   - Long AI responses are now paginated — trackball up/down scrolls
 *     line-by-line, SPACE / trackball-right advances a full page,
 *     BACKSPACE / trackball-left goes back a page.
 *   - Word-wrap renders correctly at 53 chars per line (320px / 6px/char).
 *   - ENTER at the end of a response clears the screen for the next prompt.
 *   - Header tap always exits cleanly.
 *
 * Display layout (320×240):
 *   y  0-23  : header bar (fixed)
 *   y 24-209 : text content area — 30 lines × 6px = 180px usable
 *              at textSize(1): 8px/line → ~23 lines visible
 *   y 210-239: footer / scroll indicator (fixed)
 *
 * Controls (response pager):
 *   Trackball up/down  = scroll one line
 *   SPACE / PgDn       = next page
 *   BACKSPACE / PgUp   = previous page
 *   ENTER              = done, back to prompt
 *   Header tap         = exit app
 */

#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "keyboard.h"
#include "apps.h"
#include "gemini_client.h"

extern Arduino_GFX *gfx;

// ─────────────────────────────────────────────
//  LAYOUT CONSTANTS
// ─────────────────────────────────────────────
#define TRM_HEADER_H    24
#define TRM_FOOTER_H    18
#define TRM_CONTENT_Y   (TRM_HEADER_H + 2)
#define TRM_CONTENT_H   (240 - TRM_HEADER_H - TRM_FOOTER_H)
#define TRM_LINE_H      9       // pixels per line at textSize(1) with 1px gap
#define TRM_LINES_PER_PAGE  ((TRM_CONTENT_H) / TRM_LINE_H)   // ~21
#define TRM_CHARS_PER_LINE  52  // (320 - 10px margin) / 6px per char

#define C_TRM_BG       0x0000
#define C_TRM_HEADER   0x0821
#define C_TRM_BORDER   0x07E0
#define C_TRM_PROMPT   0x07E0   // Green for USER>
#define C_TRM_RESPONSE 0xFFFF   // White for AI text
#define C_TRM_DIM      0x4208
#define C_TRM_AMBER    0xFD20
#define C_TRM_RED      0xF800

// ─────────────────────────────────────────────
//  WORD-WRAP ENGINE
//  Breaks a String into lines of at most
//  TRM_CHARS_PER_LINE characters, respecting
//  word boundaries. Returns line count.
//  Lines are stored in a heap-allocated array
//  — caller must free with delete[].
// ─────────────────────────────────────────────
static int wrapText(const String& text, String*& lines) {
    // Conservative upper bound: every char could be a line break
    int maxLines = (text.length() / 10) + 4;
    lines = new String[maxLines];
    int lineCount = 0;

    int start = 0;
    int len   = text.length();

    while (start < len) {
        // Skip leading spaces on new lines (except first)
        if (lineCount > 0) {
            while (start < len && text[start] == ' ') start++;
        }
        if (start >= len) break;

        // Find a natural newline first
        int nlPos = text.indexOf('\n', start);
        int end;
        bool forceBreak = false;

        if (nlPos >= 0 && nlPos - start <= TRM_CHARS_PER_LINE) {
            end = nlPos;
            forceBreak = true;  // consume the newline
        } else if (len - start <= TRM_CHARS_PER_LINE) {
            end = len;
        } else {
            // Find last space within the limit
            end = start + TRM_CHARS_PER_LINE;
            int spacePos = -1;
            for (int i = end; i > start; i--) {
                if (text[i] == ' ') { spacePos = i; break; }
            }
            if (spacePos > start) end = spacePos;
        }

        if (lineCount < maxLines) {
            lines[lineCount++] = text.substring(start, end);
        }
        start = end + (forceBreak ? 1 : 0);
    }

    return lineCount;
}

// ─────────────────────────────────────────────
//  DRAW HELPERS
// ─────────────────────────────────────────────
static void drawHeader(const char* msg) {
    gfx->fillRect(0, 0, 320, TRM_HEADER_H, C_TRM_HEADER);
    gfx->drawFastHLine(0, TRM_HEADER_H, 320, C_TRM_BORDER);
    gfx->setTextSize(1);
    gfx->setTextColor(C_TRM_BORDER);
    gfx->setCursor(10, 7);
    gfx->print(msg);
}

static void drawFooter(int firstLine, int totalLines) {
    gfx->fillRect(0, 240 - TRM_FOOTER_H, 320, TRM_FOOTER_H, C_TRM_HEADER);
    gfx->drawFastHLine(0, 240 - TRM_FOOTER_H, 320, C_TRM_DIM);
    gfx->setTextSize(1);

    if (totalLines <= TRM_LINES_PER_PAGE) {
        // Short response — no paging needed
        gfx->setTextColor(C_TRM_DIM);
        gfx->setCursor(60, 240 - TRM_FOOTER_H + 5);
        gfx->print("ENTER:next prompt  HDR:exit");
    } else {
        int lastLine  = min(firstLine + TRM_LINES_PER_PAGE, totalLines);
        int pctDone   = (lastLine * 100) / totalLines;

        // Progress bar
        int barW = 160;
        int barX = (320 - barW) / 2;
        int barY = 240 - TRM_FOOTER_H + 6;
        gfx->drawRect(barX, barY, barW, 5, C_TRM_DIM);
        gfx->fillRect(barX + 1, barY + 1, (barW - 2) * pctDone / 100, 3, C_TRM_BORDER);

        gfx->setTextColor(C_TRM_DIM);
        gfx->setCursor(4, 240 - TRM_FOOTER_H + 5);
        gfx->printf("%d/%d", lastLine, totalLines);

        if (lastLine < totalLines) {
            gfx->setTextColor(C_TRM_AMBER);
            gfx->setCursor(255, 240 - TRM_FOOTER_H + 5);
            gfx->print("SPC:more");
        } else {
            gfx->setTextColor(C_TRM_BORDER);
            gfx->setCursor(234, 240 - TRM_FOOTER_H + 5);
            gfx->print("END  ENTER:next");
        }
    }
}

// ─────────────────────────────────────────────
//  PAGE RENDERER
//  Draws lines[firstLine .. firstLine+page_size)
//  into the content area.
// ─────────────────────────────────────────────
static void renderPage(String* lines, int lineCount, int firstLine) {
    gfx->fillRect(0, TRM_CONTENT_Y, 320, TRM_CONTENT_H, C_TRM_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(C_TRM_RESPONSE);

    int y = TRM_CONTENT_Y + 2;
    for (int i = firstLine; i < lineCount && i < firstLine + TRM_LINES_PER_PAGE; i++) {
        gfx->setCursor(5, y);
        gfx->print(lines[i]);
        y += TRM_LINE_H;
    }

    drawFooter(firstLine, lineCount);
}

// ─────────────────────────────────────────────
//  RESPONSE PAGER
//  Displays a (possibly long) response string
//  with full scroll support. Returns true if
//  the user wants to continue chatting, false
//  if they tapped the header to exit.
// ─────────────────────────────────────────────
static bool showResponse(const String& response) {
    String* lines = nullptr;
    int lineCount = wrapText(response, lines);
    int firstLine = 0;

    drawHeader("GEMINI TERMINAL | TAP HEADER EXIT");
    renderPage(lines, lineCount, firstLine);

    bool keepGoing = true;

    while (true) {
        // Header tap = exit app
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            if (ty < TRM_HEADER_H) { keepGoing = false; break; }
        }

        // Trackball
        TrackballState tb = update_trackball();
        if (tb.y == -1 && firstLine > 0) {
            firstLine--;
            renderPage(lines, lineCount, firstLine);
        } else if (tb.y == 1 && firstLine + TRM_LINES_PER_PAGE < lineCount) {
            firstLine++;
            renderPage(lines, lineCount, firstLine);
        } else if (tb.x == 1) {
            // Right = page forward
            int next = firstLine + TRM_LINES_PER_PAGE;
            if (next < lineCount) { firstLine = next; renderPage(lines, lineCount, firstLine); }
        } else if (tb.x == -1) {
            // Left = page back
            int prev = firstLine - TRM_LINES_PER_PAGE;
            firstLine = max(0, prev);
            renderPage(lines, lineCount, firstLine);
        }

        char c = get_keypress();
        if (c == 13 || c == 10) {
            // ENTER = done reading, back to prompt
            break;
        } else if (c == ' ') {
            // SPACE = next page
            int next = firstLine + TRM_LINES_PER_PAGE;
            if (next < lineCount) { firstLine = next; renderPage(lines, lineCount, firstLine); }
            else break;  // Already at end — treat as done
        } else if (c == 8 || c == 127) {
            // BACKSPACE = previous page
            int prev = firstLine - TRM_LINES_PER_PAGE;
            firstLine = max(0, prev);
            renderPage(lines, lineCount, firstLine);
        }

        delay(30);
        yield();
    }

    delete[] lines;
    return keepGoing;
}

// ─────────────────────────────────────────────
//  MAIN TERMINAL LOOP
// ─────────────────────────────────────────────
void run_terminal() {
    gfx->fillScreen(C_TRM_BG);
    drawHeader("GEMINI TERMINAL | TAP HEADER EXIT");

    // Prompt area
    gfx->fillRect(0, TRM_CONTENT_Y, 320, TRM_CONTENT_H, C_TRM_BG);
    gfx->fillRect(0, 240 - TRM_FOOTER_H, 320, TRM_FOOTER_H, C_TRM_HEADER);
    gfx->drawFastHLine(0, 240 - TRM_FOOTER_H, 320, C_TRM_DIM);
    gfx->setTextColor(C_TRM_DIM); gfx->setTextSize(1);
    gfx->setCursor(60, 240 - TRM_FOOTER_H + 5);
    gfx->print("Type prompt, press ENTER");

    int promptY = TRM_CONTENT_Y + 4;

    while (true) {
        // Exit check
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < TRM_HEADER_H) {
            while (get_touch(&tx, &ty)) { delay(10); yield(); }
            return;
        }

        // Prompt label
        gfx->setTextColor(C_TRM_PROMPT);
        gfx->setTextSize(1);
        gfx->setCursor(5, promptY);
        gfx->print("USER> ");

        // Get input
        String prompt = get_text_input(41, promptY);
        if (prompt == "##EXIT##") return;
        if (prompt.length() == 0) { yield(); continue; }

        // Show "thinking" indicator
        gfx->fillRect(0, TRM_CONTENT_Y, 320, TRM_CONTENT_H, C_TRM_BG);
        gfx->setCursor(5, promptY);
        gfx->setTextColor(C_TRM_PROMPT);
        gfx->print("USER> ");
        gfx->setTextColor(0xC618);

        // Truncate display of long prompts
        String displayPrompt = prompt;
        if (displayPrompt.length() > 44) displayPrompt = displayPrompt.substring(0, 41) + "...";
        gfx->print(displayPrompt);

        gfx->setCursor(5, promptY + TRM_LINE_H + 2);
        gfx->setTextColor(C_TRM_AMBER);
        gfx->print("AI> Thinking...");

        // Call Gemini
        String response = ask_gemini(prompt);

        // Show paged response — exit app if user tapped header
        gfx->fillScreen(C_TRM_BG);
        drawHeader("GEMINI TERMINAL | TAP HEADER EXIT");

        bool continueChat = showResponse(response);
        if (!continueChat) return;

        // Clear content area for next prompt
        gfx->fillScreen(C_TRM_BG);
        drawHeader("GEMINI TERMINAL | TAP HEADER EXIT");
        gfx->fillRect(0, TRM_CONTENT_Y, 320, TRM_CONTENT_H, C_TRM_BG);
        gfx->fillRect(0, 240 - TRM_FOOTER_H, 320, TRM_FOOTER_H, C_TRM_HEADER);
        gfx->drawFastHLine(0, 240 - TRM_FOOTER_H, 320, C_TRM_DIM);
        gfx->setTextColor(C_TRM_DIM); gfx->setTextSize(1);
        gfx->setCursor(60, 240 - TRM_FOOTER_H + 5);
        gfx->print("Type prompt, press ENTER");

        promptY = TRM_CONTENT_Y + 4;
        yield();
    }
}
