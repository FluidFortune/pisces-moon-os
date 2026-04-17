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
//  apps_elf.cpp — Pisces Moon OS v0.9.6 "ELF On A Shelf"
//
//  Implements two SYSTEM-category apps declared in apps.h:
//
//    run_elf_browser()    — Generic ELF app loader
//                           Scans /apps/ on SD, shows scrollable
//                           manifest list, launches selected ELF.
//
//    run_gamepad_setup()  — 8BitDo Zero 2 BLE pairing screen
//                           Shows live connection status, pairing
//                           instructions, and real-time button test.
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
#include "apps.h"

extern Arduino_GFX *gfx;
extern SdFat        sd;
extern bool         exitApp;

// ─────────────────────────────────────────────
//  SHARED LOCAL PALETTE
//  Extends theme.h without polluting it.
// ─────────────────────────────────────────────
#define EA_BG       0x0000   // Black
#define EA_HDR      0x0010   // Deep blue-black header (matches launcher)
#define EA_TRACE    0x0340   // PCB trace green
#define EA_MATRIX   0x07E0   // Bright matrix green
#define EA_AMBER    0xFD20   // Amber accent (SYSTEM folder color 0x8410 variant)
#define EA_SILVER   0x8410   // SYSTEM folder accent (matches CAT_ACCENT[6])
#define EA_DIM      0x2104   // Very dim text
#define EA_SEL_BG   0x0821   // Selected row background (near-black blue tint)
#define EA_WHITE    0xFFFF
#define EA_RED      0xF800
#define EA_CYAN     0x07FF
#define EA_GREY     0x7BEF

// ─────────────────────────────────────────────
//  SHARED DRAW HELPERS
// ─────────────────────────────────────────────

static void ea_draw_header(const char* title, uint16_t accent) {
    gfx->fillRect(0, 0, 320, 24, EA_HDR);
    gfx->drawFastHLine(0, 22, 320, EA_TRACE);
    gfx->drawFastHLine(0, 23, 320, accent);

    // Small chip accent left — matches launcher aesthetic
    gfx->fillRect(4, 4, 10, 16, accent & 0x2104);
    gfx->drawRect(4, 4, 10, 16, accent);
    gfx->drawFastHLine(2, 8,  2, accent);
    gfx->drawFastHLine(2, 12, 2, accent);
    gfx->drawFastHLine(2, 16, 2, accent);
    gfx->drawFastHLine(14, 8, 2, accent);
    gfx->drawFastHLine(14,12, 2, accent);
    gfx->drawFastHLine(14,16, 2, accent);

    gfx->setTextSize(1);
    gfx->setTextColor(accent);
    int tw = strlen(title) * 6;
    gfx->setCursor((320 - tw) / 2, 8);
    gfx->print(title);
}

static void ea_draw_footer(const char* msg, uint16_t accent) {
    gfx->fillRect(0, 210, 320, 30, EA_HDR);
    gfx->drawFastHLine(0, 210, 320, EA_TRACE);
    gfx->setTextSize(1);
    gfx->setTextColor(accent);
    int tw = strlen(msg) * 6;
    gfx->setCursor((320 - tw) / 2, 220);
    gfx->print(msg);
}

// Chamfered rectangle — consistent with launcher boxes
static void ea_chamfer(int x, int y, int w, int h,
                        uint16_t fill, uint16_t border, int r = 5) {
    gfx->fillRect(x + r, y,     w - 2*r, h,     fill);
    gfx->fillRect(x,     y + r, w,       h-2*r, fill);
    gfx->drawFastHLine(x + r, y,         w - 2*r, border);
    gfx->drawFastHLine(x + r, y + h - 1, w - 2*r, border);
    gfx->drawFastVLine(x,         y + r, h - 2*r, border);
    gfx->drawFastVLine(x + w - 1, y + r, h - 2*r, border);
    // Corner pixels
    gfx->drawPixel(x + r,         y + r,         border);
    gfx->drawPixel(x + w - r - 1, y + r,         border);
    gfx->drawPixel(x + r,         y + h - r - 1, border);
    gfx->drawPixel(x + w - r - 1, y + h - r - 1, border);
}

