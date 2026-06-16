// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  PISCES MOON OS — C28P BOOT + SPLASH + LAUNCHER
//
//  First-boot validation for the LCDwiki 2.8" ESP32-S3 Display
//  (C28P variant). Native portrait 240x320. Touch-only input.
//  Audio I/O via ES8311 codec. No keyboard. No LoRa. No GPS.
//
//  This file contains everything C28P-specific in one place to
//  keep the first-boot bring-up readable. The split between this
//  file and main.cpp / launcher.cpp follows the same pattern as
//  launcher_cardputer.cpp — a per-device file that the main
//  branches can delegate to.
//
//  BRING-UP CHECKLIST (proven by this file at first boot):
//    1. Serial output appears (USB-CDC handshake completes)
//    2. SPI bus initializes on PIN_LCD_* pins
//    3. ILI9341V responds and accepts commands
//    4. Backlight lights up
//    5. Splash renders without panic / WDT timeout
//    6. I2C bus comes up on PIN_I2C_SDA / PIN_I2C_SCL
//    7. FT6336G touch IC responds to I2C probe
//    8. SDIO bus detects MicroSD card (if inserted)
//    9. WiFi radio initializes
//   10. Touch events flow into the launcher
//   11. Tile taps register and report to serial
//
//  Anything that fails at this stage points at a hardware/pinout
//  issue. The serial log narrates each step so a failed step is
//  identifiable from console output alone, without on-screen UI.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <WiFi.h>
#include <SPI.h>
#include <esp_task_wdt.h>
#include "hal_pins.h"
#include "spi_treaty.h"
#include "pm_capabilities.h"
#include "tetris.h"
#include "snake.h"
#include "pacman.h"
#include "galaga.h"
#include "pole_position.h"
#include "mario_bros.h"
#include "donkey_kong.h"
#include "breakout.h"
#include "game_2048.h"
#include "minesweeper.h"
#include "connect4.h"
#include "simon.h"
#include "solitaire.h"
#include "asteroids.h"
#include "space_invaders.h"
#include "frogger.h"
#include "chess.h"
#include "ereader.h"
#include "data_reader.h"
#include "pm_clock.h"
#include "pm_tracker_scan.h"
#include "pm_notes.h"
#include "pm_contacts.h"
#include "pm_calendar.h"
#include "pm_calculator.h"
#include "pm_units.h"

// ─────────────────────────────────────────────
//  gfx LIVES IN main.cpp
//
//  Like every other device, the C28P's gfx pointer is
//  defined inside main.cpp's driver-instantiation block.
//  This keeps display ownership consistent across devices —
//  main.cpp is the single place that defines gfx for every
//  target. We just declare extern here.
// ─────────────────────────────────────────────
extern Arduino_GFX *gfx;
extern SemaphoreHandle_t spi_mutex;

// ─────────────────────────────────────────────
//  TOUCH — FT6336G via I2C (address 0x38)
//
//  The FT6336G is the capacitive controller on the C28P. It
//  reports up to two simultaneous touch points. For first-boot
//  validation we only need single-touch coordinates.
//
//  Register map (per FT6x36 datasheet, confirmed against
//  community-published C28P references):
//    0x00  Device mode
//    0x02  Number of touch points
//    0x03  Touch1 XH (bits 7-6 = event, bits 3-0 = X high nibble)
//    0x04  Touch1 XL (X low byte)
//    0x05  Touch1 YH (bits 7-6 = ID, bits 3-0 = Y high nibble)
//    0x06  Touch1 YL (Y low byte)
// ─────────────────────────────────────────────

#define FT6336G_I2C_ADDR  0x38

static bool c28p_touch_present = false;

static bool c28p_touch_init() {
    pinMode(PIN_TOUCH_RST, OUTPUT);
    digitalWrite(PIN_TOUCH_RST, LOW);
    delay(10);
    digitalWrite(PIN_TOUCH_RST, HIGH);
    delay(50);

    // I2C is already initialized by main setup
    Wire.beginTransmission(FT6336G_I2C_ADDR);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Serial.printf("[C28P] Touch I2C probe failed (err=%d) — wrong "
                      "address or wiring\n", err);
        c28p_touch_present = false;
        return false;
    }

    // Read device-mode register as a final sanity check
    Wire.beginTransmission(FT6336G_I2C_ADDR);
    Wire.write(0x00);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)FT6336G_I2C_ADDR, (uint8_t)1);
    if (Wire.available()) {
        uint8_t mode = Wire.read();
        Serial.printf("[C28P] FT6336G touch OK. Mode reg=0x%02X\n", mode);
        c28p_touch_present = true;
        return true;
    }
    Serial.println("[C28P] FT6336G probe responded but mode read failed");
    c28p_touch_present = false;
    return false;
}

