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
 * PISCES MOON OS — LAUNCHER v2.1
 * Two-level folder navigation:
 *
 * Level 1: Category grid (6 folders, one page)
 * Level 2: App grid within a category (3x2, paged if >6 apps)
 *
 * Navigation:
 * Touch single tap            = highlight
 * Touch double tap            = open folder / launch app
 * Trackball roll              = move cursor
 * Trackball click             = open highlighted / launch highlighted
 * Trackball LEFT (in folder)  = exit back to categories
 * BACK button in header       = exit back to categories
 *
 * v2.2 changes (v0.9.6 "ELF On A Shelf"):
 * - Added APP_IDs 32-38 (Voice Terminal, LoRa PTT, SSH, MicroPython,
 * Retro Pack, ELF Browser, Gamepad Setup)
 * - COMMS: +VOICE, +LORA PTT
 * - GAMES: +RETRO (ROM browser / NES/GB/Atari ELF launcher)
 * - INTEL: +SSH
 * - SYSTEM: +uPY REPL, +ELF APPS, +GAMEPAD
 * - Added includes: elf_loader.h, gamepad.h, voice_terminal.h,
 * lora_voice.h, ssh_client.h, micropython_app.h
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <TinyGPSPlus.h>
#include <XPowersLib.h>
#include "touch.h"
#include "trackball.h"
#include "bluetooth_app.h"
#include "data_reader.h"
#include "audio_player.h"
#include "audio_recorder.h"
#include "pacman.h"
#include "galaga.h"
#include "chess.h"
#include "baseball.h"
#include "pkt_sniffer.h"
#include "beacon_spotter.h"
#include "net_scanner.h"
#include "hash_tool.h"
#include "doom_app.h"
#include "simcity.h"
#include "trails.h"
#include "mesh_messenger.h"
#include "gemini_client.h"
#include "voice_terminal.h"
#include "lora_voice.h"
#include "ssh_client.h"
#include "micropython_app.h"
#include "elf_loader.h"
#include "wifi_filemgr.h"
#include "gamepad.h"
#include "ble_gatt_explorer.h"
#include "wpa_handshake.h"
#include "rf_spectrum.h"
#include "probe_intel.h"
#include "offline_pkt_analysis.h"
#include "ble_ducky.h"
#include "usb_ducky.h"
#include "wifi_ducky.h"
#include "bridge_app.h"
#include "apps.h"

extern Arduino_GFX *gfx;
extern bool exitApp;
extern TinyGPSPlus gps;
extern XPowersAXP2101 PMU;

// ─────────────────────────────────────────────
//  THEME — Cyberpunk Circuit Aesthetic
// ─────────────────────────────────────────────
#define C_APP_RED    0xF800
#define C_SEL_BLUE   0x001F
#define C_MATRIX     0x07E0   // Bright green
#define C_DARK       0x0821   // Very dark (near-black with blue tint)
#define C_BLACK      0x0000
#define C_GREY       0x7BEF
#define C_WHITE      0xFFFF
#define C_GRID       0x0120   // Dark green grid lines
#define C_TRACE      0x0340   // PCB trace green
#define C_PAD        0x0560   // Solder pad green
#define C_SCAN       0x07E0   // Scan line highlight
#define C_HEADER_BG  0x0010   // Deep blue-black header
#define C_CHAMFER    0x0600   // Chamfer line color (mid green)
#define C_GLOW       0x0BE0   // Icon glow (bright-mid green)

// Category accent colors — used for chamfer borders and trace details
// Each folder has a distinct color that carries through to its app grid
static const uint16_t CAT_ACCENT[] = {
    0x03EF,  // COMMS  — teal
    0xF400,  // CYBER  — red-orange
    0xFD20,  // TOOLS  — amber
    0x07E0,  // GAMES  — green
    0x07FF,  // INTEL  — cyan
    0xF81F,  // MEDIA  — magenta
    0x8410,  // SYSTEM — silver
};