// ============================================================
//  ████████╗██╗  ██╗███████╗
//  ██╔════╝██║  ██║██╔════╝
//  █████╗  ███████║█████╗
//  ██╔══╝  ██╔══██║██╔══╝
//  ███████╗██║  ██║██║
//  ╚══════╝╚═╝  ╚═╝╚═╝
//
//  run_elf_browser() — ELF App Browser
//  SYSTEM → "ELF APPS"
// ============================================================

// ─────────────────────────────────────────────
//  LAYOUT CONSTANTS
// ─────────────────────────────────────────────
#define EB_ROW_H    28       // Height of each app row
#define EB_ROW_Y0   30       // Y of first row
#define EB_ROWS     6        // Visible rows
#define EB_ROW_MAX  (EB_ROW_Y0 + EB_ROWS * EB_ROW_H)

// ─────────────────────────────────────────────
//  DRAW ONE APP ROW
// ─────────────────────────────────────────────
static void eb_draw_row(int slot, const ElfManifest& m, bool selected,
                        bool launching = false) {
    int y = EB_ROW_Y0 + slot * EB_ROW_H;

    uint16_t bg  = selected  ? EA_SEL_BG : EA_BG;
    uint16_t acc = selected  ? EA_SILVER : EA_DIM;
    uint16_t txt = selected  ? EA_WHITE  : 0xC618;

    gfx->fillRect(0, y, 320, EB_ROW_H - 1, bg);
    if (selected) {
        gfx->drawFastHLine(0, y,                  320, acc);
        gfx->drawFastHLine(0, y + EB_ROW_H - 2,  320, acc);
    }

    // Left accent bar
    gfx->fillRect(0, y + 1, 3, EB_ROW_H - 3, selected ? EA_SILVER : EA_DIM);

    // App name (large-ish)
    gfx->setTextSize(1);
    gfx->setTextColor(launching ? EA_MATRIX : txt);
    gfx->setCursor(10, y + 4);
    // Truncate name to 22 chars to leave room for version
    char name_buf[24];
    snprintf(name_buf, sizeof(name_buf), "%.22s", m.name);
    gfx->print(name_buf);

    // Version — right-aligned, dimmer
    gfx->setTextColor(selected ? EA_AMBER : EA_DIM);
    if (m.version[0]) {
        char ver_buf[10];
        snprintf(ver_buf, sizeof(ver_buf), "v%.7s", m.version);
        int vw = strlen(ver_buf) * 6;
        gfx->setCursor(320 - vw - 6, y + 4);
        gfx->print(ver_buf);
    }

    // Second line — category + author + PSRAM footprint
    gfx->setTextColor(selected ? EA_CYAN : EA_DIM);
    gfx->setCursor(10, y + 15);
    char meta[48];
    if (m.author[0] && m.category[0])
        snprintf(meta, sizeof(meta), "%.12s  by %.14s", m.category, m.author);
    else if (m.category[0])
        snprintf(meta, sizeof(meta), "%.20s", m.category);
    else
        snprintf(meta, sizeof(meta), "no manifest");
    gfx->print(meta);

    // PSRAM badge — far right, second line
    if (m.psram_kb > 0) {
        gfx->setTextColor(selected ? EA_AMBER : EA_DIM);
        char psram_buf[12];
        snprintf(psram_buf, sizeof(psram_buf), "%uKB", m.psram_kb);
        int pw = strlen(psram_buf) * 6;
        gfx->setCursor(320 - pw - 6, y + 15);
        gfx->print(psram_buf);
    }

    // Launching animation — right flash
    if (launching) {
        gfx->drawFastHLine(0, y,                  320, EA_MATRIX);
        gfx->drawFastHLine(0, y + EB_ROW_H - 2,  320, EA_MATRIX);
        gfx->setTextColor(EA_MATRIX);
        gfx->setCursor(280, y + 4);
        gfx->print(">>");
    }
}