// Read a single touch point. Returns true if a finger is present.
// Coordinates are in display (240x320 portrait) space.
// Non-static so c28p_dpad.cpp (and future C28P apps) can read touch
// directly without going through c28p_boot.cpp.
bool c28p_touch_read(int16_t* x, int16_t* y) {
    if (!c28p_touch_present) return false;

    Wire.beginTransmission(FT6336G_I2C_ADDR);
    Wire.write(0x02);   // Start at "number of touches"
    if (Wire.endTransmission(false) != 0) return false;

    Wire.requestFrom((uint8_t)FT6336G_I2C_ADDR, (uint8_t)5);
    if (Wire.available() < 5) return false;

    uint8_t num    = Wire.read();
    uint8_t xh     = Wire.read();
    uint8_t xl     = Wire.read();
    uint8_t yh     = Wire.read();
    uint8_t yl     = Wire.read();

    if ((num & 0x0F) == 0) return false;

    uint16_t rx = ((uint16_t)(xh & 0x0F) << 8) | xl;
    uint16_t ry = ((uint16_t)(yh & 0x0F) << 8) | yl;

    // FT6336G reports in panel orientation; on the C28P this
    // corresponds directly to the 240x320 portrait coordinate
    // system. If a future C28P variant ships the panel rotated,
    // coordinate-swap or mirror here.
    if (rx >= SCREEN_W) rx = SCREEN_W - 1;
    if (ry >= SCREEN_H) ry = SCREEN_H - 1;

    *x = (int16_t)rx;
    *y = (int16_t)ry;
    return true;
}

// Read up to two simultaneous touch points. Returns the number of
// active touches (0, 1, or 2); fills xs[i]/ys[i] for each active
// point in display (240x320 portrait) coordinates. xs/ys must hold
// at least 2 entries.
//
// The FT6336G exposes touch 1 at registers 0x03-0x06 and touch 2 at
// 0x09-0x0C, with the active-touch count at 0x02. A single 11-byte
// burst read from 0x02 captures the count plus both points. This is
// what makes "hold a direction + press A" possible on the virtual
// D-pad — c28p_touch_read() (single point) physically can't report
// two fingers, so games that need move+action (Mario jump-while-
// running, Galaga fire-while-moving) poll this instead.
int c28p_touch_read_multi(int16_t* xs, int16_t* ys) {
    if (!c28p_touch_present) return 0;

    Wire.beginTransmission(FT6336G_I2C_ADDR);
    Wire.write(0x02);   // start at "number of touches"
    if (Wire.endTransmission(false) != 0) return 0;

    // 11 bytes: 0x02 (count) .. 0x0C (touch-2 Y low)
    Wire.requestFrom((uint8_t)FT6336G_I2C_ADDR, (uint8_t)11);
    if (Wire.available() < 11) return 0;

    uint8_t b[11];
    for (int i = 0; i < 11; i++) b[i] = Wire.read();

    int num = b[0] & 0x0F;
    if (num <= 0) return 0;
    if (num > 2) num = 2;

    // Touch 1 → b[1..4] (regs 0x03-0x06)
    // Touch 2 → b[7..10] (regs 0x09-0x0C)
    //
    // Each touch slot's XH byte (b[base]) encodes the event type in
    // its top two bits (7-6):
    //   00 = Press Down
    //   01 = Lift Up
    //   10 = Contact (held)
    //   11 = reserved
    // A "Lift Up" slot still carries the last coordinates of the finger
    // that just released. The FT6336G reports it for one frame past the
    // actual release as part of its event protocol. Treating that as an
    // active touch means the game sees a phantom press for one frame
    // every time a finger lifts — which during Mario's jump-while-running
    // looks like a stray direction nudge at the exact wrong moment.
    // Filter those slots out so only Press Down + Contact (currently in
    // contact with the panel) get reported back.
    int out = 0;
    for (int p = 0; p < num; p++) {
        int base = (p == 0) ? 1 : 7;
        uint8_t event = (b[base] >> 6) & 0x03;
        if (event == 1) continue;   // Lift Up — slot is stale, skip
        uint16_t rx = ((uint16_t)(b[base]     & 0x0F) << 8) | b[base + 1];
        uint16_t ry = ((uint16_t)(b[base + 2] & 0x0F) << 8) | b[base + 3];
        if (rx >= SCREEN_W) rx = SCREEN_W - 1;
        if (ry >= SCREEN_H) ry = SCREEN_H - 1;
        xs[out] = (int16_t)rx;
        ys[out] = (int16_t)ry;
        out++;
    }
    return out;
}

// ─────────────────────────────────────────────
//  HARDWARE PROBE — reports presence/absence to serial
//
//  Run after display init but before splash. If anything fails
//  here, the serial log says exactly what so you can isolate the
//  failure to a single subsystem without guessing.
// ─────────────────────────────────────────────

