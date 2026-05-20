// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)
//
// The text-pager and file navigator both use gfx->width() throughout so they
// scale to whichever device they're running on.
//
// SPI Bus Treaty (v1.2): Every SdFat operation in this file is wrapped in
// PM_SPI_TAKE / PM_SPI_GIVE so the file manager cannot corrupt the wardrive
// task's SD writes (and vice versa). The sd_in_use sentinel is asserted on
// entry to run_filesystem() and view_text_file() so the Ghost Engine pauses
// its SD writes entirely while the user is browsing — pure mutex contention
// would otherwise cause wardrive packet drops every time the user pages
// through a folder.
//
// Root cause of the prior crash-loop: sd.open(directory) without mutex would
// race with wardrive's append writes, sometimes returning null. The original
// code reset current_path = "/" and continue'd, which immediately tried
// sd.open() again, which failed again, producing the visible "loops forever"
// behavior reported in v1.2.0 testing.

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "SdFat.h"
#include "keyboard.h"
#include "pm_input.h"
#include "touch.h"
#include "trackball.h"
#include "theme.h"
#include "apps.h"
#include "spi_treaty.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif
extern SdFat sd;
extern volatile bool sd_in_use;   // Defined in gemini_client.cpp; honored by wardrive.cpp

// ─────────────────────────────────────────────
//  UNIVERSAL TEXT PAGER
// ─────────────────────────────────────────────
void view_text_file(String path) {
    const int W = gfx->width();
    const int H = gfx->height();
    const int WRAP_X = W - 10;
    const int FOOTER_Y = H - 25;
    const int FOOTER_TXT_Y = H - 18;
    const int BODY_TOP = 35;
    const int BODY_BOTTOM = FOOTER_Y - 5;

    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, W, 24, C_DARK);
    gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN);
    gfx->print("READING: " + path.substring(0, 20));

    // SPI Bus Treaty (v1.2):
    //   sd_in_use tells the wardrive Ghost Engine task to pause its
    //   SD writes for the duration of this view. The recursive mutex
    //   take protects against any other task simultaneously touching
    //   the bus. Both must be paired with their release on EVERY
    //   exit path below.
    sd_in_use = true;

    if (!PM_SPI_TAKE("filesystem.view_text_file")) {
        sd_in_use = false;
        gfx->setCursor(10, 50); gfx->setTextColor(C_RED);
        gfx->print("SD busy. Try again.");
        delay(2000); return;
    }

    FsFile file = sd.open(path.c_str(), O_READ);
    if (!file) {
        PM_SPI_GIVE();
        sd_in_use = false;
        gfx->setCursor(10, 50); gfx->setTextColor(C_RED);
        gfx->print("Error opening file.");
        delay(2000); return;
    }

    int cx = 5, cy = BODY_TOP;
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
            if (cx > WRAP_X) { cx = 5; cy += 12; }
        }
        if (cy > BODY_BOTTOM) {
            gfx->fillRect(0, FOOTER_Y, W, 25, C_DARK);
            gfx->setCursor(10, FOOTER_TXT_Y); gfx->setTextColor(C_GREEN);
            gfx->print("[SPACE/CLK] Next  [B] Back");

            while (true) {
                char k = get_keypress();
                if (k == 'b' || k == 'B' || pm_is_exit_key(k)) {
                    file.close();
                    PM_SPI_GIVE();
                    sd_in_use = false;
                    return;
                }
                if (k == ' ') break;
                TrackballState tb = update_trackball();
                if (tb.clicked) break;
                if (tb.x == -1) {
                    file.close();
                    PM_SPI_GIVE();
                    sd_in_use = false;
                    return;
                }
                int16_t tx, ty;
                if (get_touch(&tx, &ty)) {
                    if (ty > FOOTER_Y - 5) { while(get_touch(&tx,&ty)){delay(10);} break; }
                    if (ty < 30) {
                        while(get_touch(&tx,&ty)){delay(10);}
                        file.close();
                        PM_SPI_GIVE();
                        sd_in_use = false;
                        return;
                    }
                }
                delay(20);
            }
            gfx->fillRect(0, 25, W, FOOTER_Y - 25, C_BLACK);
            cx = 5; cy = BODY_TOP;
            gfx->setTextColor(C_WHITE);
        }
    }

    gfx->fillRect(0, FOOTER_Y, W, 25, C_DARK);
    gfx->setCursor(10, FOOTER_TXT_Y); gfx->setTextColor(C_GREY);
#ifdef DEVICE_TLORAPAGER
    gfx->print("[END] Q, [B], or CLK to exit");
#else
    gfx->print("[END] Tap Header, [B], or CLK to exit");
#endif

    while (true) {
        char k = get_keypress();
        if (k == 'b' || k == 'B' || pm_is_exit_key(k)) break;
        TrackballState tb = update_trackball();
        if (tb.clicked) break;
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 30) { while(get_touch(&tx,&ty)){delay(10);} break; }
        delay(20);
    }
    file.close();
    PM_SPI_GIVE();
    sd_in_use = false;
}