// ─────────────────────────────────────────────
//  DRAW FULL BROWSER SCREEN
// ─────────────────────────────────────────────
static void eb_draw_screen(const ElfManifest* apps, int count,
                            int cursor, int scroll) {
    gfx->fillScreen(EA_BG);
    ea_draw_header("ELF APPS", EA_SILVER);

    if (count == 0) {
        // Empty state
        gfx->setTextColor(EA_DIM);
        gfx->setTextSize(1);
        gfx->setCursor(30, 80);
        gfx->print("No .elf files found in /apps/");
        gfx->setCursor(30, 100);
        gfx->print("Copy .elf modules to SD:/apps/");
        gfx->setCursor(30, 120);
        gfx->print("Optional: add .json sidecar");
        gfx->setCursor(30, 140);
        gfx->print("See PISCES_MOON_MANUAL_ADDENDUM_ELF.md");
        ea_draw_footer("B / header tap to exit", EA_SILVER);
        return;
    }

    // Row separator under header
    gfx->drawFastHLine(0, EB_ROW_Y0 - 1, 320, EA_TRACE & 0x3186);

    // App rows
    for (int slot = 0; slot < EB_ROWS; slot++) {
        int idx = scroll + slot;
        if (idx >= count) {
            // Clear empty slot
            gfx->fillRect(0, EB_ROW_Y0 + slot * EB_ROW_H, 320, EB_ROW_H, EA_BG);
            continue;
        }
        eb_draw_row(slot, apps[idx], (idx == cursor));
    }

    // Scrollbar
    if (count > EB_ROWS) {
        int bar_h  = max(8, (EB_ROWS * (EB_ROW_MAX - EB_ROW_Y0)) / count);
        int bar_y  = EB_ROW_Y0 + (scroll * (EB_ROW_MAX - EB_ROW_Y0 - bar_h)) /
                     max(1, count - EB_ROWS);
        gfx->drawFastVLine(318, EB_ROW_Y0, EB_ROW_MAX - EB_ROW_Y0, EA_DIM);
        gfx->fillRect(317, bar_y, 3, bar_h, EA_SILVER);
    }

    // Footer hint
    char footer[48];
    if (count > 1)
        snprintf(footer, sizeof(footer), "%d/%d  CLK/ENTER launch  B exit", cursor+1, count);
    else
        snprintf(footer, sizeof(footer), "1/1  CLK/ENTER launch  B exit");
    ea_draw_footer(footer, EA_SILVER);
}

