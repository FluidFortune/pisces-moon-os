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
 * PISCES MOON OS — GHOST RIDE THE WHIP v1.0
 *
 * Read-only diagnostic browser for the wardrive CSV logs. The Ghost
 * Engine keeps writing to the card; this app lets you see what it's
 * laid down without stopping it. "Ghost rides the whip" — the engine
 * drives itself, you check the rear-view.
 *
 * Lists every /wardrive_*.csv file on the SD card with:
 *   - filename
 *   - file size in bytes
 *   - line count (rows of data; header counts as 1)
 *
 * Useful for verifying that wardrive rotation is working correctly:
 *   - Single file growing over time     → rotation broken
 *   - Many files, all only 1 line       → SD writes failing
 *                                          (header is written; rows aren't)
 *   - Many files of varying line counts → working correctly
 *
 * The app is read-only — it never modifies any file. It uses the same
 * SPI Bus Treaty as every other SD-touching app: sets sd_in_use while
 * scanning so wardrive pauses its writes, takes spi_mutex around every
 * SdFat call.
 *
 * Display:
 *   T-Deck Plus   (320x240): 9 rows visible, page up/down to navigate
 *   T-LoRa Pager  (480x222): 9 rows visible, m for next page, q to quit
 *   Cardputer ADV (240x135): 10 rows visible, Fn+arrows or n/p, Q to quit
 */

#include <Arduino.h>
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
#include "spi_treaty.h"

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif
extern SdFat sd;
extern volatile bool g_sd_ready;
extern volatile bool sd_in_use;

// One discovered file. Compact struct so we can fit many in fixed array.
struct WdFile {
    char     name[24];
    uint32_t size_bytes;
    uint32_t lines;        // approximate — counts '\n' chars
};

#define WI_MAX_FILES 64
static WdFile wi_files[WI_MAX_FILES];
static int    wi_file_count = 0;

// Count newlines in a file without loading it. Reads in 256-byte chunks,
// yields between chunks so the wardrive task doesn't starve.
static uint32_t count_lines_in_file(const char* path) {
    FsFile f = sd.open(path, O_READ);
    if (!f) return 0;
    uint32_t lines = 0;
    uint8_t buf[256];
    while (true) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
        }
        yield();
    }
    f.close();
    return lines;
}