// ─────────────────────────────────────────────
//  DRAW HELPERS
// ─────────────────────────────────────────────
static void fs_draw_row(int i, const String &name, bool isDir,
                        uint32_t sizeKb, bool highlighted) {
    int y = 45 + (i * 20);
    int W = gfx->width();
    gfx->fillRect(0, y, W, 18, highlighted ? (uint16_t)C_DARK : (uint16_t)C_BLACK);
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
    int page_offset = 0;
    int cursorIdx   = 0;

    // SPI Bus Treaty (v1.2): Tell the Ghost Engine to pause SD writes
    // while the user is browsing. Re-asserted every loop iteration in
    // case any helper cleared it. Cleared on every exit path.
    sd_in_use = true;

    while (true) {
        int W = gfx->width();
        int H = gfx->height();

        gfx->fillScreen(C_BLACK);
        gfx->fillRect(0, 0, W, 24, C_DARK);
        gfx->setCursor(10, 7); gfx->setTextColor(C_GREEN);
        gfx->print("SYS: " + current_path.substring(0, 26));

        // SPI Bus Treaty: take the mutex before any SdFat call. This
        // protects the directory scan from racing wardrive's CSV
        // writes. Held through dir.close() then released so the
        // input loop below can run without blocking other tasks.
        if (!PM_SPI_TAKE("filesystem.scan_dir")) {
            gfx->setCursor(10, 50); gfx->setTextColor(C_RED);
            gfx->print("SD busy. Try again.");
            delay(2000);
            sd_in_use = false;
            return;
        }

        FsFile dir = sd.open(current_path.c_str());
        if (!dir) {
            PM_SPI_GIVE();
            // Reset to root and retry on next iteration. The previous
            // implementation crashed here in a loop because the
            // sd.open call kept racing wardrive's writes; with the
            // mutex in place the failure is now genuine (e.g. SD
            // not mounted) and is shown to the user briefly.
            gfx->setCursor(10, 50); gfx->setTextColor(0xFD20);
            gfx->print("Path not accessible.");
            delay(800);
            if (current_path == "/") {
                // Can't recover — return to launcher
                sd_in_use = false;
                return;
            }
            current_path = "/"; page_offset = 0; continue;
        }

        String   items[9];
        bool     is_dir[9];
        uint32_t item_sizes[9];
        int count = 0;

        if (current_path != "/") {
            items[count] = "/";  is_dir[count] = true; item_sizes[count] = 0; count++;
            items[count] = ".."; is_dir[count] = true; item_sizes[count] = 0; count++;
        }

        FsFile entry;
        dir.rewind();
        int skipped = 0;
        while (skipped < page_offset && entry.openNext(&dir, O_READ)) {
            entry.close(); skipped++;
        }

        while (count < 8 && entry.openNext(&dir, O_READ)) {
            char name[64];
            entry.getName(name, sizeof(name));
            items[count]      = String(name);
            is_dir[count]     = entry.isDir();
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

        // SPI Bus Treaty: release the mutex for the input loop. Other
        // tasks can use the bus while the user is reading the file
        // list and deciding what to do next. The sd_in_use sentinel
        // stays set so wardrive still defers its writes.
        PM_SPI_GIVE();

        if (cursorIdx >= count) cursorIdx = max(0, count - 1);

        for (int i = 0; i < count; i++)
            fs_draw_row(i, items[i], is_dir[i], item_sizes[i], (i == cursorIdx));

        gfx->setTextColor(C_GREY);
        gfx->setCursor(10, H - 20);
        if (page_offset > 0 && more_files)      gfx->print("[P]rev | [N]ext | CLK:open");
        else if (page_offset > 0)               gfx->print("[P]rev Page | CLK:open");
        else if (more_files)                    gfx->print("[N]ext Page | CLK:open");
        else                                    gfx->print("CLK:open | up/down navigate");

        bool refresh = false;
        while (!refresh) {
            TrackballState tb = update_trackball();
            if (tb.y == -1) {
                if (cursorIdx > 0) {
                    int old = cursorIdx--;
                    fs_draw_row(old,       items[old],       is_dir[old],       item_sizes[old],       false);
                    fs_draw_row(cursorIdx, items[cursorIdx], is_dir[cursorIdx], item_sizes[cursorIdx], true);
                }
            } else if (tb.y == 1) {
                if (cursorIdx < count - 1) {
                    int old = cursorIdx++;
                    fs_draw_row(old,       items[old],       is_dir[old],       item_sizes[old],       false);
                    fs_draw_row(cursorIdx, items[cursorIdx], is_dir[cursorIdx], item_sizes[cursorIdx], true);
                } else if (more_files) {
                    page_offset += (current_path == "/" ? 8 : 6);
                    cursorIdx = 0; refresh = true;
                }
            } else if (tb.x == -1) {
                if (current_path != "/") {
                    int lastSlash = current_path.lastIndexOf('/');
                    if (lastSlash > 0) current_path = current_path.substring(0, lastSlash);
                    else current_path = "/";
                    page_offset = 0; cursorIdx = 0; refresh = true;
                }
            } else if (tb.x == 1) {
                if (more_files) {
                    page_offset += (current_path == "/" ? 8 : 6);
                    cursorIdx = 0; refresh = true;
                }
            } else if (tb.clicked) {
                fs_open_item(cursorIdx, current_path, page_offset, items, is_dir, refresh);
                if (refresh) cursorIdx = 0;
            }

            char c = get_keypress();
            if (pm_is_exit_key(c)) {
                sd_in_use = false;
                return;
            }
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
                fs_open_item(sel, current_path, page_offset, items, is_dir, refresh);
                if (refresh) cursorIdx = 0;
            }

            int16_t tx, ty;
            if (get_touch(&tx, &ty)) {
                if (ty < 30) {
                    while(get_touch(&tx,&ty)){delay(10);}
                    sd_in_use = false;
                    return;
                }
                int row = (ty - 35) / 20;
                if (row >= 0 && row < count) {
                    while(get_touch(&tx,&ty)){delay(10);}
                    cursorIdx = row;
                    fs_draw_row(row, items[row], is_dir[row], item_sizes[row], true);
                    fs_open_item(row, current_path, page_offset, items, is_dir, refresh);
                    if (refresh) cursorIdx = 0;
                }
            }
            delay(20);
        }
    }
}
