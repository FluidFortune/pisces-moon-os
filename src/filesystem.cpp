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
 * PISCES MOON OS — filesystem.cpp
 * Trackball added to navigator:
 *   Up/Down  — move cursor through list items
 *   Left     — go up one directory level (..)
 *   Right    — next page (if available)
 *   Click    — open file or enter directory
 * Touch and keyboard shortcuts unchanged.
 */

#include <Arduino_GFX_Library.h>
#include "SdFat.h"
#include "keyboard.h"
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "apps.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  UNIVERSAL TEXT PAGER
// ─────────────────────────────────────────────
void view_text_file(String path) {
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN);
    gfx->print("READING: " + path.substring(0, 20));

    FsFile file = sd.open(path.c_str(), O_READ);
    if (!file) {
        gfx->setCursor(10, 50); gfx->setTextColor(C_RED);
        gfx->print("Error opening file.");
        delay(2000); return;
    }

    int cx = 5, cy = 35;
    gfx->setTextColor(C_WHITE);

    while (file.available()) {
        char c = file.read();

        if (c == '\r') continue;

        if (c == '\n') {
            cx = 5; cy += 12;
        } else {
            gfx->setCursor(cx, cy);
            gfx->print(c);
            cx += 6;
            if (cx > 310) { cx = 5; cy += 12; }
        }

        if (cy > 195) {
            gfx->fillRect(0, 215, 320, 25, C_DARK);
            gfx->setCursor(10, 222); gfx->setTextColor(C_GREEN);
            gfx->print("[SPACE/CLK] Next  [B] Back");

            while (true) {
                char k = get_keypress();
                if (k == 'b' || k == 'B') { file.close(); return; }
                if (k == ' ') break;

                TrackballState tb = update_trackball();
                if (tb.clicked) break;      // Click = next page
                if (tb.x == -1) { file.close(); return; } // Left = back

                int16_t tx, ty;
                if (get_touch(&tx, &ty)) {
                    if (ty > 210) { while(get_touch(&tx,&ty)){delay(10);} break; }
                    if (ty < 30)  { while(get_touch(&tx,&ty)){delay(10);} file.close(); return; }
                }
                delay(20);
            }

            gfx->fillRect(0, 25, 320, 190, C_BLACK);
            cx = 5; cy = 35;
            gfx->setTextColor(C_WHITE);
        }
    }

    // End of file
    gfx->fillRect(0, 215, 320, 25, C_DARK);
    gfx->setCursor(10, 222); gfx->setTextColor(C_GREY);
    gfx->print("[END] Tap Header, [B], or CLK to exit");

    while (true) {
        char k = get_keypress();
        if (k == 'b' || k == 'B') break;

        TrackballState tb = update_trackball();
        if (tb.clicked) break;

        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 30) { while(get_touch(&tx,&ty)){delay(10);} break; }
        delay(20);
    }
    file.close();
}

// ─────────────────────────────────────────────
//  DRAW HELPERS
// ─────────────────────────────────────────────

// Draw one file/folder row, optionally highlighted
static void fs_draw_row(int i, const String &name, bool isDir,
                        uint32_t sizeKb, bool highlighted) {
    int y = 45 + (i * 20);
    gfx->fillRect(0, y, 320, 18, highlighted ? (uint16_t)C_DARK : (uint16_t)C_BLACK);

    if (isDir) {
        gfx->setTextColor(highlighted ? C_WHITE : C_CYAN);
        gfx->setCursor(10, y);
        gfx->printf("[%d] %s/", i + 1, name.c_str());
    } else {
        gfx->setTextColor(highlighted ? C_WHITE : 0xC618);
        gfx->setCursor(10, y);
        gfx->printf("[%d] %s (%d KB)", i + 1, name.c_str(), sizeKb);
    }
}

// ─────────────────────────────────────────────
//  NAVIGATE: shared action for opening a selected item
// ─────────────────────────────────────────────
// Returns true if a full redraw is needed, false if just re-entering.
static void fs_open_item(int sel, String &current_path, int &page_offset,
                          const String items[], const bool is_dir[],
                          bool &refresh) {
    if (items[sel] == "/") {
        current_path = "/"; page_offset = 0; refresh = true;
    } else if (items[sel] == "..") {
        int lastSlash = current_path.lastIndexOf('/');
        if (lastSlash > 0) current_path = current_path.substring(0, lastSlash);
        else current_path = "/";
        page_offset = 0; refresh = true;
    } else if (is_dir[sel]) {
        if (current_path == "/") current_path += items[sel];
        else current_path += "/" + items[sel];
        page_offset = 0; refresh = true;
    } else {
        String full_path = (current_path == "/")
            ? "/" + items[sel]
            : current_path + "/" + items[sel];
        view_text_file(full_path);
        refresh = true;
    }
}