// ─────────────────────────────────────────────
//  LAUNCH WITH SPLASH TRANSITION
// ─────────────────────────────────────────────
static void eb_launch(ElfManifest* apps, int count, int cursor, int scroll) {
    // Flash the selected row green
    int slot = cursor - scroll;
    eb_draw_row(slot, apps[cursor], true, /*launching=*/true);
    delay(120);

    // Loading splash
    gfx->fillScreen(EA_BG);
    ea_draw_header("LAUNCHING", EA_MATRIX);

    gfx->setTextSize(2);
    gfx->setTextColor(EA_MATRIX);
    // Truncate to keep it on screen
    char name_buf[18];
    snprintf(name_buf, sizeof(name_buf), "%.17s", apps[cursor].name);
    int tw = strlen(name_buf) * 12;
    gfx->setCursor((320 - tw) / 2, 50);
    gfx->print(name_buf);

    gfx->setTextSize(1);
    gfx->setTextColor(EA_AMBER);
    if (apps[cursor].version[0]) {
        char ver_buf[20];
        snprintf(ver_buf, sizeof(ver_buf), "version %s", apps[cursor].version);
        int vw = strlen(ver_buf) * 6;
        gfx->setCursor((320 - vw) / 2, 76);
        gfx->print(ver_buf);
    }

    if (apps[cursor].author[0]) {
        gfx->setTextColor(EA_DIM);
        char auth_buf[32];
        snprintf(auth_buf, sizeof(auth_buf), "by %s", apps[cursor].author);
        int aw = strlen(auth_buf) * 6;
        gfx->setCursor((320 - aw) / 2, 92);
        gfx->print(auth_buf);
    }

    // PSRAM check
    size_t free_psram = ESP.getFreePsram();
    size_t free_kb    = free_psram / 1024;
    if (apps[cursor].psram_kb > 0) {
        gfx->setTextColor(free_kb >= apps[cursor].psram_kb ? EA_MATRIX : EA_RED);
        gfx->setCursor(8, 118);
        gfx->printf("PSRAM: %uKB free / %uKB needed",
                    (unsigned)free_kb, (unsigned)apps[cursor].psram_kb);
    } else {
        gfx->setTextColor(EA_DIM);
        gfx->setCursor(8, 118);
        gfx->printf("PSRAM: %uKB free", (unsigned)free_kb);
    }

    // ELF path
    char elf_path[96];
    snprintf(elf_path, sizeof(elf_path), "/apps/%s", apps[cursor].elf_file);
    gfx->setTextColor(EA_DIM);
    gfx->setCursor(8, 134);
    gfx->printf("%.42s", elf_path);

    gfx->setTextColor(EA_MATRIX);
    gfx->setCursor(8, 158);
    gfx->print("Loading from SD...");

    // Animated dots while we proceed
    delay(80);

    // Execute
    ElfContext ctx;
    elf_build_context(&ctx, nullptr);
    ElfLoadResult result = elf_execute_manifest(&apps[cursor], &ctx);

    if (result != ELF_OK) {
        // Error screen
        gfx->fillScreen(EA_BG);
        ea_draw_header("ELF ERROR", EA_RED);

        gfx->setTextSize(2);
        gfx->setTextColor(EA_RED);
        gfx->setCursor(8, 36);
        gfx->print("LOAD FAILED");

        gfx->setTextSize(1);
        gfx->setTextColor(EA_WHITE);
        gfx->setCursor(8, 62);
        snprintf(name_buf, sizeof(name_buf), "%.17s", apps[cursor].name);
        gfx->print(name_buf);

        gfx->setTextColor(EA_RED);
        gfx->setCursor(8, 80);
        gfx->printf("Error: %s", elf_result_str(result));

        gfx->setTextColor(EA_GREY);
        gfx->setCursor(8, 98);
        switch (result) {
            case ELF_NO_PSRAM:
                gfx->printf("Need %uKB, have %uKB free",
                            (unsigned)apps[cursor].psram_kb, (unsigned)free_kb);
                gfx->setCursor(8, 114);
                gfx->print("Exit other apps and try again.");
                break;
            case ELF_BAD_MAGIC:
                gfx->print("Not a valid ESP32-S3 ELF.");
                gfx->setCursor(8, 114);
                gfx->print("Compile with Xtensa + -fPIC.");
                break;
            case ELF_ABI_MISMATCH:
                gfx->printf("Module needs ELF API v%d. OS has v%d.",
                            (int)apps[cursor].api_major, ELF_API_MAJOR_SUPPORTED);
                break;
            case ELF_NOT_FOUND:
                gfx->print("File missing from /apps/ on SD.");
                break;
            case ELF_SD_ERROR:
                gfx->print("SD read failed. Check card seating.");
                break;
            case ELF_EXEC_ERROR:
                gfx->print("Module returned non-zero exit.");
                gfx->setCursor(8, 114);
                gfx->print("Check serial monitor for details.");
                break;
            default:
                gfx->print("Check serial monitor for details.");
                break;
        }

        // Hint — file path
        gfx->setTextColor(EA_DIM);
        gfx->setCursor(8, 140);
        gfx->printf("%.42s", elf_path);

        gfx->setTextColor(EA_MATRIX);
        gfx->setCursor(8, 164);
        gfx->print("Any key / B button to return...");

        uint32_t t0 = millis();
        while (millis() - t0 < 8000) {
            if (get_keypress()) break;
            TrackballState tb = update_trackball();
            if (tb.clicked) break;
            gamepad_poll();
            if (gamepad_pressed(GP_A | GP_B | GP_START)) break;
            int16_t tx, ty;
            if (get_touch(&tx, &ty)) { while(get_touch(&tx,&ty)){delay(10);} break; }
            delay(30);
        }
    }
    // On success or after error screen, fall back through to eb_draw_screen
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_elf_browser() {
    // ── Scan /apps/ for ELF manifests ──
    static ElfManifest apps[24];   // Static: lives in heap, not stack
    int count = elf_scan_apps(apps, 24);

    int cursor = 0;
    int scroll = 0;

    eb_draw_screen(apps, count, cursor, scroll);

    while (true) {
        // ── Trackball ──
        TrackballState tb = update_trackball();

        if (tb.y == -1 && count > 0) {
            // UP
            if (cursor > 0) {
                int old_slot = cursor - scroll;
                cursor--;
                if (cursor < scroll) scroll = cursor;
                int new_slot = cursor - scroll;
                // Fast redraw — only update the two changed rows
                eb_draw_row(old_slot, apps[cursor + 1], false);
                eb_draw_row(new_slot, apps[cursor],     true);
                // Refresh footer counter
                char footer[48];
                snprintf(footer, sizeof(footer), "%d/%d  CLK/ENTER launch  B exit",
                         cursor+1, count);
                ea_draw_footer(footer, EA_SILVER);
            }
        } else if (tb.y == 1 && count > 0) {
            // DOWN
            if (cursor < count - 1) {
                int old_slot = cursor - scroll;
                cursor++;
                if (cursor >= scroll + EB_ROWS) scroll = cursor - EB_ROWS + 1;
                int new_slot = cursor - scroll;
                if (old_slot >= 0 && old_slot < EB_ROWS)
                    eb_draw_row(old_slot, apps[cursor - 1], false);
                if (new_slot >= 0 && new_slot < EB_ROWS)
                    eb_draw_row(new_slot, apps[cursor],     true);
                // Redraw scrollbar
                if (count > EB_ROWS) {
                    int bar_h = max(8, (EB_ROWS * (EB_ROW_MAX - EB_ROW_Y0)) / count);
                    int bar_y = EB_ROW_Y0 + (scroll * (EB_ROW_MAX - EB_ROW_Y0 - bar_h)) /
                                max(1, count - EB_ROWS);
                    gfx->fillRect(316, EB_ROW_Y0, 4, EB_ROW_MAX - EB_ROW_Y0, EA_BG);
                    gfx->drawFastVLine(318, EB_ROW_Y0, EB_ROW_MAX - EB_ROW_Y0, EA_DIM);
                    gfx->fillRect(317, bar_y, 3, bar_h, EA_SILVER);
                }
                char footer[48];
                snprintf(footer, sizeof(footer), "%d/%d  CLK/ENTER launch  B exit",
                         cursor+1, count);
                ea_draw_footer(footer, EA_SILVER);
            }
        } else if (tb.clicked && count > 0) {
            eb_launch(apps, count, cursor, scroll);
            // Re-scan in case the ELF modified the SD (e.g. installed another module)
            count = elf_scan_apps(apps, 24);
            if (cursor >= count) cursor = max(0, count - 1);
            if (scroll > cursor) scroll = cursor;
            eb_draw_screen(apps, count, cursor, scroll);
        } else if (tb.x == -1) {
            return; // Trackball left = exit to launcher
        }

        // ── Keyboard ──
        char k = get_keypress();
        if (k == 'b' || k == 'B' || k == 'q' || k == 'Q') return;
        if ((k == '\n' || k == '\r' || k == ' ') && count > 0) {
            eb_launch(apps, count, cursor, scroll);
            count = elf_scan_apps(apps, 24);
            if (cursor >= count) cursor = max(0, count - 1);
            if (scroll > cursor) scroll = cursor;
            eb_draw_screen(apps, count, cursor, scroll);
        }
        // Number shortcut: 1-6 = jump to row on current page
        if (k >= '1' && k <= '6') {
            int tgt = scroll + (k - '1');
            if (tgt < count) {
                cursor = tgt;
                eb_draw_screen(apps, count, cursor, scroll);
            }
        }
        // R = re-scan (user may have swapped SD)
        if (k == 'r' || k == 'R') {
            count  = elf_scan_apps(apps, 24);
            cursor = 0; scroll = 0;
            eb_draw_screen(apps, count, cursor, scroll);
        }

        // ── Gamepad ──
        if (gamepad_poll()) return; // HOME → launcher
        if (gamepad_pressed(GP_UP)   && count > 0) {
            if (cursor > 0) { cursor--; if (cursor < scroll) scroll = cursor; }
            eb_draw_screen(apps, count, cursor, scroll);
        }
        if (gamepad_pressed(GP_DOWN) && count > 0) {
            if (cursor < count - 1) {
                cursor++;
                if (cursor >= scroll + EB_ROWS) scroll = cursor - EB_ROWS + 1;
            }
            eb_draw_screen(apps, count, cursor, scroll);
        }
        if (gamepad_pressed(GP_A) && count > 0) {
            eb_launch(apps, count, cursor, scroll);
            count = elf_scan_apps(apps, 24);
            if (cursor >= count) cursor = max(0, count - 1);
            if (scroll > cursor) scroll = cursor;
            eb_draw_screen(apps, count, cursor, scroll);
        }
        if (gamepad_pressed(GP_B)) return;

        // ── Touch ──
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            while (get_touch(&tx, &ty)) { delay(10); }

            if (ty < 24) return; // Header tap = exit

            if (ty >= EB_ROW_Y0 && ty < EB_ROW_MAX) {
                int slot = (ty - EB_ROW_Y0) / EB_ROW_H;
                int tgt  = scroll + slot;
                if (tgt < count) {
                    if (tgt == cursor) {
                        // Second tap on same item = launch
                        eb_launch(apps, count, cursor, scroll);
                        count = elf_scan_apps(apps, 24);
                        if (cursor >= count) cursor = max(0, count - 1);
                        if (scroll > cursor) scroll = cursor;
                        eb_draw_screen(apps, count, cursor, scroll);
                    } else {
                        // First tap = select
                        cursor = tgt;
                        eb_draw_screen(apps, count, cursor, scroll);
                    }
                }
            }
        }

        delay(20);
    }
}