static void c28p_probe_hardware() {
    Serial.println("[C28P] === HARDWARE PROBE ===");
    Serial.printf("[C28P] Device:        %s (%s)\n",
                  pm_device_caps.device_label, pm_device_caps.device_id);
    Serial.printf("[C28P] Display:       %dx%d (portrait), ILI9341V\n",
                  SCREEN_W, SCREEN_H);
    Serial.printf("[C28P] PSRAM:         %d MB\n",
                  pm_device_caps.psram_mb);
    Serial.printf("[C28P] Form factor:   desk kiosk (USB-powered)\n");

    // I2C bus scan — useful for first-boot to see what's actually
    // sitting on the bus. We expect at minimum the touch controller
    // (0x38) and the audio codec (0x18 for ES8311).
    Serial.println("[C28P] I2C bus scan:");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            const char* label = "";
            if (addr == 0x18) label = " (ES8311 audio codec)";
            else if (addr == 0x38) label = " (FT6336G touch)";
            Serial.printf("[C28P]   0x%02X%s\n", addr, label);
            found++;
        }
    }
    Serial.printf("[C28P] I2C devices found: %d\n", found);

    // Touch init
    if (c28p_touch_init()) {
        Serial.println("[C28P] Touch:        OK");
    } else {
        Serial.println("[C28P] Touch:        MISSING (check FT6336G wiring)");
    }

    Serial.println("[C28P] === PROBE COMPLETE ===");
}

// ─────────────────────────────────────────────
//  SETUP HOOK — called from main.cpp setup()
//
//  This replaces the device-specific setup block that T-Deck and
//  Cardputer have inline in main.cpp. Keeping it here keeps
//  main.cpp readable and lets C28P bring-up changes happen in
//  one file.
//
//  Responsibilities (split with main.cpp):
//    main.cpp owns:  Serial, SPI mutex, SPI.begin, Wire.begin,
//                    backlight pin, gfx->begin(), display fill
//    c28p_setup():   I2C bus scan, touch controller probe + init,
//                    serial narration of what's present on the bus
//
//  c28p_setup() runs AFTER main.cpp's display init, so SPI / Wire /
//  backlight are already up. We just probe and report.
// ─────────────────────────────────────────────

void c28p_setup() {
    Serial.println("[C28P] setup() — peripheral probe entering");

    // Probe what's actually on the I2C bus and report. Touch init
    // happens inside the probe. SPI and I2C buses are already up
    // (main.cpp's display init block did that).
    c28p_probe_hardware();

    Serial.println("[C28P] setup() — peripheral probe done");
}

// ─────────────────────────────────────────────
//  SPLASH — portrait-tuned, native 240x320
//
//  The Cardputer splash uses a 240-wide compressed vertical layout
//  to fit 135px of height. The C28P has the opposite problem —
//  generous height, narrow width. The layout exploits portrait:
//  chip icon at top, title centered vertically, version footer at
//  bottom, all with breathing room between.
// ─────────────────────────────────────────────

extern void drawChipIcon(int cx, int cy);
extern void drawCircuitBackground();
extern void drawOctagonFrame();

void c28p_splash() {
    drawCircuitBackground();
    delay(150);
    drawOctagonFrame();
    delay(150);

    const int CX = SCREEN_W / 2;   // 120

    // Chip icon top-third
    drawChipIcon(CX, 60);
    delay(200);

    const char* title = "Pisces Moon.";
    const char* line1 = "Powered by Gemini.";
    const char* line2 = "Limited only by your";
    const char* line3 = "imagination.";   // wrapped — narrow display
    uint16_t spectrum[] = {
        0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF,
    };
    int specLen = 8;

    int titleY = 140;
    int titleCharW = 18;   // size-3 text ~18px wide per char
    int titleX = (SCREEN_W - (int)strlen(title) * titleCharW) / 2;
    if (titleX < 4) titleX = 4;

    for (int cycle = 0; cycle < 12; cycle++) {
        gfx->setCursor(titleX, titleY);
        gfx->setTextSize(3);
        for (int i = 0; i < (int)strlen(title); i++) {
            gfx->setTextColor(spectrum[(i + cycle) % specLen]);
            gfx->print(title[i]);
        }
        delay(80);
    }

    gfx->setCursor(titleX, titleY);
    gfx->setTextSize(3);
    uint16_t finalColors[] = {
        0xF81F, 0x001F, 0x07FF, 0x07E0, 0xFFE0, 0xFD20,
        0xFFFF, 0xF800, 0xFD20, 0x07E0, 0x07FF, 0xFFFF,
    };
    for (int i = 0; i < (int)strlen(title); i++) {
        gfx->setTextColor(finalColors[i]);
        gfx->print(title[i]);
    }

    delay(300);
    gfx->setTextSize(1);

    int l1x = (SCREEN_W - (int)strlen(line1) * 6) / 2;
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(l1x, 200);
    gfx->print(line1);
    delay(400);

    int l2x = (SCREEN_W - (int)strlen(line2) * 6) / 2;
    gfx->setTextColor(0xC618);
    gfx->setCursor(l2x, 224);
    gfx->print(line2);

    int l3x = (SCREEN_W - (int)strlen(line3) * 6) / 2;
    gfx->setCursor(l3x, 240);
    gfx->print(line3);
    delay(400);

    gfx->setTextColor(0x0480);
    const char* version = "v1.2.1 MULTI-DEVICE";
    int vx = (SCREEN_W - (int)strlen(version) * 6) / 2;
    gfx->setCursor(vx, 292);
    gfx->print(version);

    esp_task_wdt_reset();
    delay(2500);
    esp_task_wdt_reset();
}