// ─────────────────────────────────────────────
//  MAIN FILE SYSTEM NAVIGATOR
// ─────────────────────────────────────────────
void run_filesystem() {
    String current_path = "/";
    int page_offset  = 0;
    int cursorIdx    = 0;   // Currently highlighted row (0-based within visible list)

    while (true) {
        // ── Build directory listing ──
        gfx->fillScreen(C_BLACK);
        gfx->fillRect(0, 0, 320, 24, C_DARK);
        gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN);
        gfx->print("SYS: " + current_path.substring(0, 26));

        FsFile dir = sd.open(current_path.c_str());
        if (!dir) {
            current_path = "/";
            page_offset  = 0;
            continue;
        }

        String   items[9];
        bool     is_dir[9];
        uint32_t item_sizes[9];
        int count = 0;

        // Inject / and .. when inside a subdirectory
        if (current_path != "/") {
            items[count] = "/";  is_dir[count] = true; item_sizes[count] = 0; count++;
            items[count] = ".."; is_dir[count] = true; item_sizes[count] = 0; count++;
        }

        // Skip to page offset
        FsFile entry;
        dir.rewind();
        int skipped = 0;
        while (skipped < page_offset && entry.openNext(&dir, O_READ)) {
            entry.close(); skipped++;
        }

        // Read up to 8 items
        while (count < 8 && entry.openNext(&dir, O_READ)) {
            char name[64];
            entry.getName(name, sizeof(name));
            items[count]    = String(name);
            is_dir[count]   = entry.isDir();
            item_sizes[count] = 0;
            if (!is_dir[count]) {
                item_sizes[count] = entry.size() / 1024;
                if (item_sizes[count] == 0 && entry.size() > 0) item_sizes[count] = 1;
            }
            count++;
            entry.close();
        }

        bool more_files = entry.openNext(&dir, O_READ);
        if (more_files) entry.close();
        dir.close();

        // Clamp cursor to valid range after page change
        if (cursorIdx >= count) cursorIdx = max(0, count - 1);

        // ── Draw list ──
        for (int i = 0; i < count; i++) {
            fs_draw_row(i, items[i], is_dir[i], item_sizes[i], (i == cursorIdx));
        }

        // ── Footer ──
        gfx->setTextColor(C_GREY);
        gfx->setCursor(10, 220);
        if (page_offset > 0 && more_files)      gfx->print("[P]rev | [N]ext | CLK:open");
        else if (page_offset > 0)               gfx->print("[P]rev Page | CLK:open");
        else if (more_files)                    gfx->print("[N]ext Page | CLK:open");
        else                                    gfx->print("CLK:open | \x1C\x1D navigate");

        // ── Input loop ──
        bool refresh = false;
        while (!refresh) {

            // ── Trackball ──
            TrackballState tb = update_trackball();

            if (tb.y == -1) {
                // UP
                if (cursorIdx > 0) {
                    int old = cursorIdx--;
                    fs_draw_row(old,       items[old],       is_dir[old],       item_sizes[old],       false);
                    fs_draw_row(cursorIdx, items[cursorIdx], is_dir[cursorIdx], item_sizes[cursorIdx], true);
                }
            } else if (tb.y == 1) {
                // DOWN
                if (cursorIdx < count - 1) {
                    int old = cursorIdx++;
                    fs_draw_row(old,       items[old],       is_dir[old],       item_sizes[old],       false);
                    fs_draw_row(cursorIdx, items[cursorIdx], is_dir[cursorIdx], item_sizes[cursorIdx], true);
                } else if (more_files) {
                    // Roll to next page
                    page_offset += (current_path == "/" ? 8 : 6);
                    cursorIdx = 0;
                    refresh = true;
                }
            } else if (tb.x == -1) {
                // LEFT — go up a directory (like pressing "..")
                if (current_path != "/") {
                    int lastSlash = current_path.lastIndexOf('/');
                    if (lastSlash > 0) current_path = current_path.substring(0, lastSlash);
                    else current_path = "/";
                    page_offset = 0; cursorIdx = 0; refresh = true;
                }
            } else if (tb.x == 1) {
                // RIGHT — next page
                if (more_files) {
                    page_offset += (current_path == "/" ? 8 : 6);
                    cursorIdx = 0; refresh = true;
                }
            } else if (tb.clicked) {
                // CLICK — open selected item
                fs_open_item(cursorIdx, current_path, page_offset,
                             items, is_dir, refresh);
                if (refresh) cursorIdx = 0;
            }

            // ── Keyboard ──
            char c = get_keypress();

            if (c == 'n' || c == 'N') {
                if (more_files) {
                    page_offset += (current_path == "/" ? 8 : 6);
                    cursorIdx = 0; refresh = true;
                }
            } else if (c == 'p' || c == 'P') {
                if (page_offset > 0) {
                    page_offset -= (current_path == "/" ? 8 : 6);
                    if (page_offset < 0) page_offset = 0;
                    cursorIdx = 0; refresh = true;
                }
            } else if (c >= '1' && c <= '0' + count) {
                int sel = c - '1';
                cursorIdx = sel;
                fs_open_item(sel, current_path, page_offset,
                             items, is_dir, refresh);
                if (refresh) cursorIdx = 0;
            }

            // ── Touch ──
            int16_t tx, ty;
            if (get_touch(&tx, &ty)) {
                if (ty < 30) {
                    while(get_touch(&tx,&ty)){delay(10);}
                    return; // Header tap = exit to launcher
                }
                int row = (ty - 35) / 20;
                if (row >= 0 && row < count) {
                    while(get_touch(&tx,&ty)){delay(10);}
                    cursorIdx = row;
                    fs_draw_row(row, items[row], is_dir[row], item_sizes[row], true);
                    fs_open_item(row, current_path, page_offset,
                                 items, is_dir, refresh);
                    if (refresh) cursorIdx = 0;
                }
            }

            delay(20);
        }
    }
}