// ============================================================
//   ██████╗  █████╗ ███╗   ███╗███████╗██████╗  █████╗ ██████╗
//  ██╔════╝ ██╔══██╗████╗ ████║██╔════╝██╔══██╗██╔══██╗██╔══██╗
//  ██║  ███╗███████║██╔████╔██║█████╗  ██████╔╝███████║██║  ██║
//  ██║   ██║██╔══██║██║╚██╔╝██║██╔══╝  ██╔═══╝ ██╔══██║██║  ██║
//  ╚██████╔╝██║  ██║██║ ╚═╝ ██║███████╗██║     ██║  ██║██████╔╝
//   ╚═════╝ ╚═╝  ╚═╝╚═╝     ╚═╝╚══════╝╚═╝     ╚═╝  ╚═╝╚═════╝
//
//  run_gamepad_setup() — 8BitDo Zero 2 BLE Pairing + Button Test
//  SYSTEM → "GAMEPAD"
// ============================================================

// ─────────────────────────────────────────────
//  LAYOUT
// ─────────────────────────────────────────────
//  y  0-23   : header
//  y 28-100  : connection status panel
//  y 108-175 : button test grid
//  y 182-209 : pairing instructions
//  y 210+    : footer

#define GP_PANEL_Y   28
#define GP_PANEL_H   72
#define GP_BTN_Y     108
#define GP_BTN_H     64
#define GP_INSTR_Y   182
#define GP_INSTR_H   26