// ─────────────────────────────────────────────
//  LAUNCHER — portrait touch grid
//
//  240x320 portrait with no keyboard. Two columns × four rows of
//  large touch tiles (110×60 each, with 5px gutter and 24px header)
//  showing the categories Pisces Moon supports on this hardware.
//  Tapping a category opens an app-grid for that category.
//
//  This is a FIRST-BOOT MINIMAL LAUNCHER. It does NOT yet do
//  capability-filtered app lists, ELF loading, or any app dispatch.
//  Tapping a tile logs which one was tapped and returns to the
//  category screen. The point is to prove touch + UI works.
//
//  Once first-boot is confirmed, this file gets upgraded to a
//  proper launcher with app dispatch, app pages, and the
//  capability descriptor doing visibility filtering.
// ─────────────────────────────────────────────

struct C28PTile {
    const char* label;
    uint16_t    color;
    uint16_t    color_dim;
};

// 12-slot 3×4 grid layout:
//   Row 0: SYSTEM    CYBER      AI
//   Row 1: UTILITIES PERSONAL   REFERENCE
//   Row 2: MEDIA     WEATHER    [SOON]
//   Row 3: ARCADE    PUZZLES    TETRIS★ (hero/direct-launch)
//
// Jen loves Tetris. Tetris is the feature app.
static const C28PTile C28P_CATEGORIES[] = {
    { "SYSTEM",    0x8410, 0x2104 },  //  0 silver
    { "CYBER",     0xF400, 0x2000 },  //  1 red-orange
    { "AI",        0x9818, 0x2004 },  //  2 purple
    { "UTILITIES", 0xFD20, 0x3208 },  //  3 amber
    { "PERSONAL",  0xF81F, 0x3008 },  //  4 magenta
    { "REFERENCE", 0x07FF, 0x021F },  //  5 cyan
    { "MEDIA",     0xF81F, 0x3008 },  //  6 magenta
    { "WEATHER",   0xFFE0, 0x3300 },  //  7 yellow
    { "CLASSICS",  0xC618, 0x4208 },  //  8 warm white — Solitaire + Minesweeper
    { "ARCADE",    0x07E0, 0x0220 },  //  9 green
    { "PUZZLES",   0x0400, 0x0100 },  // 10 dark green
    { "TETRIS",    0x07FF, 0x021F },  // 11 HERO tile — cyan, direct launch
};
static const int C28P_NUM_CATEGORIES = 12;

// Tile layout: 2 columns, 4 rows. Computed once at startup.
struct TileRect { int x, y, w, h; };
static TileRect c28p_tile_rects[C28P_NUM_CATEGORIES];

static void c28p_compute_layout() {
    const int header_h = 28;
    const int footer_h = 20;
    const int margin_x = 8;
    const int gutter_x = 6;
    const int gutter_y = 6;

    int cols = 3;   // 3×4 grid for 12 categories
    int rows = 4;

    int avail_w = SCREEN_W - 2 * margin_x;
    int avail_h = SCREEN_H - header_h - footer_h;
    int tile_w  = (avail_w  - (cols - 1) * gutter_x) / cols;
    int tile_h  = (avail_h  - (rows - 1) * gutter_y) / rows;

    for (int i = 0; i < C28P_NUM_CATEGORIES; i++) {
        int col = i % cols;
        int row = i / cols;
        c28p_tile_rects[i].x = margin_x + col * (tile_w + gutter_x);
        c28p_tile_rects[i].y = header_h + row * (tile_h + gutter_y);
        c28p_tile_rects[i].w = tile_w;
        c28p_tile_rects[i].h = tile_h;
    }
}

static void c28p_draw_header() {
    gfx->fillRect(0, 0, SCREEN_W, 28, 0x0841);
    gfx->setTextColor(0x07E0);   // green
    gfx->setTextSize(1);
    gfx->setCursor(8, 6);
    gfx->print("PISCES MOON");
    gfx->setTextColor(0x8410);
    gfx->setCursor(8, 16);
    gfx->print("v1.2.1");

    // WiFi status indicator on the right
    gfx->setTextColor(WiFi.status() == WL_CONNECTED ? 0x07E0 : 0x4208);
    const char* wifi = (WiFi.status() == WL_CONNECTED) ? "WIFI:ON" : "WIFI:--";
    gfx->setCursor(SCREEN_W - (int)strlen(wifi) * 6 - 8, 6);
    gfx->print(wifi);
}