// ─────────────────────────────────────────────
//  CIRCUIT BACKGROUND (shared with splash)
//  Draws PCB grid + trace runs over the full
//  launcher area (y 24 to 210), leaving header/
//  footer clear.
// ─────────────────────────────────────────────
static void drawLauncherBackground() {
    // Fill content area
    gfx->fillRect(0, 24, 320, 186, C_BLACK);

    // Dim grid — 20px spacing
    for (int x = 0; x < 320; x += 20)
        gfx->drawFastVLine(x, 24, 186, C_GRID);
    for (int y = 24; y < 210; y += 20)
        gfx->drawFastHLine(0, y, 320, C_GRID);

    // PCB trace runs — horizontal with right-angle jogs
    // Placed to weave between the 3×3 icon grid
    // Grid boxes sit at x=17,117,217 y=40,106,172 (approx for 7 cats)
    // Traces run through the gaps

    // Top trace cluster
    gfx->drawFastHLine(0,   28, 15,  C_TRACE);
    gfx->drawFastVLine(15,  28, 12,  C_TRACE);
    gfx->drawFastHLine(15,  40, 0,   C_TRACE);  // stub

    gfx->drawFastHLine(305, 28, 15,  C_TRACE);
    gfx->drawFastVLine(305, 28, 12,  C_TRACE);

    // Between row 0 and row 1 (around y=98)
    gfx->drawFastHLine(0,   98,  14, C_TRACE);
    gfx->drawFastVLine(14,  90,  8,  C_TRACE);
    gfx->drawFastHLine(14,  90,  6,  C_TRACE);

    gfx->drawFastHLine(306, 98,  14, C_TRACE);
    gfx->drawFastVLine(306, 90,  8,  C_TRACE);
    gfx->drawFastHLine(300, 90,  6,  C_TRACE);

    // Between row 1 and row 2 (around y=164)
    gfx->drawFastHLine(0,   164, 14, C_TRACE);
    gfx->drawFastVLine(14,  156, 8,  C_TRACE);
    gfx->drawFastHLine(14,  156, 6,  C_TRACE);

    gfx->drawFastHLine(306, 164, 14, C_TRACE);
    gfx->drawFastVLine(306, 156, 8,  C_TRACE);

    // Diagonal trace suggestion (top-right corner)
    gfx->drawLine(300, 24, 319, 43,  C_TRACE);
    // Bottom-left diagonal
    gfx->drawLine(0,  191, 19, 210,  C_TRACE);

    // Solder pads at junctions
    gfx->fillRect(13,  38,  4, 4, C_PAD);
    gfx->fillRect(13,  154, 4, 4, C_PAD);
    gfx->fillRect(304, 88,  4, 4, C_PAD);
    gfx->fillRect(304, 96,  4, 4, C_PAD);
    gfx->fillRect(13,  88,  4, 4, C_PAD);
}

// ─────────────────────────────────────────────
//  CHAMFERED BOX
//  Draws a hexagonal (chamfered corner) box —
//  the core visual element for all icons.
//  fill   = background fill color
//  border = chamfer line color
//  cut    = corner cut size in pixels
// ─────────────────────────────────────────────
static void drawChamferedBox(int x, int y, int w, int h,
                              uint16_t fill, uint16_t border, int cut = 6) {
    // Fill interior (inset from chamfer edges)
    gfx->fillRect(x + cut, y,      w - cut*2, h,      fill);
    gfx->fillRect(x,       y + cut, cut,      h-cut*2, fill);
    gfx->fillRect(x+w-cut, y + cut, cut,      h-cut*2, fill);

    // Chamfer lines
    gfx->drawLine(x,       y+cut,   x+cut,   y,       border);  // TL
    gfx->drawLine(x+w-cut, y,       x+w,     y+cut,   border);  // TR
    gfx->drawLine(x,       y+h-cut, x+cut,   y+h,     border);  // BL
    gfx->drawLine(x+w-cut, y+h,     x+w,     y+h-cut, border);  // BR

    // Straight edges
    gfx->drawFastHLine(x+cut,   y,    w-cut*2, border);  // Top
    gfx->drawFastHLine(x+cut,   y+h,  w-cut*2, border);  // Bottom
    gfx->drawFastVLine(x,       y+cut, h-cut*2, border); // Left
    gfx->drawFastVLine(x+w,     y+cut, h-cut*2, border); // Right
}

// ─────────────────────────────────────────────
//  CATEGORY ICON TRACE DECORATION
//  Draws short PCB trace stubs running off the
//  edges of an icon box — makes each icon look
//  like it's wired into the circuit board.
// ─────────────────────────────────────────────
static void drawIconTraces(int x, int y, int w, int h, uint16_t col) {
    int mx = x + w/2;
    int my = y + h/2;

    // Short traces off top edge
    gfx->drawFastVLine(mx - 8, y - 6, 6, col);
    gfx->drawFastHLine(mx - 12, y - 6, 8, col);
    gfx->fillRect(mx - 13, y - 8, 3, 3, col);  // Pad

    // Short trace off bottom
    gfx->drawFastVLine(mx + 8, y + h, 6, col);
    gfx->drawFastHLine(mx + 4,  y + h + 6, 8, col);
    gfx->fillRect(mx + 11, y + h + 4, 3, 3, col);  // Pad

    // Right side trace
    gfx->drawFastHLine(x + w, my - 6, 6, col);
    gfx->drawFastVLine(x + w + 6, my - 10, 8, col);
    gfx->fillRect(x + w + 4, my - 11, 3, 3, col);  // Pad
}