// ─────────────────────────────────────────────
//  BUTTON TEST GRID
//  13 buttons laid out in 5 rows:
//
//    Row 0 (D-pad):  UP  DOWN  LEFT  RIGHT
//    Row 1 (action): A   B     X     Y
//    Row 2 (shoulder): L  R
//    Row 3 (meta):   START  SELECT
//    Row 4 (OS):     HOME
// ─────────────────────────────────────────────

struct BtnDef {
    const char* label;
    uint16_t    mask;
    int         gx, gy;   // Grid col (0-3), grid row (0-4)
};

static const BtnDef BTNS[] = {
    // D-pad
    { "UP",    GP_UP,     1, 0 },
    { "DN",    GP_DOWN,   2, 0 },
    { "LT",    GP_LEFT,   0, 0 },
    { "RT",    GP_RIGHT,  3, 0 },
    // Actions
    { "A",     GP_A,      3, 1 },
    { "B",     GP_B,      2, 1 },
    { "X",     GP_X,      1, 1 },
    { "Y",     GP_Y,      0, 1 },
    // Shoulders
    { "L",     GP_L,      0, 2 },
    { "R",     GP_R,      1, 2 },
    // Meta
    { "START", GP_START,  0, 3 },
    { "SEL",   GP_SELECT, 2, 3 },
    // OS reserved
    { "HOME",  GP_HOME,   1, 4 },
};
#define NUM_BTNS 13

// Column positions for 4-col grid within button panel
static const int BTN_COL_X[] = { 12, 88, 164, 240 };
#define BTN_W 66
#define BTN_H 12
#define BTN_ROW_H 14