// Scan SD root for /wardrive_*.csv files. Returns the count.
// Skips quietly if SD isn't ready or sd_in_use is set.
static int scan_wardrive_files() {
    wi_file_count = 0;
    if (!g_sd_ready) return 0;
    if (sd_in_use)   return 0;

    if (!PM_SPI_TAKE("wardrive_inspect.scan")) return 0;

    FsFile root = sd.open("/");
    if (!root) {
        PM_SPI_GIVE();
        return 0;
    }

    FsFile entry;
    while (wi_file_count < WI_MAX_FILES && entry.openNext(&root, O_READ)) {
        char name[64];
        entry.getName(name, sizeof(name));
        // Match wardrive_*.csv (case-insensitive on prefix only)
        bool match =
            strncasecmp(name, "wardrive_", 9) == 0 &&
            strlen(name) > 9 &&
            strcasecmp(name + strlen(name) - 4, ".csv") == 0;
        if (match && !entry.isDir()) {
            WdFile &wf = wi_files[wi_file_count];
            strncpy(wf.name, name, sizeof(wf.name) - 1);
            wf.name[sizeof(wf.name) - 1] = '\0';
            wf.size_bytes = entry.size();
            // Line count requires a separate open; defer outside this
            // openNext loop so we don't keep two FsFile handles open.
            wf.lines = 0;
            wi_file_count++;
        }
        entry.close();
    }
    root.close();
    PM_SPI_GIVE();

    // Second pass: count lines for each discovered file. One file at a
    // time, with mutex release between files so wardrive can write.
    for (int i = 0; i < wi_file_count; i++) {
        if (sd_in_use) break;   // bail if file manager grabs the card
        char path[32];
        snprintf(path, sizeof(path), "/%s", wi_files[i].name);
        if (PM_SPI_TAKE("wardrive_inspect.count")) {
            wi_files[i].lines = count_lines_in_file(path);
            PM_SPI_GIVE();
        }
        // Tight yield between files so other tasks run
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return wi_file_count;
}

// Sort files newest-first by name (lexicographic descending — works
// because the names are zero-padded /wardrive_NNNN.csv).
static void sort_files_desc() {
    for (int i = 0; i < wi_file_count - 1; i++) {
        for (int j = i + 1; j < wi_file_count; j++) {
            if (strcmp(wi_files[i].name, wi_files[j].name) < 0) {
                WdFile tmp = wi_files[i];
                wi_files[i] = wi_files[j];
                wi_files[j] = tmp;
            }
        }
        yield();
    }
}

#ifdef DEVICE_CARDPUTER_ADV
// ── Cardputer 240x135 ─────────────────────────────────────
static void run_wardrive_inspect_cp() {
    int sel = 0;
    int page = 0;
    const int ROWS_PER_PAGE = 10;

    // Initial scan
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0xFFE0); gfx->setTextSize(1);
    gfx->setCursor(4, 60); gfx->print("Scanning SD card...");

    // Pause wardrive while we scan to avoid bus contention
    sd_in_use = true;
    int n = scan_wardrive_files();
    sd_in_use = false;
    sort_files_desc();

    while (true) {
        gfx->fillScreen(0x0000);
        // Header
        gfx->fillRect(0, 0, 240, 14, 0x18C3);
        gfx->setTextSize(1); gfx->setTextColor(0x07E0);
        gfx->setCursor(4, 4);
        gfx->printf("GHOST RIDE THE WHIP (%d) Q EXIT", n);

        if (!g_sd_ready) {
            gfx->setTextColor(0xF800);
            gfx->setCursor(4, 60); gfx->print("SD not ready");
        } else if (n == 0) {
            gfx->setTextColor(0xC618);
            gfx->setCursor(4, 60); gfx->print("No wardrive_*.csv files");
        } else {
            int start = page * ROWS_PER_PAGE;
            for (int i = 0; i < ROWS_PER_PAGE && start + i < n; i++) {
                int row_y = 18 + i * 11;
                WdFile &f = wi_files[start + i];
                bool highlighted = (start + i) == sel;
                if (highlighted) {
                    gfx->fillRect(0, row_y - 1, 240, 11, 0x2104);
                }
                gfx->setTextColor(highlighted ? 0xFFFF : 0x07E0);
                gfx->setCursor(4, row_y);
                gfx->print(f.name);
                gfx->setTextColor(highlighted ? 0xFFFF : 0xC618);
                gfx->setCursor(112, row_y);
                gfx->printf("%6lu B  %lu rows",
                            (unsigned long)f.size_bytes,
                            (unsigned long)(f.lines > 0 ? f.lines - 1 : 0));
            }
        }

        // Wait for input
        char k = 0;
        while (k == 0) {
            k = get_keypress();
            if (k == 0) { delay(50); yield(); }
        }
        if (pm_is_exit_key(k)) return;
        if (k == PM_KEY_UP   || k == 'k' || k == 'K') {
            if (sel > 0) { sel--; page = sel / ROWS_PER_PAGE; }
        } else if (k == PM_KEY_DOWN || k == 'j' || k == 'J') {
            if (sel < n - 1) { sel++; page = sel / ROWS_PER_PAGE; }
        } else if (k == 'r' || k == 'R') {
            // Manual rescan
            sd_in_use = true;
            n = scan_wardrive_files();
            sd_in_use = false;
            sort_files_desc();
            if (sel >= n) sel = (n > 0) ? n - 1 : 0;
            page = sel / ROWS_PER_PAGE;
        }
    }
}
#endif // DEVICE_CARDPUTER_ADV