// ─────────────────────────────────────────────
//  HEADER — Rainbow "PISCES MOON OS" title
//  Static version of the splash animation
// ─────────────────────────────────────────────
static void drawCyberpunkHeader() {
    gfx->fillRect(0, 0, 320, 24, C_HEADER_BG);

    // Thin circuit trace along bottom of header
    gfx->drawFastHLine(0, 22, 320, C_TRACE);
    gfx->drawFastHLine(0, 23, 320, C_MATRIX);

    // Small chip accent left
    gfx->fillRect(4, 4, 10, 16, C_CHAMFER);
    gfx->drawRect(4, 4, 10, 16, C_MATRIX);
    gfx->drawFastHLine(2, 8,  2, C_MATRIX);
    gfx->drawFastHLine(2, 12, 2, C_MATRIX);
    gfx->drawFastHLine(2, 16, 2, C_MATRIX);
    gfx->drawFastHLine(14,8,  2, C_MATRIX);
    gfx->drawFastHLine(14,12, 2, C_MATRIX);
    gfx->drawFastHLine(14,16, 2, C_MATRIX);

    // "PISCES MOON OS" in rainbow — static alignment from splash
    const char* title = "PISCES MOON OS";
    uint16_t colors[] = {
        0xF81F,  // P - Magenta
        0x001F,  // I - Blue
        0x07FF,  // S - Cyan
        0x07E0,  // C - Green
        0xFFE0,  // E - Yellow
        0xFD20,  // S - Orange
        0xFFFF,  //   - White (space)
        0xF800,  // M - Red
        0xFD20,  // O - Orange
        0x07E0,  // O - Green
        0x07FF,  // N - Cyan
        0xFFFF,  //   - White (space)
        0x07E0,  // O - Green
        0xF81F,  // S - Magenta
    };
    gfx->setTextSize(1);
    gfx->setCursor(22, 8);
    for (int i = 0; i < (int)strlen(title); i++) {
        gfx->setTextColor(colors[i]);
        gfx->print(title[i]);
    }

    // Version tag far right
    gfx->setTextColor(C_TRACE);
    gfx->setCursor(262, 8);
    gfx->print("v1.0.1");
}

// ─────────────────────────────────────────────
//  DATA STRUCTURES
// ─────────────────────────────────────────────
struct AppEntry { const char* name; int id; };

struct Category {
    const char* name;
    const char* icon;
    uint16_t    color;
    AppEntry    apps[16];   // Sized for largest category (CYBER: 14 apps)
    int         appCount;
};

// ─────────────────────────────────────────────
//  APP IDs
// ─────────────────────────────────────────────
#define APP_WIFI_JOIN    1
#define APP_BT_RADAR     2
#define APP_GPS          3
#define APP_WARDRIVE     4
#define APP_PLAYER       5
#define APP_RECORDER     6
#define APP_JOURNAL      7
#define APP_CALC         8
#define APP_CLOCK        9
#define APP_CALENDAR     10
#define APP_ETCH         11
#define APP_SNAKE        12
#define APP_PACMAN       13
#define APP_GALAGA       14
#define APP_TERMINAL     15
#define APP_GEMINI_LOG   16
#define APP_REF_MED      17
#define APP_REF_SURV     18
#define APP_FILES        19
#define APP_ABOUT        20
#define APP_SYSTEM       21
#define APP_CHESS        22
#define APP_BASEBALL     23
#define APP_PKT_SNIFFER  24
#define APP_BEACON       25
#define APP_NET_SCANNER  26
#define APP_HASH_TOOL    27
#define APP_DOOM         28
#define APP_SIMCITY      29
#define APP_TRAILS       30
#define APP_MESH         31

// v0.9.5 — new comms / system apps
#define APP_VOICE_TERM   32   // Speech-to-Text + Gemini + TTS
#define APP_LORA_VOICE   33   // LoRa push-to-talk (Codec2)
#define APP_SSH          34   // SSH terminal client
#define APP_MICROPYTHON  35   // MicroPython REPL

// v1.0.0 — ELF On A Shelf
#define APP_RETRO        36   // ROM browser + NES/GB/Atari launcher
#define APP_ELF_BROWSER  37   // Generic ELF app loader
#define APP_GAMEPAD      38   // BLE gamepad pairing + test
#define APP_FILEMGR      39   // WiFi file manager — SD card over HTTP

// v0.9.7 — CYBER Expansion
#define APP_BLE_GATT     40   // BLE GATT service/characteristic explorer
#define APP_WPA_HS       41   // WPA handshake capture (.hccapx)
#define APP_RF_SPECTRUM  42   // SX1262 RF spectrum visualizer
#define APP_PROBE_INTEL  43   // Probe request intelligence / device fingerprinting
#define APP_PKT_ANALYSIS 44   // Offline packet analysis engine

// v0.9.7 — Ducky Suite
#define APP_BLE_DUCKY    45   // BLE HID keyboard injection (DuckyScript)
#define APP_USB_DUCKY    46   // USB HID keyboard injection (requires HID build)
#define APP_WIFI_DUCKY   47   // WiFi payload delivery / reverse C2