// ─────────────────────────────────────────────
//  DRAW STATUS PANEL
// ─────────────────────────────────────────────
static void gp_draw_status(bool connected, const char* device_name,
                            uint32_t last_input_ms) {
    gfx->fillRect(0, GP_PANEL_Y, 320, GP_PANEL_H, EA_SEL_BG);
    gfx->drawFastHLine(0, GP_PANEL_Y,              320, EA_SILVER);
    gfx->drawFastHLine(0, GP_PANEL_Y + GP_PANEL_H, 320, EA_TRACE);

    uint16_t dot_col = connected ? EA_MATRIX : EA_RED;
    uint16_t txt_col = connected ? EA_WHITE  : EA_GREY;

    // Status dot + text
    gfx->fillCircle(22, GP_PANEL_Y + 14, 6, dot_col);
    gfx->drawCircle(22, GP_PANEL_Y + 14, 7, dot_col & 0x39E7);
    gfx->setTextSize(2);
    gfx->setTextColor(txt_col);
    gfx->setCursor(36, GP_PANEL_Y + 6);
    gfx->print(connected ? "CONNECTED" : "NOT CONNECTED");

    // Device name
    gfx->setTextSize(1);
    if (connected && device_name[0]) {
        gfx->setTextColor(EA_AMBER);
        gfx->setCursor(36, GP_PANEL_Y + 28);
        char dev_buf[34];
        snprintf(dev_buf, sizeof(dev_buf), "%.32s", device_name);
        gfx->print(dev_buf);
    } else {
        gfx->setTextColor(EA_DIM);
        gfx->setCursor(36, GP_PANEL_Y + 28);
        gfx->print("Searching for 8BitDo Zero 2...");
    }

    // Last input time
    if (connected && last_input_ms > 0) {
        uint32_t ago_sec = (millis() - last_input_ms) / 1000;
        gfx->setTextColor(EA_DIM);
        gfx->setCursor(36, GP_PANEL_Y + 42);
        if (ago_sec < 5)
            gfx->print("Active");
        else
            gfx->printf("Idle %lus", (unsigned long)ago_sec);
    }

    // PSRAM / heap info — useful for debugging BLE
    size_t free_heap = ESP.getFreeHeap();
    gfx->setTextColor(EA_DIM);
    gfx->setCursor(200, GP_PANEL_Y + 42);
    gfx->printf("HEAP:%uK", (unsigned)(free_heap / 1024));
}

// ─────────────────────────────────────────────
//  DRAW ONE BUTTON IN THE TEST GRID
// ─────────────────────────────────────────────
static void gp_draw_button(int idx, bool pressed) {
    const BtnDef& b = BTNS[idx];

    // Map gx/gy to pixel coordinates within the button panel
    int col_x = BTN_COL_X[b.gx];
    int row_y  = GP_BTN_Y + b.gy * BTN_ROW_H;

    uint16_t fill   = pressed ? EA_MATRIX   : EA_SEL_BG;
    uint16_t border = pressed ? EA_MATRIX   : EA_DIM;
    uint16_t label_col = pressed ? EA_BG    : EA_GREY;

    ea_chamfer(col_x, row_y, BTN_W, BTN_H, fill, border, 2);
    gfx->setTextSize(1);
    gfx->setTextColor(label_col);
    int lw = strlen(b.label) * 6;
    gfx->setCursor(col_x + (BTN_W - lw) / 2, row_y + 2);
    gfx->print(b.label);

    // HOME gets special OS marker
    if (b.mask == GP_HOME) {
        uint16_t os_col = pressed ? EA_BG : EA_AMBER;
        gfx->setTextColor(os_col);
        gfx->setCursor(col_x + BTN_W - 20, row_y + 2);
        gfx->print("OS");
    }
}

// ─────────────────────────────────────────────
//  DRAW ALL BUTTONS
// ─────────────────────────────────────────────
static void gp_draw_all_buttons(uint16_t buttons) {
    // Panel background
    gfx->fillRect(0, GP_BTN_Y, 320, GP_BTN_H + 4, EA_BG);
    gfx->drawFastHLine(0, GP_BTN_Y,             320, EA_TRACE & 0x3186);
    gfx->drawFastHLine(0, GP_BTN_Y + GP_BTN_H,  320, EA_TRACE & 0x3186);

    // Section label
    gfx->setTextSize(1);
    gfx->setTextColor(EA_SILVER);
    gfx->setCursor(4, GP_BTN_Y - 8);
    gfx->print("BUTTON TEST");

    for (int i = 0; i < NUM_BTNS; i++) {
        gp_draw_button(i, (buttons & BTNS[i].mask) != 0);
    }
}

