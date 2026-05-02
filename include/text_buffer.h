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

#ifndef TEXT_BUFFER_H
#define TEXT_BUFFER_H

#include <Arduino.h>

// ─────────────────────────────────────────────
//  TEXT BUFFER — Scrollback for any app that displays
//  variable-length text output.
//
//  Lives in PSRAM. Circular buffer of up to 200 lines
//  of up to 56 chars each (= ~11KB).
//
//  USAGE:
//    TextBuffer tb;
//    tb.init();                       // Allocate PSRAM
//    tb.append_wrapped("long text"); // Word-wraps to buffer
//    tb.scroll_up();   tb.scroll_down();
//    tb.scroll_to_bottom();
//    tb.draw(x, y, w, h, textSize);  // Render visible window
//    tb.free();                       // Release PSRAM on app exit
//
//  Call free() in every exit path of the app or you'll
//  leak 11KB of PSRAM per session.
// ─────────────────────────────────────────────

#define TB_MAX_LINES      200
#define TB_LINE_WIDTH     56
#define TB_DEFAULT_COLOR  0xFFFF

struct TextLine {
    char     text[TB_LINE_WIDTH];
    uint16_t color;
};

class TextBuffer {
public:
    TextLine* lines        = nullptr;
    int       count        = 0;     // Total lines in buffer
    int       scroll       = 0;     // Index of topmost visible line
    int       visible_rows = 16;    // How many lines fit on screen

    bool init();
    void free_mem();
    void clear();
    void append(const char* text, uint16_t color = TB_DEFAULT_COLOR);
    void append_wrapped(const char* text, uint16_t color = TB_DEFAULT_COLOR);
    void scroll_up(int n = 1);
    void scroll_down(int n = 1);
    void scroll_to_bottom();
    bool at_bottom() const;
    void draw(int x, int y, int w, int h, int textSize = 1);
};

#endif