// v1.0.1 — Bridge App
#define APP_BRIDGE       48   // USB Serial JSON bridge for web emulator

// ─────────────────────────────────────────────
//  CATEGORY DEFINITIONS
//
//  Folder colors (RGB565):
//  COMMS  = Teal       0x03EF
//  CYBER  = Red/orange 0xF200  (distinct, signals security context)
//  TOOLS  = Dark red   0x8C00
//  GAMES  = Dark green 0x0400
//  MEDIA  = Dark amber 0x7800
//  INTEL  = Dark blue  0x000F
//  SYSTEM = Dark grey  0x3186
// ─────────────────────────────────────────────
static const Category categories[] = {

    { "COMMS", "C", 0x03EF,
      {{"WIFI JOIN", APP_WIFI_JOIN},
       {"GPS",       APP_GPS},
       {"MESH",      APP_MESH},
       {"VOICE",     APP_VOICE_TERM},
       {"LORA PTT",  APP_LORA_VOICE}},
      5 },

    { "CYBER", "!", 0xF200,
      {{"WARDRIVE",  APP_WARDRIVE},
       {"BT RADAR",  APP_BT_RADAR},
       {"PKT SNIFF", APP_PKT_SNIFFER},
       {"BEACON",    APP_BEACON},
       {"NET SCAN",  APP_NET_SCANNER},
       {"HASH TOOL", APP_HASH_TOOL},
       {"GATT XPLR", APP_BLE_GATT},
       {"WPA HS",    APP_WPA_HS},
       {"RF SPECTRM",APP_RF_SPECTRUM},
       {"PROBE INTL",APP_PROBE_INTEL},
       {"PKT ANLYS", APP_PKT_ANALYSIS},
       {"BLE DUCKY", APP_BLE_DUCKY},
       {"USB DUCKY", APP_USB_DUCKY},
       {"WIFI DUCKY",APP_WIFI_DUCKY}},
      14 },

    { "TOOLS", "T", 0x8C00,
      {{"JOURNAL",   APP_JOURNAL},
       {"CALC",      APP_CALC},
       {"CLOCK",     APP_CLOCK},
       {"CALENDAR",  APP_CALENDAR},
       {"ETCH",      APP_ETCH}},
      5 },

    { "GAMES", "G", 0x0400,
      {{"SNAKE",     APP_SNAKE},
       {"PAC-MAN",   APP_PACMAN},
       {"GALAGA",    APP_GALAGA},
       {"CHESS",     APP_CHESS},
       {"DOOM",      APP_DOOM},
       {"SIMCITY",   APP_SIMCITY},
       {"RETRO",     APP_RETRO}},
      7 },

    { "INTEL", "I", 0x000F,
      {{"TERMINAL",  APP_TERMINAL},
       {"GEMINI LOG",APP_GEMINI_LOG},
       {"REF: MED",  APP_REF_MED},
       {"REF: SURV", APP_REF_SURV},
       {"BASEBALL",  APP_BASEBALL},
       {"TRAILS",    APP_TRAILS},
       {"SSH",       APP_SSH}},
      7 },

    { "MEDIA", "M", 0x7800,
      {{"PLAYER",    APP_PLAYER},
       {"RECORDER",  APP_RECORDER}},
      2 },

    { "SYSTEM", "S", 0x3186,
      {{"FILES",     APP_FILES},
       {"SD FILES",  APP_FILEMGR},
       {"ABOUT",     APP_ABOUT},
       {"SYSTEM",    APP_SYSTEM},
       {"uPY REPL",  APP_MICROPYTHON},
       {"ELF APPS",  APP_ELF_BROWSER},
       {"GAMEPAD",   APP_GAMEPAD},
       {"BRIDGE",    APP_BRIDGE}},
      8 },

};
#define NUM_CATEGORIES 7

// ─────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────
static int  currentLevel     = 0;
static int  selectedCategory = -1;
static int  openCategory     = -1;
static int  selectedApp      = -1;
static int  appPage          = 0;

// ─────────────────────────────────────────────
//  GRID GEOMETRY
//  7 categories — two rows: 3 top, 4 bottom (or 4+3)
//  We use a 3-wide grid with row wrapping. Row 0 = cats 0-2, row 1 = cats 3-5, row 2 = cat 6
//  But the existing 3-col geometry handles this fine with paging at category level if needed.
//  For 7 cats in a 3-col grid: row 0 = 0,1,2  row 1 = 3,4,5  row 2 = 6
//  We shrink box height slightly to fit 3 rows.
// ─────────────────────────────────────────────
#define BOX_W  85
#define BOX_H  56    // Reduced from 65 to fit 3 rows
#define GAP_X  15
#define GAP_Y  10
#define GRID_X 17
#define GRID_Y 32