// ── T-Deck Plus + T-LoRa Pager ────────────────────────────
void run_wardrive_inspect() {
#ifdef DEVICE_CARDPUTER_ADV
    run_wardrive_inspect_cp();
    return;
#else
    const int W = gfx->width();
    const int H = gfx->height();
    int sel = 0;
    int page = 0;
    const int ROWS_PER_PAGE = 9;

    // Initial scan
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, W, 24, 0x18C3);
    gfx->setTextSize(1); gfx->setTextColor(0x07E0);
    gfx->setCursor(10, 7); gfx->print("GHOST RIDE THE WHIP");
    gfx->setTextColor(0xFFE0);
    gfx->setCursor(10, 100); gfx->print("Scanning SD card...");

    sd_in_use = true;
    int n = scan_wardrive_files();
    sd_in_use = false;
    sort_files_desc();

    while (true) {
        gfx->fillScreen(0x0000);
        // Header
        gfx->fillRect(0, 0, W, 24, 0x18C3);
        gfx->setTextSize(1); gfx->setTextColor(0x07E0);
        gfx->setCursor(10, 7);
#ifdef DEVICE_TLORAPAGER
        gfx->printf("GHOST RIDE THE WHIP (%d files)   M REFRESH   Q EXIT", n);
#else
        gfx->printf("GHOST RIDE THE WHIP (%d files) | TAP HEADER EXIT", n);
#endif

        if (!g_sd_ready) {
            gfx->setTextColor(0xF800); gfx->setTextSize(2);
            gfx->setCursor(10, 100); gfx->print("SD not ready");
            gfx->setTextSize(1); gfx->setTextColor(0xC618);
            gfx->setCursor(10, 130); gfx->print("Wait for SD to mount, then press R to rescan.");
        } else if (n == 0) {
            gfx->setTextColor(0xC618); gfx->setTextSize(2);
            gfx->setCursor(10, 100); gfx->print("No /wardrive_*.csv files found");
            gfx->setTextSize(1);
            gfx->setCursor(10, 130);
            gfx->print("Open the wardrive app to start a session.");
        } else {
            // Column headers
            gfx->setTextColor(0x07E0);
            gfx->setCursor(10,  32); gfx->print("FILE");
            gfx->setCursor(W - 220, 32); gfx->print("SIZE");
            gfx->setCursor(W - 110, 32); gfx->print("DATA ROWS");

            int start = page * ROWS_PER_PAGE;
            for (int i = 0; i < ROWS_PER_PAGE && start + i < n; i++) {
                int row_y = 50 + i * 18;
                WdFile &f = wi_files[start + i];
                bool highlighted = (start + i) == sel;
                if (highlighted) gfx->fillRect(0, row_y - 2, W, 18, 0x2104);
                gfx->setTextColor(highlighted ? 0xFFFF : 0x07E0);
                gfx->setCursor(10, row_y); gfx->print(f.name);
                gfx->setTextColor(highlighted ? 0xFFFF : 0xC618);
                gfx->setCursor(W - 220, row_y);
                gfx->printf("%lu", (unsigned long)f.size_bytes);
                gfx->setCursor(W - 110, row_y);
                // f.lines includes the header; data rows = lines - 1.
                long data_rows = (long)f.lines - 1;
                if (data_rows < 0) data_rows = 0;
                gfx->printf("%ld", data_rows);
            }

            // Footer hint
            gfx->setTextColor(0x4208);
            gfx->setCursor(10, H - 18);
#ifdef DEVICE_TLORAPAGER
            gfx->print("UP/DN: select   R: rescan   N: next page   P: prev page");
#else
            gfx->print("UP/DN select | R rescan | N/P page | Q exit");
#endif
        }

        // Input
        char k = 0;
        while (k == 0) {
            k = get_keypress();
            // Header-tap exit on T-Deck
            int16_t tx, ty;
            if (get_touch(&tx, &ty)) {
                if (ty < 30) {
                    while (get_touch(&tx, &ty)) { delay(10); yield(); }
                    return;
                }
            }
            TrackballState tb = update_trackball();
            if (tb.y == -1) k = PM_KEY_UP;
            else if (tb.y == 1) k = PM_KEY_DOWN;
            if (k == 0) { delay(30); yield(); }
        }

        if (pm_is_exit_key(k)) return;
        if (k == PM_KEY_UP   || k == 'k' || k == 'K') {
            if (sel > 0) { sel--; page = sel / ROWS_PER_PAGE; }
        } else if (k == PM_KEY_DOWN || k == 'j' || k == 'J') {
            if (sel < n - 1) { sel++; page = sel / ROWS_PER_PAGE; }
        } else if (k == 'n' || k == 'N') {
            if ((page + 1) * ROWS_PER_PAGE < n) {
                page++; sel = page * ROWS_PER_PAGE;
            }
        } else if (k == 'p' || k == 'P') {
            if (page > 0) { page--; sel = page * ROWS_PER_PAGE; }
        } else if (k == 'r' || k == 'R'
#ifdef DEVICE_TLORAPAGER
                   || k == 'm'
#endif
                  ) {
            sd_in_use = true;
            n = scan_wardrive_files();
            sd_in_use = false;
            sort_files_desc();
            if (sel >= n) sel = (n > 0) ? n - 1 : 0;
            page = sel / ROWS_PER_PAGE;
        }
    }
#endif // DEVICE_CARDPUTER_ADV
}
