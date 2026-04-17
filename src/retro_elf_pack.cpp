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

// ============================================================
//  retro_elf_pack.cpp — Pisces Moon OS Retro ELF Pack
//  "ELF ON A SHELF" — ROM Browser + Emulator Launcher v1.0
// ============================================================
//
//  Implements run_retro_pack():
//    - Scans /roms/nes/, /roms/gb/, /roms/atari/ on SD card
//    - Presents unified ROM list with color-coded system badges
//    - Routes selected ROM to correct emulator ELF via elf_execute()
//    - Graceful error screen if ELF is missing from /apps/
//
//  Include order follows The FS.h Mandate:
//    #include <FS.h> BEFORE #include "SdFat.h"
//
// ============================================================

#include <Arduino.h>
#include <FS.h>
#include "SdFat.h"
#include <Arduino_GFX_Library.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "elf_loader.h"
#include "gamepad.h"
#include "retro_elf_pack.h"

extern Arduino_GFX* gfx;
extern SdFat        sd;

// ============================================================
//  SYSTEM METADATA TABLE — one entry per RetroSystem
// ============================================================
const RetroSystemInfo RETRO_SYSTEMS[SYS_COUNT] = {
    // SYS_NES
    {
        "/roms/nes",
        "/apps/nofrendo.elf",
        "NES",
        { "nes", nullptr, nullptr },
        0xF800,   // Red
        2048
    },
    // SYS_GB
    {
        "/roms/gb",
        "/apps/gnuboy.elf",
        "GB",
        { "gb", "gbc", nullptr },
        0x07E0,   // Green
        1536
    },
    // SYS_ATARI
    {
        "/roms/atari",
        "/apps/stella.elf",
        "ATR",
        { "a26", "bin", nullptr },
        0xFFE0,   // Yellow
        1024
    },
};

// ============================================================
//  LOCAL PALETTE
// ============================================================
#define RB_BG       0x0000
#define RB_HDR      0x0010
#define RB_TRACE    0x0340
#define RB_SEL_BG   0x0821
#define RB_DIM      0x2104
#define RB_WHITE    0xFFFF
#define RB_GREY     0x7BEF

// ============================================================
//  ROM LIST — static allocation
// ============================================================
static RomEntry  _roms[ROM_LIST_MAX];
static int       _rom_count = 0;

// ============================================================
//  LAYOUT CONSTANTS
// ============================================================
#define RB_ROW_H   22
#define RB_ROW_Y0  28
#define RB_ROWS    8
#define RB_ROW_MAX (RB_ROW_Y0 + RB_ROWS * RB_ROW_H)

// ============================================================
//  HELPERS
// ============================================================

// Case-insensitive extension match
static bool ext_matches(const char* filename, const char* ext) {
    const char* dot = strrchr(filename, '.');
    if (!dot || !*(dot + 1)) return false;
    const char* fe = dot + 1;
    while (*fe && *ext) {
        if (tolower(*fe) != tolower(*ext)) return false;
        fe++; ext++;
    }
    return (*fe == '\0' && *ext == '\0');
}

// Determine system from filename extension
static int system_for_file(const char* filename) {
    for (int s = 0; s < SYS_COUNT; s++) {
        for (int e = 0; e < 3 && RETRO_SYSTEMS[s].exts[e]; e++) {
            if (ext_matches(filename, RETRO_SYSTEMS[s].exts[e])) return s;
        }
    }
    return -1;
}

// Strip extension from filename into name_buf
static void strip_ext(const char* filename, char* name_buf, int buf_size) {
    strncpy(name_buf, filename, buf_size - 1);
    name_buf[buf_size - 1] = '\0';
    char* dot = strrchr(name_buf, '.');
    if (dot) *dot = '\0';
}

// ============================================================
//  ROM SCANNER
//  Scans all three system directories and populates _roms[].
// ============================================================
static void scan_roms() {
    _rom_count = 0;

    for (int s = 0; s < SYS_COUNT && _rom_count < ROM_LIST_MAX; s++) {
        const char* dir_path = RETRO_SYSTEMS[s].dir;
        FsFile dir = sd.open(dir_path);
        if (!dir || !dir.isDir()) {
            Serial.printf("[RETRO] Directory not found: %s\n", dir_path);
            dir.close();
            continue;
        }

        FsFile entry;
        while (_rom_count < ROM_LIST_MAX && entry.openNext(&dir, O_READ)) {
            if (entry.isDir()) { entry.close(); continue; }

            char fname[64];
            entry.getName(fname, sizeof(fname));
            entry.close();

            int sys = system_for_file(fname);
            if (sys < 0) continue;  // Not a recognised ROM extension

            RomEntry& rom = _roms[_rom_count];
            strip_ext(fname, rom.name, ROM_NAME_MAX);
            snprintf(rom.path, ROM_PATH_MAX, "%s/%s", dir_path, fname);
            rom.system = (RetroSystem)sys;
            _rom_count++;
        }
        dir.close();
    }

    Serial.printf("[RETRO] Scan complete: %d ROMs found\n", _rom_count);
}