static void getBoxPos(int slot, int& bx, int& by) {
    bx = GRID_X + (slot % 3) * (BOX_W + GAP_X);
    by = GRID_Y + (slot / 3) * (BOX_H + GAP_Y);
}

// ─────────────────────────────────────────────
//  STATUS BAR
// ─────────────────────────────────────────────
static void updateStatusBar() {
    // Status bar lives in the bottom 18px (y 222-240)
    gfx->fillRect(0, 222, 320, 18, C_BLACK);
    gfx->drawFastHLine(0, 222, 320, C_GRID);
    gfx->setTextSize(1);

    // WiFi
    gfx->setCursor(4, 226);
    if (WiFi.status() == WL_CONNECTED) {
        gfx->setTextColor(C_MATRIX);
        gfx->print("*WIFI");   // bullet + WIFI
    } else {
        gfx->setTextColor(0x2104);
        gfx->print(" WIFI");
    }

    // GPS
    gfx->setCursor(60, 226);
    if (gps.location.isValid()) {
        gfx->setTextColor(C_MATRIX); gfx->print("*GPS:FIX");
    } else if (gps.satellites.value() > 0) {
        gfx->setTextColor(0xFD20);
        gfx->printf("*GPS:%dS", gps.satellites.value());
    } else {
        gfx->setTextColor(0x2104); gfx->print(" GPS:--");
    }

    // Battery
    gfx->setCursor(148, 226);
    if (PMU.isBatteryConnect()) {
        int bat = min((int)PMU.getBatteryPercent(), 100);
        uint16_t batCol = bat > 50 ? C_MATRIX : bat > 20 ? 0xFD20 : C_APP_RED;
        gfx->setTextColor(batCol);
        gfx->printf("*BAT:%d%%", bat);
    } else {
        gfx->setTextColor(0xFFE0); gfx->print("*USB");
    }

    // Cyber warning
    if (openCategory >= 0 && strcmp(categories[openCategory].name, "CYBER") == 0) {
        gfx->setTextColor(0xFD20);
        gfx->setCursor(230, 226);
        gfx->print("[AUTH ONLY]");
    }
}

// ─────────────────────────────────────────────
//  CATEGORY SCREEN
// ─────────────────────────────────────────────
static void drawCategoryGrid() {
    // Background drawn by drawLauncherBackground() — just draw icons on top
    for (int i = 0; i < NUM_CATEGORIES; i++) {
        int bx, by; getBoxPos(i, bx, by);
        bool sel = (selectedCategory == i);
        uint16_t accent = CAT_ACCENT[i];

        // Trace decorations behind box
        if (!sel) drawIconTraces(bx, by, BOX_W, BOX_H, accent);

        // Chamfered box — dark fill, accent border
        drawChamferedBox(bx, by, BOX_W, BOX_H,
                         sel ? 0x0018 : C_DARK,
                         sel ? accent : (accent & 0x39E7), 8);

        // Inner corner accent lines for depth
        if (sel) {
            gfx->drawLine(bx+2, by+10, bx+10, by+2, accent);
            gfx->drawLine(bx+BOX_W-10, by+2, bx+BOX_W-2, by+10, accent);
            // Scan line — subtle CRT feel
            int scanY = by + ((millis() / 80) % BOX_H);
            gfx->drawFastHLine(bx+8, scanY, BOX_W-16, accent & 0x1CE7);
        } else {
            gfx->drawLine(bx+2, by+10, bx+10, by+2, accent & 0x2104);
            gfx->drawLine(bx+BOX_W-10, by+2, bx+BOX_W-2, by+10, accent & 0x2104);
        }

        // Big letter icon
        gfx->setTextSize(3);
        gfx->setTextColor(sel ? accent : C_GLOW);
        gfx->setCursor(bx + BOX_W/2 - 9, by + 10);
        gfx->print(categories[i].icon);

        // Category name
        gfx->setTextSize(1);
        gfx->setTextColor(sel ? accent : 0xA534);
        int nlen = strlen(categories[i].name);
        gfx->setCursor(bx + (BOX_W - nlen*6)/2, by + BOX_H - 12);
        gfx->print(categories[i].name);

        // App count — dim, top-right
        gfx->setTextColor(sel ? accent : 0x2945);
        gfx->setCursor(bx + BOX_W - 13, by + 3);
        gfx->printf("%d", categories[i].appCount);

        // CYBER pulsing alert dot
        if (strcmp(categories[i].name, "CYBER") == 0) {
            uint16_t dotCol = ((millis() / 400) % 2) ? 0xF400 : 0xFD20;
            gfx->fillRect(bx + 3, by + 3, 3, 3, dotCol);
        }
    }
}