static void c28p_draw_footer() {
    int fy = SCREEN_H - 16;
    gfx->fillRect(0, fy, SCREEN_W, 16, 0x0841);
    gfx->setTextColor(0x4208);
    gfx->setTextSize(1);
    gfx->setCursor(8, fy + 4);
    gfx->print("Tap a tile");

    // Time placeholder — real clock comes from time_service in
    // a later pass once C28P boot is verified.
    const char* now = "--:--";
    gfx->setCursor(SCREEN_W - (int)strlen(now) * 6 - 8, fy + 4);
    gfx->print(now);
}

static void c28p_draw_tile(int idx, bool pressed) {
    const TileRect& r = c28p_tile_rects[idx];
    const C28PTile& t = C28P_CATEGORIES[idx];

    // Slot 11 — TETRIS hero tile, styled in Tetris cyan
    if (idx == 11) {
        uint16_t acc = 0x07FF;   // Tetris cyan
        uint16_t bg  = pressed ? acc : 0x0414;
        gfx->fillRoundRect(r.x, r.y, r.w, r.h, 5, bg);
        // Pulsing border: alternates white/cyan
        uint16_t border = ((millis() / 350) % 2) ? 0xFFFF : acc;
        gfx->drawRoundRect(r.x,   r.y,   r.w,   r.h,   5, border);
        gfx->drawRoundRect(r.x+1, r.y+1, r.w-2, r.h-2, 4, border);
        // "TETRIS" label
        gfx->setTextSize(1);
        gfx->setTextColor(pressed ? 0x0000 : acc);
        int lw = 6 * 6;   // "TETRIS" = 6 chars
        gfx->setCursor(r.x + (r.w - lw)/2, r.y + r.h/2 - 10);
        gfx->print("TETRIS");
        // "> PLAY" sub-label in amber
        gfx->setTextColor(pressed ? 0x0000 : 0xFD20);
        int pw = 6 * 6;   // "> PLAY" = 6 chars
        gfx->setCursor(r.x + (r.w - pw)/2, r.y + r.h/2 + 2);
        gfx->print("> PLAY");
        return;
    }

    // Standard tile
    uint16_t bg = pressed ? t.color : t.color_dim;
    uint16_t fg = pressed ? 0x0000 : t.color;
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 5, bg);
    gfx->drawRoundRect(r.x, r.y, r.w, r.h, 5, t.color);
    gfx->setTextSize(1);
    gfx->setTextColor(fg);
    // Wrap long names at size-1 (6px/char): tile_w≈70px = ~11 chars
    int text_w = (int)strlen(t.label) * 6;
    int text_x = r.x + (r.w - text_w) / 2;
    int text_y = r.y + (r.h - 8) / 2;
    gfx->setCursor(text_x, text_y);
    gfx->print(t.label);
}

static void c28p_draw_all_tiles() {
    for (int i = 0; i < C28P_NUM_CATEGORIES; i++) {
        c28p_draw_tile(i, false);
    }
}

static int c28p_hit_test(int16_t tx, int16_t ty) {
    for (int i = 0; i < C28P_NUM_CATEGORIES; i++) {
        const TileRect& r = c28p_tile_rects[i];
        if (tx >= r.x && tx < r.x + r.w &&
            ty >= r.y && ty < r.y + r.h) {
            return i;
        }
    }
    return -1;
}

// ─────────────────────────────────────────────
//  SUB-LAUNCHER
//
//  Generic vertical-list picker for second-level app menus.
//  Each category (GAMES, TOOLS, AUDIO, etc.) calls this with
//  its own item list. Tapping an item invokes its action.
//  Tapping the "BACK" row at the top returns to the main grid.
//
//  Layout: 240×320 with title bar (40px), back button (32px),
//  N item buttons (44px each, max 6 fits). Below 320 is wasted
//  but consistent with main launcher visual language.
// ─────────────────────────────────────────────
struct C28PSubItem {
    const char* label;
    void (*action)();
};

extern bool c28p_touch_read(int16_t* x, int16_t* y);