// ============================================================
//  DRAW HELPERS
// ============================================================

static void rb_draw_header(int cursor, int count) {
    gfx->fillRect(0, 0, 320, 24, RB_HDR);
    gfx->drawFastHLine(0, 23, 320, 0x07E0); // GAMES green accent

    // Title
    gfx->setTextSize(1);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(10, 8);
    gfx->print("RETRO PACK");

    // Gamepad status — right side
    gfx->setTextColor(g_gamepad.connected ? 0x07E0 : RB_DIM);
    gfx->setCursor(220, 8);
    gfx->print(g_gamepad.connected ? "CTRL OK" : "NO CTRL");

    // ROM count
    if (count > 0) {
        gfx->setTextColor(RB_DIM);
        gfx->setCursor(150, 8);
        char buf[12];
        snprintf(buf, sizeof(buf), "%d/%d", cursor + 1, count);
        gfx->print(buf);
    }
}

static void rb_draw_row(int slot, const RomEntry& rom, bool selected) {
    int y = RB_ROW_Y0 + slot * RB_ROW_H;

    uint16_t bg     = selected ? RB_SEL_BG : RB_BG;
    uint16_t txt    = selected ? RB_WHITE  : 0xC618;
    uint16_t badge_col = RETRO_SYSTEMS[rom.system].color;

    gfx->fillRect(0, y, 320, RB_ROW_H - 1, bg);

    if (selected) {
        gfx->drawFastHLine(0, y,              320, badge_col & 0x39E7);
        gfx->drawFastHLine(0, y + RB_ROW_H - 2, 320, badge_col & 0x39E7);
    }

    // System badge — 3-char label in system color, left-justified
    gfx->setTextSize(1);
    gfx->setTextColor(selected ? badge_col : (badge_col & 0x39E7));
    gfx->setCursor(4, y + 7);
    gfx->print(RETRO_SYSTEMS[rom.system].badge);

    // Separator
    gfx->drawFastVLine(26, y + 2, RB_ROW_H - 5, selected ? badge_col : RB_DIM);

    // ROM name — truncate to fit
    gfx->setTextColor(txt);
    gfx->setCursor(32, y + 7);
    char name_buf[40];
    snprintf(name_buf, sizeof(name_buf), "%.38s", rom.name);
    gfx->print(name_buf);
}

static void rb_draw_screen(int cursor, int scroll) {
    gfx->fillScreen(RB_BG);
    rb_draw_header(cursor, _rom_count);

    if (_rom_count == 0) {
        // Empty state — clear instructions
        gfx->setTextSize(1);
        gfx->setTextColor(RB_DIM);
        gfx->setCursor(20, 60);  gfx->print("No ROMs found on SD card.");
        gfx->setCursor(20, 78);  gfx->print("Create these directories:");
        gfx->setTextColor(0x07E0);
        gfx->setCursor(30, 96);  gfx->print("/roms/nes/   <- .nes files");
        gfx->setCursor(30, 112); gfx->print("/roms/gb/    <- .gb .gbc files");
        gfx->setCursor(30, 128); gfx->print("/roms/atari/ <- .a26 .bin files");
        gfx->setTextColor(RB_DIM);
        gfx->setCursor(20, 152); gfx->print("Also needed in /apps/:");
        gfx->setTextColor(0xFFE0);
        gfx->setCursor(30, 168); gfx->print("nofrendo.elf  gnuboy.elf  stella.elf");
        gfx->setTextColor(RB_DIM);
        gfx->setCursor(20, 192); gfx->print("See PISCES_MOON_MANUAL_ADDENDUM_ELF.md");

        gfx->fillRect(0, 210, 320, 30, RB_HDR);
        gfx->drawFastHLine(0, 210, 320, RB_TRACE);
        gfx->setTextColor(RB_DIM);
        gfx->setCursor(72, 220);
        gfx->print("B / header tap to exit");
        return;
    }

    // ROM rows
    for (int slot = 0; slot < RB_ROWS; slot++) {
        int idx = scroll + slot;
        if (idx >= _rom_count) {
            gfx->fillRect(0, RB_ROW_Y0 + slot * RB_ROW_H, 320, RB_ROW_H, RB_BG);
            continue;
        }
        rb_draw_row(slot, _roms[idx], (idx == cursor));
    }

    // Scrollbar
    if (_rom_count > RB_ROWS) {
        int range  = RB_ROW_MAX - RB_ROW_Y0;
        int bar_h  = max(8, (RB_ROWS * range) / _rom_count);
        int bar_y  = RB_ROW_Y0 + (scroll * (range - bar_h)) /
                     max(1, _rom_count - RB_ROWS);
        gfx->drawFastVLine(318, RB_ROW_Y0, range, RB_DIM);
        gfx->fillRect(317, bar_y, 3, bar_h, 0x07E0);
    }

    // Footer
    gfx->fillRect(0, 210, 320, 30, RB_HDR);
    gfx->drawFastHLine(0, 210, 320, RB_TRACE);
    gfx->setTextSize(1);
    gfx->setTextColor(RB_DIM);
    gfx->setCursor(10, 220);
    gfx->print("CLK/ENTER launch  W/S scroll  B exit");
}