static void drawCategoryUI() {
    gfx->fillScreen(C_BLACK);
    drawCyberpunkHeader();
    drawLauncherBackground();

    // Footer
    gfx->fillRect(0, 210, 320, 12, C_HEADER_BG);
    gfx->drawFastHLine(0, 210, 320, C_TRACE);
    gfx->setTextColor(0x2945); gfx->setTextSize(1);
    gfx->setCursor(60, 213); gfx->print("DBL-TAP OR CLICK TO OPEN");

    drawCategoryGrid();
    updateStatusBar();
}

// ─────────────────────────────────────────────
//  APP SCREEN
// ─────────────────────────────────────────────
static void drawAppGrid() {
    const Category& cat = categories[openCategory];
    int startIdx = appPage * 6;
    int catIdx = openCategory;
    uint16_t accent = (catIdx < NUM_CATEGORIES) ? CAT_ACCENT[catIdx] : C_MATRIX;

    // Circuit background in content area
    gfx->fillRect(0, 24, 320, 186, C_BLACK);
    for (int x = 0; x < 320; x += 20)
        gfx->drawFastVLine(x, 24, 186, C_GRID);
    for (int y = 24; y < 210; y += 20)
        gfx->drawFastHLine(0, y, 320, C_GRID);

    // Side trace accents in folder color
    gfx->drawFastVLine(1,  26, 182, accent & 0x1CE7);
    gfx->drawFastVLine(318,26, 182, accent & 0x1CE7);

    for (int slot = 0; slot < 6; slot++) {
        int appIdx = startIdx + slot;
        int bx, by; getBoxPos(slot, bx, by);
        if (appIdx >= cat.appCount) continue;

        bool sel = (selectedApp == appIdx);

        // Trace stubs behind box
        if (!sel) drawIconTraces(bx, by, BOX_W, BOX_H, accent & 0x39E7);

        // Chamfered box
        uint16_t fillCol = sel ? 0x0018 : C_DARK;
        uint16_t bdrCol  = sel ? accent  : (accent & 0x39E7);
        drawChamferedBox(bx, by, BOX_W, BOX_H, fillCol, bdrCol, 7);

        // Inner corner accent (just TL + BR)
        if (sel) {
            gfx->drawLine(bx+2, by+9, bx+9, by+2, accent);
            gfx->drawLine(bx+BOX_W-9, by+BOX_H-2, bx+BOX_W-2, by+BOX_H-9, accent);
        }

        // App name — centered, category accent color when selected
        gfx->setTextSize(1);
        gfx->setTextColor(sel ? accent : 0xC618);
        const char* nm = cat.apps[appIdx].name;
        int nw = strlen(nm) * 6;
        gfx->setCursor(bx + (BOX_W - nw)/2, by + BOX_H/2 - 4);
        gfx->print(nm);

        // Subtle dot in bottom-right corner — circuit pad feel
        gfx->fillRect(bx + BOX_W - 5, by + BOX_H - 5, 3, 3,
                      sel ? accent : (accent & 0x2104));
    }
}

static void drawAppUI() {
    gfx->fillScreen(C_BLACK);
    const Category& cat = categories[openCategory];
    int catIdx = openCategory;
    uint16_t accent = (catIdx < NUM_CATEGORIES) ? CAT_ACCENT[catIdx] : C_MATRIX;
    bool isCyber = (strcmp(cat.name, "CYBER") == 0);

    // Header — dark with accent trace
    gfx->fillRect(0, 0, 320, 24, C_HEADER_BG);
    gfx->drawFastHLine(0, 22, 320, C_TRACE);
    gfx->drawFastHLine(0, 23, 320, accent);

    // Chamfered back button
    drawChamferedBox(4, 3, 44, 18, C_DARK, accent & 0x39E7, 4);
    gfx->setTextColor(accent); gfx->setTextSize(1);
    gfx->setCursor(9, 8); gfx->print("< BACK");

    // Category name with accent color
    gfx->setTextSize(2);
    int nlen = strlen(cat.name);
    gfx->setTextColor(accent);
    gfx->setCursor((320 - nlen*12)/2, 5);
    gfx->print(cat.name);

    drawAppGrid();

    gfx->fillRect(0, 210, 320, 12, C_HEADER_BG);
    gfx->drawFastHLine(0, 210, 320, C_TRACE);
    gfx->setTextSize(1);
    int totalPages = (cat.appCount + 5) / 6;
    if (totalPages > 1) {
        gfx->setTextColor(accent);
        gfx->setCursor(10, 213);
        if (appPage > 0) gfx->print("< PREV");
        gfx->setTextColor(0x2945);
        gfx->setCursor(130, 213);
        gfx->printf("PAGE %d/%d", appPage+1, totalPages);
        gfx->setTextColor(accent);
        gfx->setCursor(265, 213);
        if (appPage < totalPages-1) gfx->print("NEXT >");
    } else {
        gfx->setCursor(40, 213);
        if (isCyber) {
            gfx->setTextColor(0xFD20);
            gfx->print("OWN/AUTHORIZED NETWORKS ONLY");
        } else {
            gfx->setTextColor(0x2945);
            gfx->print("DBL-TAP OR CLICK TO LAUNCH");
        }
    }
    updateStatusBar();
}