static void c28p_sub_launcher(const char* title,
                              const C28PSubItem* items, int count) {
    const int title_h     = 40;
    const int back_y      = 44;
    const int back_h      = 32;
    const int item_y0     = 84;
    const int item_h      = 40;
    const int item_visible = 4;   // 4 rows: y=84,124,164,204 (each 36 tall,
                                  // bottom of row 3 = 240), clear of the
                                  // nav row at 286. Lists longer than 4
                                  // (GAMES=5, TOOLS=7) paginate with PREV/NEXT.
    const int margin      = 12;
    const int nav_y       = 286;  // PREV/NEXT row
    const int nav_h       = 32;

    int page = 0;
    int pages = (count + item_visible - 1) / item_visible;

    auto draw_chrome = [&]() {
        gfx->fillScreen(0x0000);
        // Title with page indicator if multi-page
        gfx->setTextSize(2);
        gfx->setTextColor(0x07FF);
        gfx->setCursor(margin, 10);
        gfx->print(title);
        if (pages > 1) {
            gfx->setTextSize(1);
            gfx->setTextColor(0x8410);
            gfx->setCursor(180, 16);
            gfx->printf("%d/%d", page + 1, pages);
        }
        gfx->drawFastHLine(0, title_h - 4, 240, 0x4208);
        // Back button
        gfx->fillRect(margin, back_y, 240 - 2 * margin, back_h, 0x18C3);
        gfx->drawRect(margin, back_y, 240 - 2 * margin, back_h, 0x8410);
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(margin + 12, back_y + 12);
        gfx->print("< BACK");
        // Items (current page)
        int start = page * item_visible;
        int end = min(count, start + item_visible);
        for (int i = start; i < end; i++) {
            int row = i - start;
            int y = item_y0 + row * item_h;
            gfx->fillRect(margin, y, 240 - 2 * margin, item_h - 4, 0x2104);
            gfx->drawRect(margin, y, 240 - 2 * margin, item_h - 4, 0x4208);
            gfx->setTextSize(2);
            gfx->setTextColor(0xFFE0);
            gfx->setCursor(margin + 14, y + 10);
            gfx->print(items[i].label);
        }
        // Page navigation (only if multi-page)
        if (pages > 1) {
            // PREV button
            uint16_t prev_color = (page > 0) ? 0xFFE0 : 0x4208;
            gfx->fillRect(margin, nav_y, 100, nav_h, 0x18C3);
            gfx->drawRect(margin, nav_y, 100, nav_h, prev_color);
            gfx->setTextSize(1);
            gfx->setTextColor(prev_color);
            gfx->setCursor(margin + 36, nav_y + 12);
            gfx->print("PREV");
            // NEXT button
            uint16_t next_color = (page < pages - 1) ? 0xFFE0 : 0x4208;
            gfx->fillRect(240 - margin - 100, nav_y, 100, nav_h, 0x18C3);
            gfx->drawRect(240 - margin - 100, nav_y, 100, nav_h, next_color);
            gfx->setTextColor(next_color);
            gfx->setCursor(240 - margin - 100 + 36, nav_y + 12);
            gfx->print("NEXT");
        }
    };

    draw_chrome();

    bool was_touched = false;
    int pressed_idx = -2;   // -2 = none, -1 = back, -3 = prev, -4 = next, 0..n-1 = item
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            // Touch began
            if (ty >= back_y && ty < back_y + back_h &&
                tx >= margin && tx < 240 - margin) {
                pressed_idx = -1;
            }
            else if (pages > 1 && ty >= nav_y && ty < nav_y + nav_h) {
                if (tx >= margin && tx < margin + 100 && page > 0) {
                    pressed_idx = -3;
                } else if (tx >= 240 - margin - 100 && tx < 240 - margin &&
                           page < pages - 1) {
                    pressed_idx = -4;
                }
            }
            else if (tx >= margin && tx < 240 - margin) {
                int start = page * item_visible;
                int end = min(count, start + item_visible);
                for (int i = start; i < end; i++) {
                    int row = i - start;
                    int y = item_y0 + row * item_h;
                    if (ty >= y && ty < y + item_h - 4) {
                        pressed_idx = i;
                        break;
                    }
                }
            }
        } else if (!touched && was_touched) {
            // Touch released — dispatch
            if (pressed_idx == -1) {
                return;   // BACK
            } else if (pressed_idx == -3) {
                if (page > 0) { page--; draw_chrome(); }
            } else if (pressed_idx == -4) {
                if (page < pages - 1) { page++; draw_chrome(); }
            } else if (pressed_idx >= 0 && pressed_idx < count) {
                items[pressed_idx].action();
                draw_chrome();   // repaint after the app returns
            }
            pressed_idx = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  COMING SOON
//
//  Generic "this category is on the v1.1 roadmap" screen used
//  for WEATHER, RSS, MEDIA tiles. Honest about what's not yet
//  shipped rather than empty no-op behavior.
// ─────────────────────────────────────────────
static void c28p_coming_soon(const char* category) {
    gfx->fillScreen(0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFC00);   // orange
    int title_w = strlen(category) * 12;
    gfx->setCursor((240 - title_w) / 2, 100);
    gfx->print(category);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(60, 140);
    gfx->print("Coming in v1.1");
    gfx->setTextColor(0x4208);
    gfx->setCursor(40, 250);
    gfx->print("Tap anywhere to return");

    // Wait for any touch
    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            return;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  APP WRAPPERS
//
//  Each category gets its own sub-launcher invocation here.
//  This keeps the dispatch block in c28p_launcher() readable —
//  it just calls c28p_open_games(), c28p_open_tools(), etc.
// ─────────────────────────────────────────────
extern void c28p_run_audio_player();   // forward decl, audio file player
extern void c28p_run_ai_terminal();    // forward decl, Gemini voice UI
extern void c28p_run_wardrive();       // forward decl, stationary wardrive
extern void c28p_run_wifi_setup();     // forward decl, WiFi connection UI
extern void c28p_run_about();          // forward decl, about/version screen
extern void c28p_run_weather();        // forward decl, Open-Meteo weather
extern void c28p_run_rss();            // forward decl, RSS reader
extern void c28p_run_media();          // forward decl, voice recorder + library
extern void c28p_run_system();         // forward decl, real settings screen
extern void run_c28p_mic_probe();      // forward decl, mic capture diagnostic
extern void run_wifi_filemgr();        // forward decl, WiFi SD file server

static void c28p_open_games() {
    static const C28PSubItem items[] = {
        { "TETRIS",     run_tetris },
        { "SNAKE",      run_snake },
        { "PAC-MAN",    run_pacman },
        { "GALAGA",     run_galaga },
        { "POLE POS",   run_pole_position },
        { "MARIO BROS", run_mario_bros },
        { "BREAKOUT",   run_breakout },
        { "2048",       run_2048 },
        { "MINESWEEPER",run_minesweeper },
        { "CONNECT 4",  run_connect4 },
        { "SIMON",      run_simon },
        { "SOLITAIRE",  run_solitaire },
        { "ASTEROIDS",  run_asteroids },
        { "INVADERS",   run_space_invaders },
        { "FROGGER",    run_frogger },
        { "CHESS",      run_chess },
    };
    c28p_sub_launcher("GAMES", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_run_survival() { run_data_reader("survival", "SURVIVAL"); }
static void c28p_run_medical()  { run_data_reader("medical",  "MEDICAL");  }
static void c28p_run_history()  { run_data_reader("history",  "HISTORY");  }

static void c28p_open_arcade() {
    static const C28PSubItem items[] = {
        { "PAC-MAN",    run_pacman },
        { "GALAGA",     run_galaga },
        { "MARIO BROS", run_mario_bros },
        { "DONKEY KONG",run_donkey_kong },
        { "BREAKOUT",   run_breakout },
        { "ASTEROIDS",  run_asteroids },
        { "INVADERS",   run_space_invaders },
        { "FROGGER",    run_frogger },
        { "POLE POS",   run_pole_position },
        { "SNAKE",      run_snake },
    };
    c28p_sub_launcher("ARCADE", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_puzzles() {
    static const C28PSubItem items[] = {
        { "2048",       run_2048 },
        { "MINESWEEPER",run_minesweeper },
        { "CONNECT 4",  run_connect4 },
        { "SIMON",      run_simon },
        { "SOLITAIRE",  run_solitaire },
        { "CHESS",      run_chess },
    };
    c28p_sub_launcher("PUZZLES", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_utilities() {
    static const C28PSubItem items[] = {
        { "CLOCK",     pm_run_clock },
        { "TIMER",     pm_run_timer },
        { "STOPWATCH", pm_run_stopwatch },
        { "CALC",      pm_run_calculator },
        { "UNITS",     pm_run_units },
    };
    c28p_sub_launcher("UTILITIES", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_personal() {
    static const C28PSubItem items[] = {
        { "NOTES",    pm_run_notes },
        { "CONTACTS", pm_run_contacts },
        { "CALENDAR", pm_run_calendar },
    };
    c28p_sub_launcher("PERSONAL", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_reference() {
    static const C28PSubItem items[] = {
        { "SURVIVAL",  c28p_run_survival },
        { "E-READER",  run_ereader },
        { "MEDICAL",   c28p_run_medical },
        { "HISTORY",   c28p_run_history },
        { "RSS",       c28p_run_rss },
    };
    c28p_sub_launcher("REFERENCE", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_cyber() {
    static const C28PSubItem items[] = {
        { "WARDRIVE",  c28p_run_wardrive },
        { "TRACKERS",  pm_run_tracker_scan },
        { "MIC TEST",  run_c28p_mic_probe },
    };
    c28p_sub_launcher("CYBER", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_system() {
    static const C28PSubItem items[] = {
        { "WIFI",      c28p_run_wifi_setup },
        { "SD FILES",  run_wifi_filemgr },
        { "ABOUT",     c28p_run_about },
        { "SYSTEM",    c28p_run_system },
    };
    c28p_sub_launcher("SYSTEM", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_media() {
    static const C28PSubItem items[] = {
        { "PLAYER",   c28p_run_audio_player },
        { "RECORDER", c28p_run_media },
    };
    c28p_sub_launcher("MEDIA", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_classics() {
    static const C28PSubItem items[] = {
        { "SOLITAIRE",   run_solitaire },
        { "MINESWEEPER", run_minesweeper },
    };
    c28p_sub_launcher("CLASSICS", items, sizeof(items) / sizeof(items[0]));
}

static void c28p_open_ai() {
    static const C28PSubItem items[] = {
        { "VOICE AI",  c28p_run_ai_terminal },
    };
    c28p_sub_launcher("AI", items, sizeof(items) / sizeof(items[0]));
}

void c28p_launcher() {
    Serial.println("[C28P] Launcher starting");
    c28p_compute_layout();

    gfx->fillScreen(0x0000);
    c28p_draw_header();
    c28p_draw_footer();
    c28p_draw_all_tiles();

    int pressed_idx = -1;
    unsigned long last_status = millis();

    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);

        if (touched) {
            int idx = c28p_hit_test(tx, ty);
            if (idx >= 0 && idx != pressed_idx) {
                // Visual press feedback
                if (pressed_idx >= 0) c28p_draw_tile(pressed_idx, false);
                c28p_draw_tile(idx, true);
                pressed_idx = idx;
                Serial.printf("[C28P] Tile pressed: %s (idx=%d)\n",
                              C28P_CATEGORIES[idx].label, idx);
            }
        } else if (pressed_idx >= 0) {
            // Touch released — dispatch to the selected category
            int dispatched = pressed_idx;
            Serial.printf("[C28P] Tile released: %s\n",
                          C28P_CATEGORIES[dispatched].label);
            c28p_draw_tile(pressed_idx, false);
            pressed_idx = -1;

            // ── App dispatch ──
            // Slot 8  = SOON placeholder (no-op)
            // Slot 11 = TETRIS hero (direct launch)
            // All other slots open a sub-launcher.
            switch (dispatched) {
                case  0: c28p_open_system();    break;
                case  1: c28p_open_cyber();     break;
                case  2: c28p_open_ai();        break;
                case  3: c28p_open_utilities(); break;
                case  4: c28p_open_personal();  break;
                case  5: c28p_open_reference(); break;
                case  6: c28p_open_media();     break;
                case  7: c28p_run_weather();    break;  // Weather: direct
                case  8: c28p_open_classics(); break;
                case  9: c28p_open_arcade();    break;
                case 10: c28p_open_puzzles();   break;
                case 11: run_tetris();           break;  // Tetris: direct hero
            }
            // Apps return — repaint the launcher chrome.
            gfx->fillScreen(0x0000);
            c28p_draw_header();
            c28p_draw_footer();
            c28p_draw_all_tiles();
            last_status = millis();
            Serial.printf("[C28P] Returned to launcher from %s\n",
                          C28P_CATEGORIES[dispatched].label);
        }

        // Periodic status refresh — WiFi indicator, etc.
        if (millis() - last_status > 3000) {
            c28p_draw_header();
            last_status = millis();
        }

        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  C28P INPUT/AUDIO STUBS
//
//  game_input.cpp / game_audio.cpp call into keyboard, trackball,
//  gamepad, and audio subsystems unconditionally. The C28P has none
//  of those — it has touch and (eventually) ES8311 I2S audio.
//
//  Rather than gating every call site in game_input.cpp with
//  #ifndef DEVICE_C28P, we provide no-op stubs here. The game code
//  reads zero/false from all of them, the C28P-specific D-pad poll
//  then fills in the actual input state via OR.
//
//  When real C28P audio lands (ES8311 codec via I2S), the audio
//  stubs get replaced with real implementations and Tetris plays
//  Korobeiniki for real.
// ─────────────────────────────────────────────

#include "gamepad.h"

// ── Keyboard (no keyboard on C28P) ──
char get_keypress() { return 0; }

// ── Trackball (no trackball on C28P) ──
#include "trackball.h"
void init_trackball() {}
TrackballState update_trackball_game() {
    return TrackballState{0, 0, false};
}

// ── Gamepad (no BLE gamepad on C28P first-boot) ──
GamepadState g_gamepad = {};
void gamepad_init() {}
bool gamepad_poll() { return false; }
bool gamepad_pair() { return false; }
bool gamepad_is_paired() { return false; }
void gamepad_forget() {}
void gamepad_disconnect() {}

// ── Game audio: real implementation ──
// pm_game_audio_* functions live in game_audio.cpp with a
// DEVICE_C28P branch that routes through c28p_audio.cpp's
// ES8311 + I2S driver. Both files are in the C28P src_filter.

#endif // DEVICE_C28P