// ============================================================
//  LAUNCH A ROM
// ============================================================
static void rb_launch(int cursor) {
    const RomEntry& rom = _roms[cursor];
    const RetroSystemInfo& sys = RETRO_SYSTEMS[rom.system];

    // Flash row
    int slot = cursor % RB_ROWS; // approximate — good enough for flash
    uint16_t badge_col = sys.color;
    gfx->drawFastHLine(0, RB_ROW_Y0 + slot * RB_ROW_H, 320, badge_col);
    delay(80);

    // Loading screen
    gfx->fillScreen(RB_BG);
    gfx->fillRect(0, 0, 320, 24, RB_HDR);
    gfx->drawFastHLine(0, 23, 320, badge_col);
    gfx->setTextColor(badge_col);
    gfx->setTextSize(1);
    gfx->setCursor(10, 8);
    gfx->print("LOADING");

    // System badge — large
    gfx->setTextSize(3);
    gfx->setTextColor(badge_col & 0x39E7);
    gfx->setCursor(10, 40);
    gfx->print(sys.badge);

    // ROM name
    gfx->setTextSize(2);
    gfx->setTextColor(RB_WHITE);
    char name_buf[18];
    snprintf(name_buf, sizeof(name_buf), "%.17s", rom.name);
    gfx->setCursor(10, 80);
    gfx->print(name_buf);

    // ELF path info
    gfx->setTextSize(1);
    gfx->setTextColor(RB_DIM);
    gfx->setCursor(10, 112);
    gfx->print(sys.elf);

    // PSRAM check
    size_t free_kb = ESP.getFreePsram() / 1024;
    gfx->setCursor(10, 128);
    if (free_kb >= sys.psram_kb) {
        gfx->setTextColor(0x07E0);
        gfx->printf("PSRAM: %uKB free / %uKB needed  OK", (unsigned)free_kb, (unsigned)sys.psram_kb);
    } else {
        gfx->setTextColor(0xF800);
        gfx->printf("PSRAM: %uKB free / %uKB needed  LOW", (unsigned)free_kb, (unsigned)sys.psram_kb);
    }

    gfx->setTextColor(0x07E0);
    gfx->setCursor(10, 152);
    gfx->print("Starting emulator...");
    delay(60);

    // Build context with ROM path
    ElfContext ctx;
    elf_build_context(&ctx, rom.path);

    // Execute emulator ELF
    ElfLoadResult result = elf_execute(sys.elf, &ctx);

    if (result != ELF_OK) {
        // Error screen
        gfx->fillScreen(RB_BG);
        gfx->fillRect(0, 0, 320, 24, RB_HDR);
        gfx->drawFastHLine(0, 23, 320, 0xF800);
        gfx->setTextColor(0xF800);
        gfx->setTextSize(1);
        gfx->setCursor(10, 8);
        gfx->print("EMULATOR ERROR");

        gfx->setTextSize(2);
        gfx->setTextColor(0xF800);
        gfx->setCursor(8, 36);
        gfx->print("LOAD FAILED");

        gfx->setTextSize(1);
        gfx->setTextColor(RB_WHITE);
        gfx->setCursor(8, 64);
        gfx->printf("%.38s", rom.name);

        gfx->setTextColor(0xF800);
        gfx->setCursor(8, 82);
        gfx->printf("Error: %s", elf_result_str(result));

        gfx->setTextColor(RB_GREY);
        gfx->setCursor(8, 100);
        if (result == ELF_NOT_FOUND) {
            gfx->print("Emulator ELF not found in /apps/");
            gfx->setCursor(8, 116);
            gfx->printf("Copy %s to your SD card.", sys.elf + 6); // skip "/apps/"
            gfx->setCursor(8, 132);
            gfx->print("See PISCES_MOON_MANUAL_ADDENDUM_ELF.md");
        } else if (result == ELF_NO_PSRAM) {
            gfx->printf("Need %uKB PSRAM, have %uKB.", (unsigned)sys.psram_kb, (unsigned)free_kb);
        } else if (result == ELF_BAD_MAGIC) {
            gfx->print("Not a valid ESP32-S3 ELF binary.");
            gfx->setCursor(8, 116);
            gfx->print("Compile with Xtensa toolchain + -fPIC.");
        } else {
            gfx->print("Check serial monitor for details.");
        }

        gfx->setTextColor(0x07E0);
        gfx->setCursor(8, 168);
        gfx->print("Any key / B to return to browser...");

        uint32_t t0 = millis();
        while (millis() - t0 < 8000) {
            if (get_keypress()) break;
            TrackballState tb = update_trackball();
            if (tb.clicked || tb.x == -1) break;
            gamepad_poll();
            if (gamepad_pressed(GP_A | GP_B | GP_START)) break;
            int16_t tx, ty;
            if (get_touch(&tx, &ty)) { while(get_touch(&tx,&ty)){delay(10);} break; }
            delay(30);
        }
    }
    // On success or after error ack, return to ROM browser (caller redraws)
}