// ─────────────────────────────────────────────
//  LAUNCH APP
// ─────────────────────────────────────────────
static void launchApp(int launchId) {
    exitApp = false;
    gfx->fillScreen(C_BLACK);
    int16_t dumpX, dumpY;
    while (get_touch(&dumpX, &dumpY)) { delay(10); yield(); }
    delay(100);

    switch (launchId) {
        // COMMS
        case APP_WIFI_JOIN:    run_wifi_connect();                          break;
        case APP_GPS:          run_gps();                                    break;
        case APP_MESH:         run_mesh_messenger();                         break;
        case APP_VOICE_TERM:   run_voice_terminal();                         break;
        case APP_LORA_VOICE:   run_lora_voice();                             break;

        // CYBER
        case APP_WARDRIVE:     run_wardrive();                              break;
        case APP_BT_RADAR:     runBluetoothApp();                           break;
        case APP_PKT_SNIFFER:  run_pkt_sniffer();                           break;
        case APP_BEACON:       run_beacon_spotter();                        break;
        case APP_NET_SCANNER:  run_net_scanner();                           break;
        case APP_HASH_TOOL:    run_hash_tool();                             break;

        // TOOLS
        case APP_JOURNAL:      run_notepad();                               break;
        case APP_CALC:         run_calculator();                            break;
        case APP_CLOCK:        run_clock();                                 break;
        case APP_CALENDAR:     run_calendar();                              break;
        case APP_ETCH:         run_etch();                                  break;

        // GAMES
        case APP_SNAKE:        run_snake();                                 break;
        case APP_PACMAN:       run_pacman();                                break;
        case APP_GALAGA:       run_galaga();                                break;
        case APP_CHESS:        run_chess();                                 break;
        case APP_DOOM:         run_doom();                                   break;
        case APP_SIMCITY:      run_simcity();                                break;

        // INTEL
        case APP_TERMINAL:     run_terminal();                              break;
        case APP_GEMINI_LOG:   run_data_reader("gemini",  "GEMINI LOG");   break;
        case APP_REF_MED:      run_data_reader("medical", "MEDICAL REF");  break;
        case APP_REF_SURV:     run_data_reader("survival","SURVIVAL");     break;
        case APP_BASEBALL:     run_baseball();                              break;
        case APP_TRAILS:      run_trails();                                break;
        case APP_SSH:          run_ssh_client();                             break;

        // MEDIA
        case APP_PLAYER:       run_audio_player();                          break;
        case APP_RECORDER:     run_audio_recorder();                        break;

        // SYSTEM
        case APP_FILES:        run_filesystem();                            break;
        case APP_FILEMGR:      run_wifi_filemgr();                          break;
        case APP_ABOUT:        run_about();                                 break;
        case APP_SYSTEM:       run_system();                                break;
        case APP_MICROPYTHON:  run_micropython();                            break;
        case APP_ELF_BROWSER:  run_elf_browser();                            break;
        
        // --- FIXED ROUTING ---
        case APP_GAMEPAD:      gamepad_pair();                               break;

        // GAMES — ELF retro pack
        case APP_RETRO:        run_retro_pack();                             break;

        // CYBER — v0.9.7 expansion
        case APP_BLE_GATT:     run_ble_gatt_explorer();                     break;
        case APP_WPA_HS:       run_wpa_handshake();                         break;
        case APP_RF_SPECTRUM:  run_rf_spectrum();                           break;
        case APP_PROBE_INTEL:  run_probe_intel();                           break;
        case APP_PKT_ANALYSIS: run_offline_pkt_analysis();                  break;

        // CYBER — Ducky Suite
        case APP_BLE_DUCKY:    run_ble_ducky();                             break;
        case APP_USB_DUCKY:    run_usb_ducky();                             break;
        case APP_WIFI_DUCKY:   run_wifi_ducky();                            break;

        // SYSTEM — Bridge App
        case APP_BRIDGE:       run_bridge();                                break;

        default:
            gfx->setCursor(80, 110); gfx->setTextColor(C_APP_RED);
            gfx->setTextSize(2); gfx->print("UNKNOWN APP");
            delay(1000); break;
    }

    selectedApp = -1;
    while (get_touch(&dumpX, &dumpY)) { delay(10); yield(); }
    drawAppUI();
}

// ─────────────────────────────────────────────
//  FOLDER OPEN / CLOSE
// ─────────────────────────────────────────────
static void openFolder(int idx) {
    openCategory = idx;
    currentLevel = 1;
    selectedApp  = -1;
    appPage      = 0;
    drawAppUI();
}

