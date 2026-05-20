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

#include <Arduino.h>
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "text_buffer.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif

// ─────────────────────────────────────────────
//  LIFECYCLE
// ─────────────────────────────────────────────
bool TextBuffer::init() {
    if (lines) return true;  // Already initialized

    size_t sz = TB_MAX_LINES * sizeof(TextLine);

    // Prefer PSRAM if available. On no-PSRAM devices (Cardputer ADV)
    // ps_malloc returns NULL — fall back to internal SRAM. The buffer
    // is small (~11.6 KB for default TB_MAX_LINES=200, TB_LINE_WIDTH=56)
    // and fits comfortably in internal heap.
    lines = (TextLine*)ps_malloc(sz);
    if (!lines) {
        lines = (TextLine*)malloc(sz);
        if (!lines) {
            Serial.println("[TextBuffer] malloc failed (both PSRAM and SRAM)");
            return false;
        }
        Serial.println("[TextBuffer] PSRAM unavailable — using internal SRAM");
    }

    memset(lines, 0, sz);
    count = scroll = 0;
    return true;
}

void TextBuffer::free_mem() {
    if (lines) {
        free(lines);
        lines = nullptr;
    }
    count = scroll = 0;
}

void TextBuffer::clear() {
    count = scroll = 0;
    if (lines) memset(lines, 0, TB_MAX_LINES * sizeof(TextLine));
}

// ─────────────────────────────────────────────
//  APPEND — single line (truncated to TB_LINE_WIDTH)
// ─────────────────────────────────────────────
void TextBuffer::append(const char* text, uint16_t color) {
    if (!lines || !text) return;

    int dest = count;
    if (dest >= TB_MAX_LINES) {
        // Circular — drop oldest line, shift everything up
        memmove(lines, lines + 1, (TB_MAX_LINES - 1) * sizeof(TextLine));
        dest = TB_MAX_LINES - 1;
        if (scroll > 0) scroll--;
    } else {
        count++;
    }

    strncpy(lines[dest].text, text, TB_LINE_WIDTH - 1);
    lines[dest].text[TB_LINE_WIDTH - 1] = '\0';
    lines[dest].color = color;
}

// ─────────────────────────────────────────────
//  APPEND_WRAPPED — breaks long text at word boundaries
//  Respects explicit \n in input.
// ─────────────────────────────────────────────
void TextBuffer::append_wrapped(const char* text, uint16_t color) {
    if (!lines || !text) return;

    const int WRAP_AT = TB_LINE_WIDTH - 1;
    const char* p = text;

    while (*p) {
        // Handle explicit newlines: emit empty or partial line
        const char* nl = strchr(p, '\n');
        int segment_len = nl ? (nl - p) : strlen(p);

        // Word-wrap this segment
        while (segment_len > 0) {
            int take = segment_len > WRAP_AT ? WRAP_AT : segment_len;

            // If we're mid-word, try to break at last space
            if (take < segment_len) {
                int space = take;
                while (space > 0 && p[space] != ' ') space--;
                if (space > WRAP_AT / 2) take = space;  // Only honor if reasonable
            }

            char buf[TB_LINE_WIDTH];
            strncpy(buf, p, take);
            buf[take] = '\0';
            append(buf, color);

            p += take;
            segment_len -= take;
            // Skip leading space on next line
            while (*p == ' ' && segment_len > 0) { p++; segment_len--; }
        }

        if (nl) { p = nl + 1; }
        else break;
    }
}

// ─────────────────────────────────────────────
//  SCROLL
// ─────────────────────────────────────────────
void TextBuffer::scroll_up(int n) {
    scroll -= n;
    if (scroll < 0) scroll = 0;
}

void TextBuffer::scroll_down(int n) {
    scroll += n;
    int max_scroll = count - visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;
}

void TextBuffer::scroll_to_bottom() {
    scroll = count - visible_rows;
    if (scroll < 0) scroll = 0;
}

bool TextBuffer::at_bottom() const {
    int max_scroll = count - visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    return scroll >= max_scroll;
}

// ─────────────────────────────────────────────
//  DRAW — render visible window into rect (x,y,w,h)
// ─────────────────────────────────────────────
void TextBuffer::draw(int x, int y, int w, int h, int textSize) {
    if (!lines) return;

    // Clear render area
    gfx->fillRect(x, y, w, h, 0x0000);

    int lineHeight = (textSize == 1) ? 10 : 16;
    visible_rows = h / lineHeight;

    // Clamp scroll to valid range
    int max_scroll = count - visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;

    gfx->setTextSize(textSize);

    for (int i = 0; i < visible_rows; i++) {
        int idx = scroll + i;
        if (idx >= count) break;
        gfx->setTextColor(lines[idx].color);
        gfx->setCursor(x + 2, y + i * lineHeight + 1);
        gfx->print(lines[idx].text);
    }

    // Scrollbar on the right edge
    if (count > visible_rows) {
        int track_h = h - 4;
        int thumb_h = (track_h * visible_rows) / count;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = y + 2 + ((track_h - thumb_h) * scroll) / max_scroll;
        gfx->drawFastVLine(x + w - 3, y + 2, track_h, 0x2104);
        gfx->fillRect(x + w - 4, thumb_y, 3, thumb_h, 0x07E0);
    }
}