// ─────────────────────────────────────────────
//  DRAW PAIRING INSTRUCTIONS
// ─────────────────────────────────────────────
static void gp_draw_instructions(bool connected) {
    gfx->fillRect(0, GP_INSTR_Y, 320, GP_INSTR_H, EA_BG);
    gfx->drawFastHLine(0, GP_INSTR_Y, 320, EA_TRACE & 0x3186);

    gfx->setTextSize(1);
    if (!connected) {
        // Step-by-step pairing guide in one compact line block
        gfx->setTextColor(EA_AMBER);
        gfx->setCursor(6, GP_INSTR_Y + 4);
        gfx->print("PAIR: Hold SELECT+R-Shoulder 3s");
        gfx->setTextColor(EA_DIM);
        gfx->setCursor(6, GP_INSTR_Y + 15);
        gfx->print("Purple LED = BLE mode. Blue LED = wrong mode (won't connect).");
    } else {
        gfx->setTextColor(EA_DIM);
        gfx->setCursor(6, GP_INSTR_Y + 4);
        gfx->print("Press any button to test. HOME returns to launcher (OS reserved).");
        gfx->setCursor(6, GP_INSTR_Y + 15);
        gfx->print("Disconnect: unplug controller or hold HOME 3s on Zero 2.");
    }
}

// ─────────────────────────────────────────────
//  DRAW FULL GAMEPAD SETUP SCREEN
// ─────────────────────────────────────────────
static void gp_draw_screen() {
    gfx->fillScreen(EA_BG);
    ea_draw_header("GAMEPAD SETUP", EA_SILVER);
    gp_draw_status(g_gamepad.connected, g_gamepad.device_name,
                   g_gamepad.last_input_ms);
    gp_draw_all_buttons(0);
    gp_draw_instructions(g_gamepad.connected);
    ea_draw_footer("B / header tap to exit  |  R to disconnect", EA_SILVER);
}

// ─────────────────────────────────────────────
//  MAIN ENTRY POINT
// ─────────────────────────────────────────────
void run_gamepad_setup() {
    gp_draw_screen();

    // State tracking for differential redraws
    bool     last_connected   = g_gamepad.connected;
    uint16_t last_buttons     = 0;
    uint32_t last_status_ms   = millis();

    while (true) {
        // ── Poll gamepad (intercepts HOME → returns true) ──
        if (gamepad_poll()) return;  // HOME = exit to launcher

        // ── Check for trackball / keyboard exit ──
        TrackballState tb = update_trackball();
        if (tb.x == -1) return;  // Trackball left = exit

        char k = get_keypress();
        if (k == 'b' || k == 'B' || k == 'q' || k == 'Q') return;
        if (k == 'r' || k == 'R') {
            gamepad_disconnect();
            // Redraw status after disconnect request
            gp_draw_status(g_gamepad.connected, g_gamepad.device_name,
                           g_gamepad.last_input_ms);
            gp_draw_instructions(g_gamepad.connected);
        }

        // ── Touch exit ──
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            while (get_touch(&tx, &ty)) { delay(10); }
            if (ty < 24) return;  // Header tap = exit
        }

        // ── Button test — redraw only changed buttons ──
        uint16_t cur_buttons = g_gamepad.buttons;
        if (cur_buttons != last_buttons) {
            uint16_t changed = cur_buttons ^ last_buttons;
            for (int i = 0; i < NUM_BTNS; i++) {
                if (changed & BTNS[i].mask) {
                    gp_draw_button(i, (cur_buttons & BTNS[i].mask) != 0);
                }
            }
            last_buttons = cur_buttons;
        }

        // ── Connection state change ──
        if (g_gamepad.connected != last_connected) {
            last_connected = g_gamepad.connected;
            gp_draw_status(g_gamepad.connected, g_gamepad.device_name,
                           g_gamepad.last_input_ms);
            gp_draw_all_buttons(0);   // Clear any stale button states
            gp_draw_instructions(g_gamepad.connected);
            last_buttons = 0;
        }

        // ── Periodic status refresh (idle time, heap, battery) ──
        if (millis() - last_status_ms > 2000) {
            gp_draw_status(g_gamepad.connected, g_gamepad.device_name,
                           g_gamepad.last_input_ms);
            last_status_ms = millis();
        }

        delay(20);
    }
}