static void closeFolder() {
    currentLevel     = 0;
    openCategory     = -1;
    selectedApp      = -1;
    appPage          = 0;
    drawCategoryUI();
}

// ─────────────────────────────────────────────
//  TOUCH HANDLER
// ─────────────────────────────────────────────
static void handleTouch() {
    int16_t tx, ty;
    if (!get_touch(&tx, &ty)) return;
    static int16_t lastTx = -1, lastTy = -1;
    static unsigned long lastTouchTime = 0;

    bool doubleTap = (abs(tx - lastTx) < 20 && abs(ty - lastTy) < 20 &&
                      millis() - lastTouchTime < 500);
    lastTx = tx; lastTy = ty; lastTouchTime = millis();

    while (get_touch(&tx, &ty)) { delay(10); yield(); }

    if (currentLevel == 0) {
        // Header area — nothing to do
        if (ty < 40) return;
        // Find tapped category
        for (int i = 0; i < NUM_CATEGORIES; i++) {
            int bx, by; getBoxPos(i, bx, by);
            if (tx >= bx && tx < bx + BOX_W && ty >= by && ty < by + BOX_H) {
                if (doubleTap) { openFolder(i); return; }
                if (selectedCategory != i) { selectedCategory = i; drawCategoryGrid(); }
                return;
            }
        }
    } else {
        // Back button
        if (ty < 40) { closeFolder(); return; }
        // App tap
        const Category& cat = categories[openCategory];
        int startIdx = appPage * 6;
        for (int slot = 0; slot < 6; slot++) {
            int appIdx = startIdx + slot;
            if (appIdx >= cat.appCount) continue;
            int bx, by; getBoxPos(slot, bx, by);
            if (tx >= bx && tx < bx + BOX_W && ty >= by && ty < by + BOX_H) {
                if (doubleTap) { launchApp(cat.apps[appIdx].id); return; }
                if (selectedApp != appIdx) { selectedApp = appIdx; drawAppGrid(); }
                return;
            }
        }
        // Footer paging
        if (ty > 210) {
            int totalPages = (cat.appCount + 5) / 6;
            if (tx < 80 && appPage > 0)               { appPage--; selectedApp = -1; drawAppUI(); }
            if (tx > 240 && appPage < totalPages - 1)  { appPage++; selectedApp = -1; drawAppUI(); }
        }
    }
}

// ─────────────────────────────────────────────
//  TRACKBALL HANDLER
// ─────────────────────────────────────────────
static void handleTrackball() {
    TrackballState tb = update_trackball();
    if (tb.x == 0 && tb.y == 0 && !tb.clicked) return;

    if (currentLevel == 0) {
        if (tb.clicked) {
            if (selectedCategory != -1) openFolder(selectedCategory);
            return;
        }
        if (selectedCategory == -1) {
            selectedCategory = 0; drawCategoryGrid(); return;
        }
        int col = selectedCategory % 3;
        int row = selectedCategory / 3;
        int maxRow = (NUM_CATEGORIES - 1) / 3;
        if      (tb.x ==  1 && col < 2) col++;
        else if (tb.x == -1 && col > 0) col--;
        else if (tb.y ==  1 && row < maxRow) row++;
        else if (tb.y == -1 && row > 0) row--;
        int ns = row * 3 + col;
        if (ns < NUM_CATEGORIES && ns != selectedCategory) {
            selectedCategory = ns; drawCategoryGrid();
        }
    } else {
        const Category& cat = categories[openCategory];
        int totalPages = (cat.appCount + 5) / 6;
        int start      = appPage * 6;

        if (tb.x == -1) { closeFolder(); return; }

        if (tb.clicked) {
            if (selectedApp != -1) launchApp(cat.apps[selectedApp].id);
            return;
        }

        if (selectedApp == -1) {
            selectedApp = start; drawAppGrid(); return;
        }

        int local = selectedApp - start;
        int col = local % 3, row = local / 3;

        if (tb.x == 1) {
            col++;
            if (col >= 3) {
                if (appPage < totalPages - 1) {
                    appPage++; selectedApp = appPage * 6; drawAppUI();
                }
                return;
            }
        } else if (tb.y ==  1 && row < 1) row++;
        else if   (tb.y == -1 && row > 0) row--;

        int nlocal = row * 3 + col;
        int napp   = start + nlocal;
        if (napp < cat.appCount && napp != selectedApp) {
            selectedApp = napp; drawAppGrid();
        }
    }
}

// ─────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────
void run_launcher() {
    init_trackball();
    currentLevel = 0; openCategory = -1;
    selectedCategory = -1; selectedApp = -1; appPage = 0;
    drawCategoryUI();

    unsigned long lastStatus = millis();

    while (true) {
        handleTouch();
        handleTrackball();
        if (millis() - lastStatus > 3000) {
            updateStatusBar();
            lastStatus = millis();
        }
        delay(50);
    }
}