// ============================================================
//  run_retro_pack() — MAIN ENTRY POINT
// ============================================================
void run_retro_pack() {
    scan_roms();

    int cursor = 0;
    int scroll = 0;

    rb_draw_screen(cursor, scroll);

    while (true) {
        // ── Trackball ──
        TrackballState tb = update_trackball();

        if (tb.y == -1 && _rom_count > 0) {
            if (cursor > 0) {
                cursor--;
                if (cursor < scroll) scroll = cursor;
                rb_draw_screen(cursor, scroll);
            }
        } else if (tb.y == 1 && _rom_count > 0) {
            if (cursor < _rom_count - 1) {
                cursor++;
                if (cursor >= scroll + RB_ROWS) scroll = cursor - RB_ROWS + 1;
                rb_draw_screen(cursor, scroll);
            }
        } else if (tb.clicked && _rom_count > 0) {
            rb_launch(cursor);
            rb_draw_screen(cursor, scroll);
        } else if (tb.x == -1) {
            return;
        }

        // ── Keyboard ──
        char k = get_keypress();
        if (k == 'q' || k == 'Q' || k == 'b' || k == 'B') return;
        if ((k == '\n' || k == '\r' || k == ' ') && _rom_count > 0) {
            rb_launch(cursor);
            rb_draw_screen(cursor, scroll);
        }
        if ((k == 'w' || k == 'W') && cursor > 0) {
            cursor--;
            if (cursor < scroll) scroll = cursor;
            rb_draw_screen(cursor, scroll);
        }
        if ((k == 's' || k == 'S') && cursor < _rom_count - 1) {
            cursor++;
            if (cursor >= scroll + RB_ROWS) scroll = cursor - RB_ROWS + 1;
            rb_draw_screen(cursor, scroll);
        }
        // R = re-scan (hot-swap SD)
        if (k == 'r' || k == 'R') {
            scan_roms();
            cursor = 0; scroll = 0;
            rb_draw_screen(cursor, scroll);
        }

        // ── Gamepad ──
        if (gamepad_poll()) return; // HOME → launcher
        if (gamepad_pressed(GP_UP) && _rom_count > 0) {
            if (cursor > 0) {
                cursor--;
                if (cursor < scroll) scroll = cursor;
                rb_draw_screen(cursor, scroll);
            }
        }
        if (gamepad_pressed(GP_DOWN) && _rom_count > 0) {
            if (cursor < _rom_count - 1) {
                cursor++;
                if (cursor >= scroll + RB_ROWS) scroll = cursor - RB_ROWS + 1;
                rb_draw_screen(cursor, scroll);
            }
        }
        if (gamepad_pressed(GP_A) && _rom_count > 0) {
            rb_launch(cursor);
            rb_draw_screen(cursor, scroll);
        }
        if (gamepad_pressed(GP_B)) return;

        // ── Touch ──
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            while (get_touch(&tx, &ty)) { delay(10); }
            if (ty < 24) return; // header tap = exit
            if (ty >= RB_ROW_Y0 && ty < RB_ROW_MAX && _rom_count > 0) {
                int slot = (ty - RB_ROW_Y0) / RB_ROW_H;
                int tgt  = scroll + slot;
                if (tgt < _rom_count) {
                    if (tgt == cursor) {
                        rb_launch(cursor);
                        rb_draw_screen(cursor, scroll);
                    } else {
                        cursor = tgt;
                        rb_draw_screen(cursor, scroll);
                    }
                }
            }
        }

        delay(20);
    }
}