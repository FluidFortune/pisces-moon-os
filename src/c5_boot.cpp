// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  PISCES MOON OS — C5 BOOT + SPLASH + LAUNCHER
//
//  First-boot bring-up for the NM-CYD-C5 (RockBase "Colorful"):
//  ESP32-C5-WROOM-1 single-core RISC-V, dual-band Wi-Fi 6,
//  2.8" ST7789 240×320 display, XPT2046 resistive touch.
//
//  Branches structurally from c28p_boot.cpp — same launcher
//  layout, same sub-menu pattern, same hero-tile Tetris — with
//  two device-specific replacements:
//
//  1. TOUCH driver: XPT2046 over SPI (not FT6336G over I2C).
//     XPT2046 is a 12-bit ADC controller addressed by 8-bit
//     command bytes (0xD0 = read X, 0x90 = read Y). Returns
//     raw ADC values 0..4095 that we map to pixel coordinates
//     via simple calibration constants. The bus is shared with
//     the LCD and SD, so every touch read takes the SPI mutex.
//
//  2. AUDIO stubs: the C5 has no I2S audio path wired in the
//     reference firmware. Stubs route through game_audio.cpp
//     the same way as on Maxine.
//
//  Everything else — the 12-tile launcher, sub-launchers, BACK
//  navigation, "coming soon" placeholders — is the C28P pattern.
//
//  BRING-UP CHECKLIST (proven by this file at first boot):
//    1. Serial output appears (USB-CDC handshake)
//    2. SPI bus initializes on GPIO 6/2/7
//    3. ST7789 responds (LCD CS=23)
//    4. Backlight lights up
//    5. Splash renders without WDT
//    6. I2C bus comes up on GPIO 9/8 (CN1)
//    7. XPT2046 responds on SPI (touch CS=1)
//    8. microSD detected (CS=10)
//    9. WiFi radio initializes (2.4 GHz first scan)
//   10. 5 GHz scan returns at least one AP (proof of life)
//   11. Touch events flow into the launcher
//   12. Tile taps register to serial
// ─────────────────────────────────────────────

#ifdef DEVICE_C5

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>   // esp_wifi_set_band_mode for 5G-only scanning
#include <SPI.h>
#include <esp_task_wdt.h>
#include "hal_pins.h"
#include "hal_c5.h"
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

extern Arduino_GFX *gfx;
extern SemaphoreHandle_t spi_mutex;

// ─────────────────────────────────────────────
//  TOUCH — XPT2046 resistive controller on shared SPI
//
//  Wire protocol (per Texas Instruments XPT2046 datasheet):
//
//    Send command byte → read 2 response bytes per axis.
//      0xD0  read X position (12-bit single-ended, power-down between)
//      0x90  read Y position
//    Response bytes hold the 12-bit value left-aligned in the high
//    bits: result = ((b0 << 8) | b1) >> 3.
//
//  Calibration:
//    Raw ADC range is 0..4095 with deadband at both ends. Typical
//    usable range is ~150..3900 on each axis. We linearly map to
//    panel pixel space (320×240 native) then optionally rotate to
//    portrait (240×320 logical) to match the rest of the fleet.
//
//    Calibration constants below are starting values from CYD
//    community defaults; tune RAW_X_MIN/MAX and RAW_Y_MIN/MAX
//    against the corners of the actual panel after first boot.
//
//  SPI bus discipline:
//    Every read takes the treaty mutex (LCD pixel pushes and SD
//    writes can be mid-flight on the same bus). We also explicitly
//    pull the LCD CS high before asserting touch CS, defending
//    against a stray LCD transaction having left its CS asserted.
// ─────────────────────────────────────────────

static bool c5_touch_present = false;

// ── Touch calibration ────────────────────────────────────────
// The XPT2046's raw ADC axes don't necessarily align with the LCD's
// display axes, and on many panels one or both are inverted relative
// to what you'd intuitively expect. Rather than carry a separate
// "rotate" flag plus min/max pairs (which can't express inversion
// cleanly), we store four numbers — the raw ADC value at each of
// the four physical edges of the display.
//
// The mapping in c5_touch_read() then becomes a single signed linear
// interpolation per axis. If the panel's X-axis ADC happens to count
// DOWN as you move right (as the NM-CYD-C5 does), RAW_X_AT_LEFT will
// be NUMERICALLY LARGER than RAW_X_AT_RIGHT — and the integer math
// handles the negative denominator correctly.
//
// Values below are from a corner-tap calibration on Eric's NM-CYD-C5
// (see [CAL] output, 2026-06-09):
//   TL: x=3533 y= 339     TR: x= 496 y= 351
//   BL: x=3592 y=3607     BR: x= 515 y=3676
// Averaged per edge to get the constants below. Other CYD-C5 units
// may need their own calibration via SYSTEM → CALIBRATE — these
// values are a reasonable starting point but every panel is slightly
// different.
static int RAW_X_AT_LEFT   = 3562;   // raw_x at pixel_x = 0
static int RAW_X_AT_RIGHT  =  505;   // raw_x at pixel_x = SCREEN_W-1
static int RAW_Y_AT_TOP    =  345;   // raw_y at pixel_y = 0
static int RAW_Y_AT_BOTTOM = 3641;   // raw_y at pixel_y = SCREEN_H-1

// Deadband — reject readings outside the usable ADC range. Pulled in
// 5% from each edge to leave headroom for inter-panel variation; the
// stability gate in c5_touch_read() filters most noise anyway.
static inline int rx_lo() { int a=RAW_X_AT_LEFT, b=RAW_X_AT_RIGHT; return (a<b?a:b) - 100; }
static inline int rx_hi() { int a=RAW_X_AT_LEFT, b=RAW_X_AT_RIGHT; return (a>b?a:b) + 100; }
static inline int ry_lo() { int a=RAW_Y_AT_TOP,  b=RAW_Y_AT_BOTTOM; return (a<b?a:b) - 100; }
static inline int ry_hi() { int a=RAW_Y_AT_TOP,  b=RAW_Y_AT_BOTTOM; return (a>b?a:b) + 100; }

// Send a single XPT2046 command and read the 12-bit response.
// CALLER must hold the SPI mutex and have CS asserted.
static uint16_t xpt_read(uint8_t cmd) {
    SPI.transfer(cmd);
    uint8_t hi = SPI.transfer(0x00);
    uint8_t lo = SPI.transfer(0x00);
    return (((uint16_t)hi << 8) | lo) >> 3;
}

static bool c5_touch_init() {
    pinMode(PIN_TOUCH_CS, OUTPUT);
    digitalWrite(PIN_TOUCH_CS, HIGH);
#if PIN_TOUCH_IRQ >= 0
    pinMode(PIN_TOUCH_IRQ, INPUT_PULLUP);
#endif
    // Sanity probe: read X with no touch present — controller is
    // present iff we get a value back (anything non-trivially-zero
    // from the SPI bus when CS is asserted).
    if (!PM_SPI_TAKE("C5_TOUCH_INIT")) {
        Serial.println("[C5] Touch init: SPI mutex timeout");
        return false;
    }
    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_TOUCH_CS, LOW);
    (void)xpt_read(XPT2046_CMD_READ_X);   // first read primes the ADC
    uint16_t probe = xpt_read(XPT2046_CMD_READ_X);
    digitalWrite(PIN_TOUCH_CS, HIGH);
    SPI.endTransaction();
    PM_SPI_GIVE();

    // Untouched XPT2046 reads typically return 0 or 4095 depending
    // on pull-up state. Either is acceptable; missing controller
    // returns garbage spread across the range. We accept anything
    // — the touch loop will validate by requiring TWO consecutive
    // reads within ±20 ADC counts of each other.
    Serial.printf("[C5] XPT2046 probe: 0x%04X\n", probe);
    c5_touch_present = true;
    return true;
}

// Read a single touch point in display coordinates. Returns true
// only when the controller reports a stable touch (two consecutive
// reads within ±20 ADC counts on each axis). The stability check
// rejects ADC noise from an untouched panel.
//
// Coordinates are in 240×320 portrait space (post-rotation) so
// caller code does not need to know the panel's native orientation.
bool c5_touch_read(int16_t* x, int16_t* y) {
    if (!c5_touch_present) return false;

    if (!PM_SPI_TAKE("C5_TOUCH_READ")) return false;

    SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_LCD_CS,   HIGH);   // defend against stale LCD CS
    digitalWrite(PIN_SD_CS,    HIGH);
    digitalWrite(PIN_TOUCH_CS, LOW);

    // Two reads per axis — first primes, second is the measurement.
    (void)xpt_read(XPT2046_CMD_READ_X);
    uint16_t x1 = xpt_read(XPT2046_CMD_READ_X);
    uint16_t y1 = xpt_read(XPT2046_CMD_READ_Y);
    uint16_t x2 = xpt_read(XPT2046_CMD_READ_X);
    uint16_t y2 = xpt_read(XPT2046_CMD_READ_Y);

    digitalWrite(PIN_TOUCH_CS, HIGH);
    SPI.endTransaction();
    PM_SPI_GIVE();

    // Stability gate: reject noise
    int dx = (int)x1 - (int)x2; if (dx < 0) dx = -dx;
    int dy = (int)y1 - (int)y2; if (dy < 0) dy = -dy;
    if (dx > 20 || dy > 20) return false;

    int raw_x = (x1 + x2) / 2;
    int raw_y = (y1 + y2) / 2;

    // Deadband — reject readings outside the usable range on either
    // axis. min/max-based bounds so inverted axes work transparently.
    if (raw_x < rx_lo() || raw_x > rx_hi()) return false;
    if (raw_y < ry_lo() || raw_y > ry_hi()) return false;

    // Signed linear interpolation — if RAW_*_AT_LEFT > RAW_*_AT_RIGHT
    // (which is the case on this panel for X), the denominator is
    // negative and the math still gives the right answer.
    long pixel_x = (long)(raw_x - RAW_X_AT_LEFT) * (long)SCREEN_W /
                   (long)(RAW_X_AT_RIGHT - RAW_X_AT_LEFT);
    long pixel_y = (long)(raw_y - RAW_Y_AT_TOP) * (long)SCREEN_H /
                   (long)(RAW_Y_AT_BOTTOM - RAW_Y_AT_TOP);

    if (pixel_x < 0)         pixel_x = 0;
    if (pixel_x >= SCREEN_W) pixel_x = SCREEN_W - 1;
    if (pixel_y < 0)         pixel_y = 0;
    if (pixel_y >= SCREEN_H) pixel_y = SCREEN_H - 1;

    *x = (int16_t)pixel_x;
    *y = (int16_t)pixel_y;
    return true;
}

// ─────────────────────────────────────────────
//  HARDWARE PROBE
// ─────────────────────────────────────────────
static void c5_probe_hardware() {
    Serial.println("[C5] === HARDWARE PROBE ===");
    Serial.printf("[C5] Device:        %s (%s)\n",
                  pm_device_caps.device_label, pm_device_caps.device_id);
    Serial.printf("[C5] Display:       %dx%d (portrait), ST7789\n",
                  SCREEN_W, SCREEN_H);
    Serial.printf("[C5] PSRAM:         %d MB\n", pm_device_caps.psram_mb);
    Serial.printf("[C5] Radios:        Wi-Fi 6 dual-band, BLE 5.3, 802.15.4\n");
    Serial.printf("[C5] SPI bus:       SCK=%d MISO=%d MOSI=%d (shared)\n",
                  PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
    Serial.printf("[C5] LCD CS=%d, Touch CS=%d, SD CS=%d\n",
                  PIN_LCD_CS, PIN_TOUCH_CS, PIN_SD_CS);

    // I2C scan on CN1
    Serial.println("[C5] I2C bus scan (CN1 extend connector):");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            Serial.printf("[C5]   0x%02X\n", addr);
            found++;
        }
    }
    Serial.printf("[C5] I2C devices found: %d\n", found);

    if (c5_touch_init()) {
        Serial.println("[C5] Touch:        OK (XPT2046)");
    } else {
        Serial.println("[C5] Touch:        MISSING");
    }

    Serial.println("[C5] === PROBE COMPLETE ===");
}

// ─────────────────────────────────────────────
//  SETUP HOOK (called from main.cpp setup())
// ─────────────────────────────────────────────
void c5_setup() {
    Serial.println("[C5] setup() — peripheral probe entering");
    c5_probe_hardware();
    Serial.println("[C5] setup() — peripheral probe done");
}

// ─────────────────────────────────────────────
//  SPLASH (portrait, mirrors c28p_splash)
// ─────────────────────────────────────────────
extern void drawChipIcon(int cx, int cy);
extern void drawCircuitBackground();
extern void drawOctagonFrame();

void c5_splash() {
    drawCircuitBackground();
    delay(150);
    drawOctagonFrame();
    delay(150);

    const int CX = SCREEN_W / 2;
    drawChipIcon(CX, 60);
    delay(200);

    const char* title = "Pisces Moon.";
    const char* line1 = "Dual-band Wi-Fi 6.";
    const char* line2 = "First in the fleet";
    const char* line3 = "to see 5 GHz.";

    uint16_t spectrum[] = {
        0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF,
    };
    int specLen = 8;

    int titleY = 140;
    int titleCharW = 18;
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
    const char* version = "v1.2.1 C5";
    int vx = (SCREEN_W - (int)strlen(version) * 6) / 2;
    gfx->setCursor(vx, 292);
    gfx->print(version);

    esp_task_wdt_reset();
    delay(2500);
    esp_task_wdt_reset();
}

// ─────────────────────────────────────────────
//  LAUNCHER — 12-tile portrait grid (matches C28P layout)
//
//  Identical to c28p_boot.cpp's launcher structure. The C5 has
//  fewer apps wired up than the C28P (no audio, no AI terminal,
//  no native weather) so some tiles open "coming soon" or are
//  hidden. The grid stays consistent across devices so muscle
//  memory between a C28P and a C5 carries over.
// ─────────────────────────────────────────────

struct C5Tile {
    const char* label;
    uint16_t    color;
    uint16_t    color_dim;
};

static const C5Tile C5_CATEGORIES[] = {
    { "SYSTEM",    0x8410, 0x2104 },
    { "CYBER",     0xF400, 0x2000 },   // wardrive + 5GHz sniffer (5G SCAN sub-item) + trackers
    { "WEATHER",   0xFFE0, 0x3300 },   // Open-Meteo, dual-band WiFi-fetched
    { "UTILITIES", 0xFD20, 0x3208 },
    { "PERSONAL",  0xF81F, 0x3008 },
    { "REFERENCE", 0x07FF, 0x021F },
    { "GPS",       0xFFE0, 0x3300 },   // LP-UART GPS slot (or PMU1 to P4)
    { "RSS",       0xFD20, 0x3008 },
    { "CLASSICS",  0xC618, 0x4208 },
    { "ARCADE",    0x07E0, 0x0220 },
    { "PUZZLES",   0x0400, 0x0100 },
    { "TETRIS",    0x07FF, 0x021F },   // HERO tile
};
// 12 tiles in a 3×4 grid, fleet-parity with C28P + Maxine.
// C5-specific substitutions vs C28P:
//   slot 6 GPS    (C28P has MEDIA — no audio hardware here)
//   slot 7 RSS    (C28P has WEATHER at this slot — we moved it to slot 2)
// 5G SCAN previously lived as slot 2; it's now a sub-item under CYBER
// alongside WARDRIVE since both are wifi-scanning workflows.
static const int C5_NUM_CATEGORIES = 12;

struct TileRect { int x, y, w, h; };
static TileRect c5_tile_rects[C5_NUM_CATEGORIES];

static void c5_compute_layout() {
    const int header_h = 28;
    const int footer_h = 20;
    const int margin_x = 8;
    const int gutter_x = 6;
    const int gutter_y = 6;
    int cols = 3, rows = 4;
    int avail_w = SCREEN_W - 2 * margin_x;
    int avail_h = SCREEN_H - header_h - footer_h;
    int tile_w  = (avail_w  - (cols - 1) * gutter_x) / cols;
    int tile_h  = (avail_h  - (rows - 1) * gutter_y) / rows;
    for (int i = 0; i < C5_NUM_CATEGORIES; i++) {
        int col = i % cols;
        int row = i / cols;
        c5_tile_rects[i].x = margin_x + col * (tile_w + gutter_x);
        c5_tile_rects[i].y = header_h + row * (tile_h + gutter_y);
        c5_tile_rects[i].w = tile_w;
        c5_tile_rects[i].h = tile_h;
    }
}

static void c5_draw_header() {
    gfx->fillRect(0, 0, SCREEN_W, 28, 0x0841);
    gfx->setTextColor(0x07E0);
    gfx->setTextSize(1);
    gfx->setCursor(8, 6);
    gfx->print("PISCES MOON C5");
    gfx->setTextColor(0x07FF);
    gfx->setCursor(8, 16);
    gfx->print("5G+2.4G");

    // WiFi status on the right
    gfx->setTextColor(WiFi.status() == WL_CONNECTED ? 0x07E0 : 0x4208);
    const char* wifi = (WiFi.status() == WL_CONNECTED) ? "WIFI:ON" : "WIFI:--";
    gfx->setCursor(SCREEN_W - (int)strlen(wifi) * 6 - 8, 6);
    gfx->print(wifi);
}

static void c5_draw_footer() {
    int fy = SCREEN_H - 16;
    gfx->fillRect(0, fy, SCREEN_W, 16, 0x0841);
    gfx->setTextColor(0x4208);
    gfx->setTextSize(1);
    gfx->setCursor(8, fy + 4);
    gfx->print("Tap a tile");
    const char* now = "--:--";
    gfx->setCursor(SCREEN_W - (int)strlen(now) * 6 - 8, fy + 4);
    gfx->print(now);
}

static void c5_draw_tile(int idx, bool pressed) {
    const TileRect& r = c5_tile_rects[idx];
    const C5Tile& t   = C5_CATEGORIES[idx];

    // Slot 11 — TETRIS hero tile
    if (idx == 11) {
        uint16_t acc = 0x07FF;
        uint16_t bg  = pressed ? acc : 0x0414;
        gfx->fillRoundRect(r.x, r.y, r.w, r.h, 5, bg);
        uint16_t border = ((millis() / 350) % 2) ? 0xFFFF : acc;
        gfx->drawRoundRect(r.x,   r.y,   r.w,   r.h,   5, border);
        gfx->drawRoundRect(r.x+1, r.y+1, r.w-2, r.h-2, 4, border);
        gfx->setTextSize(1);
        gfx->setTextColor(pressed ? 0x0000 : acc);
        int lw = 6 * 6;
        gfx->setCursor(r.x + (r.w - lw)/2, r.y + r.h/2 - 10);
        gfx->print("TETRIS");
        gfx->setTextColor(pressed ? 0x0000 : 0xFD20);
        int pw = 6 * 6;
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
    int text_w = (int)strlen(t.label) * 6;
    int text_x = r.x + (r.w - text_w) / 2;
    int text_y = r.y + (r.h - 8) / 2;
    gfx->setCursor(text_x, text_y);
    gfx->print(t.label);
}

static void c5_draw_all_tiles() {
    for (int i = 0; i < C5_NUM_CATEGORIES; i++) c5_draw_tile(i, false);
}

static int c5_hit_test(int16_t tx, int16_t ty) {
    for (int i = 0; i < C5_NUM_CATEGORIES; i++) {
        const TileRect& r = c5_tile_rects[i];
        if (tx >= r.x && tx < r.x + r.w &&
            ty >= r.y && ty < r.y + r.h)
            return i;
    }
    return -1;
}

// ─────────────────────────────────────────────
//  Generic sub-launcher (same shape as c28p_sub_launcher)
// ─────────────────────────────────────────────
struct C5SubItem {
    const char* label;
    void (*action)();
};

static void c5_sub_launcher(const char* title,
                             const C5SubItem* items, int count) {
    const int title_h     = 40;
    const int back_y      = 44;
    const int back_h      = 32;
    const int item_y0     = 84;
    const int item_h      = 40;
    const int item_visible = 4;
    const int margin      = 12;
    const int nav_y       = 286;
    const int nav_h       = 32;

    int page  = 0;
    int pages = (count + item_visible - 1) / item_visible;

    auto draw_chrome = [&]() {
        gfx->fillScreen(0x0000);
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
        gfx->fillRect(margin, back_y, 240 - 2 * margin, back_h, 0x18C3);
        gfx->drawRect(margin, back_y, 240 - 2 * margin, back_h, 0x8410);
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(margin + 12, back_y + 12);
        gfx->print("< BACK");

        int start = page * item_visible;
        int end   = min(count, start + item_visible);
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
        if (pages > 1) {
            uint16_t prev_color = (page > 0) ? 0xFFE0 : 0x4208;
            gfx->fillRect(margin, nav_y, 100, nav_h, 0x18C3);
            gfx->drawRect(margin, nav_y, 100, nav_h, prev_color);
            gfx->setTextSize(1);
            gfx->setTextColor(prev_color);
            gfx->setCursor(margin + 36, nav_y + 12);
            gfx->print("PREV");
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
    int  pressed_idx = -2;
    while (true) {
        int16_t tx, ty;
        bool touched = c5_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            if (ty >= back_y && ty < back_y + back_h &&
                tx >= margin && tx < 240 - margin) {
                pressed_idx = -1;
            } else if (pages > 1 && ty >= nav_y && ty < nav_y + nav_h) {
                if (tx >= margin && tx < margin + 100 && page > 0) {
                    pressed_idx = -3;
                } else if (tx >= 240 - margin - 100 && tx < 240 - margin &&
                           page < pages - 1) {
                    pressed_idx = -4;
                }
            } else if (tx >= margin && tx < 240 - margin) {
                int start = page * item_visible;
                int end   = min(count, start + item_visible);
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
            if (pressed_idx == -1) return;
            else if (pressed_idx == -3) { if (page > 0) { page--; draw_chrome(); } }
            else if (pressed_idx == -4) { if (page < pages - 1) { page++; draw_chrome(); } }
            else if (pressed_idx >= 0 && pressed_idx < count) {
                items[pressed_idx].action();
                draw_chrome();
            }
            pressed_idx = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  COMING SOON helper
// ─────────────────────────────────────────────
static void c5_coming_soon(const char* category) {
    gfx->fillScreen(0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0xFC00);
    int title_w = strlen(category) * 12;
    gfx->setCursor((SCREEN_W - title_w) / 2, 100);
    gfx->print(category);
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(60, 140);
    gfx->print("Coming soon on C5");
    gfx->setTextColor(0x4208);
    gfx->setCursor(40, 250);
    gfx->print("Tap anywhere to return");

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c5_touch_read(&tx, &ty);
        if (touched && !was_touched) return;
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  Forward decls + category wrappers
// ─────────────────────────────────────────────
extern void c5_run_wardrive();          // dual-band scan UI
extern void c5_run_wifi_setup();        // kiosk_wifi_setup.cpp (alias → kiosk_run_wifi_setup)
// c5_run_5g_scan is a file-static helper defined later in this same TU —
// no forward decl needed (and adding `extern` here would mismatch the
// `static` definition and fail to compile with -fpermissive).
extern void c5_run_about();             // defined below — device-info screen
extern void run_wifi_filemgr();
extern void run_weather();              // Open-Meteo, weather.cpp
extern void run_rss();                  // RSS reader, rss.cpp (portrait branch)

// Default stubs — these get real implementations as C5 apps come online.
// c5_run_wardrive is exposed (non-static) so c5_wardrive_engine.cpp's
// run_wardrive() shim can dispatch to it across translation units.
//
// c5_run_wifi_setup IS now real — implemented in kiosk_wifi_setup.cpp.
// run_rss IS now real — rss.cpp's portrait branch (v1.2.1 parity).
// c5_run_about IS now real — defined below.
void        c5_run_wardrive()  { c5_coming_soon("WARDRIVE");  }
static void c5_stub_gps()       { c5_coming_soon("GPS");       }

// ─────────────────────────────────────────────
//  ABOUT — device info screen (parity with C28P/Maxine ABOUT)
// ─────────────────────────────────────────────
void c5_run_about() {
    gfx->fillScreen(0x0000);

    gfx->setTextSize(2);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(12, 16);
    gfx->print("Pisces Moon");
    gfx->setTextColor(0x07FF);
    gfx->setCursor(12, 38);
    gfx->print("C5 Edition");
    gfx->setTextSize(1);
    gfx->setTextColor(0x8410);
    gfx->setCursor(12, 60);
    gfx->print("Version " PISCES_OS_VERSION);

    gfx->drawFastHLine(12, 78, SCREEN_W - 24, 0x4208);

    int y = 90;
    auto line = [&](const char* label, const char* value) {
        gfx->setTextColor(0x8410);
        gfx->setCursor(12, y);
        gfx->print(label);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(86, y);
        gfx->print(value);
        y += 16;
    };
    line("Device:",  "NM-CYD-C5");
    line("SoC:",     "ESP32-C5 RISC-V");
    line("Display:", "240x320 ST7789");
    line("Touch:",   "XPT2046 (res.)");
    line("Radio:",   "WiFi 6 2.4+5GHz");
    line("BLE:",     "5.3");
    line("Storage:", "MicroSD (SPI)");
    line("Form:",    "Desk kiosk");

    y += 6;
    gfx->drawFastHLine(12, y, SCREEN_W - 24, 0x4208);
    y += 12;
    gfx->setTextColor(0x07FF);
    gfx->setCursor(12, y);
    gfx->print("First in the fleet to see 5 GHz.");
    y += 22;
    gfx->setTextColor(0xFC00);
    gfx->setCursor(12, y);
    gfx->print("By Eric Becker");
    y += 14;
    gfx->setTextColor(0x8410);
    gfx->setCursor(12, y);
    gfx->print("Fluid Fortune");
    y += 14;
    gfx->setTextColor(0x4208);
    gfx->setCursor(12, y);
    gfx->print("Court Jester of Vibe Code");
    y += 14;
    gfx->setTextColor(0x07FF);
    gfx->setCursor(12, y);
    gfx->print("fluidfortune.com");
    y += 18;
    gfx->setTextColor(0x4208);
    gfx->setCursor(12, y);
    gfx->print("Licensed AGPL-3.0-or-later");

    gfx->setTextColor(0x07E0);
    const char* back = "TAP TO RETURN";
    gfx->setCursor((SCREEN_W - (int)strlen(back) * 6) / 2, SCREEN_H - 24);
    gfx->print(back);

    bool was_touched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c5_touch_read(&tx, &ty);
        if (touched && !was_touched) {
            // Drain the release so the submenu doesn't re-trigger.
            while (c5_touch_read(&tx, &ty)) { delay(20); yield(); }
            return;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  5G SCAN — C5-exclusive 5GHz-only access point scanner
//
//  The NM-CYD-C5 is the only board in the Pisces Moon fleet with a
//  dual-band Wi-Fi 6 radio (ESP32-C5 has 2.4 + 5 GHz). This tile
//  takes advantage of that by running a 5GHz-only scan and showing
//  whatever APs come back — a population invisible to the rest of
//  the fleet's 2.4-only radios (T-Deck, T-LoRa, Cardputer, C28P,
//  Maxine all 2.4 GHz only).
//
//  Implementation:
//    1. Save current WiFi mode + band mode
//    2. WIFI_STA + WIFI_BAND_MODE_5G_ONLY (via esp_wifi_set_band_mode)
//    3. WiFi.scanNetworks(blocking, show_hidden) — typically 2-3s
//    4. Render results sorted by RSSI descending
//    5. Wait for touch: REFRESH re-scans, EXIT restores WiFi state
//
//  This is intentionally a simpler UI than c5_wardrive_engine.cpp's
//  full async engine — just a screen for "what 5 GHz networks are
//  around me right now". The wardrive engine handles the continuous
//  log + PMU1 fan-out flow.
// ─────────────────────────────────────────────
static void c5_run_5g_scan() {
    const int header_h    = 28;
    const int status_y    = header_h + 4;
    const int list_y      = header_h + 20;
    const int row_h       = 14;
    const int footer_h    = 44;
    const int list_h      = SCREEN_H - list_y - footer_h;
    const int max_rows    = list_h / row_h;

    auto draw_chrome = [&]() {
        gfx->fillScreen(0x0000);
        gfx->fillRect(0, 0, SCREEN_W, header_h, 0x031F);
        gfx->setTextSize(1);
        gfx->setTextColor(0xFFFF);
        gfx->setCursor(8, 6);
        gfx->print("5G SCAN");
        gfx->setTextColor(0x07FF);
        gfx->setCursor(8, 16);
        gfx->print("5 GHz only");
        gfx->setTextColor(0x8410);
        gfx->setCursor(SCREEN_W - 48, 6);
        gfx->print("< EXIT");
    };

    auto draw_status = [&](const char *msg, uint16_t color) {
        gfx->fillRect(0, status_y, SCREEN_W, 12, 0x0000);
        gfx->setTextColor(color);
        gfx->setCursor(8, status_y);
        gfx->print(msg);
    };

    auto draw_refresh = [&](bool pressed) {
        int btn_y = SCREEN_H - footer_h + 4;
        gfx->fillRect(20, btn_y, SCREEN_W - 40, 32, pressed ? 0x07E0 : 0x0260);
        gfx->drawRect(20, btn_y, SCREEN_W - 40, 32, 0x07E0);
        gfx->setTextSize(2);
        gfx->setTextColor(pressed ? 0x0000 : 0x07E0);
        gfx->setCursor((SCREEN_W - 84) / 2, btn_y + 9);
        gfx->print("REFRESH");
        gfx->setTextSize(1);
    };

    auto draw_results = [&](int n) {
        gfx->fillRect(0, list_y, SCREEN_W, list_h, 0x0000);
        if (n <= 0) {
            gfx->setTextColor(0xC618);
            gfx->setCursor(8, list_y + 8);
            gfx->print("No 5 GHz APs in range.");
            gfx->setTextColor(0x8410);
            gfx->setCursor(8, list_y + 24);
            gfx->print("Check antenna & try again.");
            return;
        }
        int show = (n < max_rows) ? n : max_rows;
        for (int i = 0; i < show; i++) {
            int y    = list_y + i * row_h;
            int ch   = WiFi.channel(i);
            int rssi = WiFi.RSSI(i);
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0)      ssid = "(hidden)";
            else if (ssid.length() > 17) ssid = ssid.substring(0, 17);

            uint16_t rssi_col =
                (rssi > -55) ? 0x07E0 :
                (rssi > -70) ? 0xFFE0 :
                (rssi > -82) ? 0xFD20 : 0xF800;

            gfx->setTextColor(0x07FF);
            gfx->setCursor(8, y);
            char buf[8];
            snprintf(buf, sizeof(buf), "%-3d", ch);
            gfx->print(buf);

            gfx->setTextColor(rssi_col);
            gfx->setCursor(40, y);
            snprintf(buf, sizeof(buf), "%4d", rssi);
            gfx->print(buf);

            gfx->setTextColor(0xFFFF);
            gfx->setCursor(80, y);
            gfx->print(ssid);
        }
        if (n > max_rows) {
            gfx->setTextColor(0x8410);
            gfx->setCursor(8, list_y + max_rows * row_h);
            char buf[32];
            snprintf(buf, sizeof(buf), "... +%d more", n - max_rows);
            gfx->print(buf);
        }
    };

    // Save WiFi state so we can restore it cleanly on exit
    wifi_mode_t prev_mode = WiFi.getMode();
    wifi_band_mode_t prev_band = WIFI_BAND_MODE_AUTO;
    esp_wifi_get_band_mode(&prev_band);

    draw_chrome();
    draw_refresh(false);
    draw_status("Configuring radio...", 0xFFE0);

    // Bring up STA + restrict to 5 GHz
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);   // drop assoc but keep credentials
    delay(80);
    esp_err_t band_err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
    if (band_err != ESP_OK) {
        Serial.printf("[5G] band_mode set returned 0x%x (band switch may take effect next scan)\n", band_err);
    }
    delay(50);

    auto do_scan = [&]() {
        draw_status("Scanning 5 GHz...", 0xFFE0);
        uint32_t t0 = millis();
        int n = WiFi.scanNetworks(false /*sync*/, true /*show_hidden*/);
        uint32_t dt = millis() - t0;

        char buf[48];
        if (n < 0) {
            snprintf(buf, sizeof(buf), "Scan error: %d", n);
            draw_status(buf, 0xF800);
            draw_results(0);
        } else {
            snprintf(buf, sizeof(buf), "Found %d AP%s in %lums", n, n == 1 ? "" : "s", (unsigned long)dt);
            draw_status(buf, 0x07E0);
            draw_results(n);
        }
        WiFi.scanDelete();   // free the scan-result buffer
    };

    do_scan();

    bool was_touched = false;
    int  pressed     = -2;   // -2 = none, 0 = refresh, 1 = exit
    while (true) {
        int16_t tx, ty;
        bool touched = c5_touch_read(&tx, &ty);

        if (touched && !was_touched) {
            // Header exit zone (top bar OR "< EXIT" label at right end)
            if (ty < header_h) {
                pressed = 1;
            } else {
                int btn_y = SCREEN_H - footer_h + 4;
                if (ty >= btn_y && ty < btn_y + 32 &&
                    tx >= 20  && tx < SCREEN_W - 20) {
                    pressed = 0;
                    draw_refresh(true);
                }
            }
        } else if (!touched && was_touched) {
            if (pressed == 1) {
                // Restore WiFi state for downstream apps that expect it
                esp_wifi_set_band_mode(prev_band);
                WiFi.mode(prev_mode);
                return;
            } else if (pressed == 0) {
                draw_refresh(false);
                do_scan();
            }
            pressed = -2;
        }
        was_touched = touched;
        delay(20);
        yield();
    }
}

// REFERENCE app wrappers — partial: SURVIVAL/MEDICAL/HISTORY still
// deferred (those use data_reader.cpp's trackball+keyboard navigation,
// which needs a touch-only port — c5_data_reader.cpp — that doesn't
// exist yet). E-READER already has touch branches for C28P + C5 in
// ereader.cpp, so it's wired up directly.
static void c5_run_survival() { c5_coming_soon("SURVIVAL"); }
static void c5_run_medical()  { c5_coming_soon("MEDICAL");  }
static void c5_run_history()  { c5_coming_soon("HISTORY");  }

// ─────────────────────────────────────────────
//  TOUCH CALIBRATION — 4-corner crosshair walkthrough
//
//  XPT2046 is a 12-bit ADC reading a resistive overlay. The raw
//  ADC range mapped to each physical edge depends on overlay
//  tolerances, bias resistors, and chassis mount. CYD community
//  defaults (RAW_X_MIN=200, MAX=3900, RAW_Y_MIN=240, MAX=3800)
//  are a starting point — typically off by 50–150 ADC counts on
//  any individual panel. A 100-count error on a ~3700-count range
//  is ~3%, ~7 pixels of registration drift at the edges.
//
//  This function walks TL → TR → BL → BR, reads raw ADC at each
//  crosshair tap, computes new constants, applies them live for
//  the rest of this boot, and prints the persistent values to
//  serial so they can be hard-coded into c5_boot.cpp.
//
//  Mapping geometry (see c5_touch_read above):
//    panel_x = (raw_x - RAW_X_MIN) * 320 / (RAW_X_MAX - RAW_X_MIN)
//    panel_y = (raw_y - RAW_Y_MIN) * 240 / (RAW_Y_MAX - RAW_Y_MIN)
//    pixel_x = panel_y                          (0..239 portrait)
//    pixel_y = (C5_PANEL_W - 1) - panel_x       (319..0)
//  Hence:
//    RAW_X_MIN = avg(BL.raw_x, BR.raw_x)
//    RAW_X_MAX = avg(TL.raw_x, TR.raw_x)
//    RAW_Y_MIN = avg(TL.raw_y, BL.raw_y)
//    RAW_Y_MAX = avg(TR.raw_y, BR.raw_y)
// ─────────────────────────────────────────────
static void c5_run_calibration() {
    struct Corner { int x, y; const char* name; };
    Corner corners[4] = {
        { 8,             8,              "TOP LEFT"     },
        { SCREEN_W - 9,  8,              "TOP RIGHT"    },
        { 8,             SCREEN_H - 9,   "BOTTOM LEFT"  },
        { SCREEN_W - 9,  SCREEN_H - 9,   "BOTTOM RIGHT" },
    };

    uint16_t raw_x_at[4] = {0, 0, 0, 0};
    uint16_t raw_y_at[4] = {0, 0, 0, 0};

    // Intro screen
    gfx->fillScreen(0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07FF);
    {
        const char *t = "CALIBRATION";
        int w = (int)strlen(t) * 12;
        gfx->setCursor((SCREEN_W - w) / 2, 30);
        gfx->print(t);
    }
    gfx->setTextSize(1);
    gfx->setTextColor(0xC618);
    const char *intro[] = {
        "Tap each crosshair as it",
        "appears. Press as close to",
        "the corner as you can.",
        "",
        "Results print to serial.",
    };
    int ly = 90;
    for (auto& l : intro) {
        int lw = (int)strlen(l) * 6;
        gfx->setCursor((SCREEN_W - lw) / 2, ly);
        gfx->print(l);
        ly += 16;
    }
    gfx->setTextColor(0x07E0);
    const char *go = "TAP TO BEGIN";
    gfx->setCursor((SCREEN_W - (int)strlen(go) * 6) / 2, 220);
    gfx->print(go);

    {
        bool was = false;
        while (true) {
            int16_t tx, ty;
            bool t = c5_touch_read(&tx, &ty);
            if (!t && was) break;
            was = t;
            delay(30);
            yield();
        }
    }

    // Four corners
    for (int i = 0; i < 4; i++) {
        gfx->fillScreen(0x0000);

        gfx->setTextSize(1);
        gfx->setTextColor(0x07FF);
        gfx->setCursor(8, 6);
        gfx->printf("CALIBRATION %d / 4", i + 1);

        gfx->setTextSize(2);
        gfx->setTextColor(0xFD20);
        const char *name = corners[i].name;
        int nw = (int)strlen(name) * 12;
        gfx->setCursor((SCREEN_W - nw) / 2, SCREEN_H / 2 - 20);
        gfx->print(name);

        gfx->setTextSize(1);
        gfx->setTextColor(0xC618);
        const char *hint = "Tap the crosshair";
        gfx->setCursor((SCREEN_W - (int)strlen(hint) * 6) / 2, SCREEN_H / 2 + 8);
        gfx->print(hint);

        int cx = corners[i].x;
        int cy = corners[i].y;
        gfx->drawFastHLine(cx - 12, cy, 25, 0xF800);
        gfx->drawFastVLine(cx, cy - 12, 25, 0xF800);
        gfx->drawCircle(cx, cy, 5, 0xFFFF);
        gfx->drawCircle(cx, cy, 2, 0xFFFF);

        // Sample raw ADC directly. Lenient stability gate (±40 ADC counts
        // vs the runtime driver's ±20) because corner taps have noisier
        // ADC due to edge effects on the resistive overlay.
        uint32_t sum_x = 0, sum_y = 0;
        int      samples = 0;
        bool     was_touched = false;
        uint32_t corner_start = millis();

        while (true) {
            if (millis() - corner_start > 30000) {
                Serial.printf("[CAL] Timeout waiting for %s tap\n", name);
                raw_x_at[i] = 0;
                raw_y_at[i] = 0;
                break;
            }

            if (!PM_SPI_TAKE("CAL")) { delay(20); yield(); continue; }
            SPI.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
            digitalWrite(PIN_LCD_CS,   HIGH);
            digitalWrite(PIN_SD_CS,    HIGH);
            digitalWrite(PIN_TOUCH_CS, LOW);
            (void)xpt_read(XPT2046_CMD_READ_X);
            uint16_t x1 = xpt_read(XPT2046_CMD_READ_X);
            uint16_t y1 = xpt_read(XPT2046_CMD_READ_Y);
            uint16_t x2 = xpt_read(XPT2046_CMD_READ_X);
            uint16_t y2 = xpt_read(XPT2046_CMD_READ_Y);
            digitalWrite(PIN_TOUCH_CS, HIGH);
            SPI.endTransaction();
            PM_SPI_GIVE();

            int dx = (int)x1 - (int)x2; if (dx < 0) dx = -dx;
            int dy = (int)y1 - (int)y2; if (dy < 0) dy = -dy;
            bool stable = (dx <= 40 && dy <= 40);
            uint16_t raw_x = (x1 + x2) / 2;
            uint16_t raw_y = (y1 + y2) / 2;
            bool touched =
                stable &&
                raw_x > 50  && raw_x < 4050 &&
                raw_y > 50  && raw_y < 4050;

            if (touched) {
                sum_x += raw_x;
                sum_y += raw_y;
                samples++;
                was_touched = true;
            } else if (was_touched && samples >= 3) {
                raw_x_at[i] = (uint16_t)(sum_x / samples);
                raw_y_at[i] = (uint16_t)(sum_y / samples);
                Serial.printf("[CAL] %s: raw_x=%d  raw_y=%d  (%d samples)\n",
                              name, raw_x_at[i], raw_y_at[i], samples);
                break;
            } else if (!touched) {
                was_touched = false;
                sum_x = sum_y = 0;
                samples = 0;
            }

            delay(20);
            yield();
        }

        // Drain finger-still-down so the next corner starts clean.
        uint32_t drain_start = millis();
        while (millis() - drain_start < 500) {
            int16_t tx, ty;
            if (!c5_touch_read(&tx, &ty)) break;
            delay(30);
            yield();
        }
        delay(150);
    }

    // Compute and apply edge constants from corner readings
    //
    // Each pixel-edge averages the two corners that share it:
    //   LEFT  (pixel_x=0)        ← TL.raw_x + BL.raw_x
    //   RIGHT (pixel_x=SCREEN_W) ← TR.raw_x + BR.raw_x
    //   TOP   (pixel_y=0)        ← TL.raw_y + TR.raw_y
    //   BOT   (pixel_y=SCREEN_H) ← BL.raw_y + BR.raw_y
    //
    // This naming is unambiguous about which physical edge each value
    // represents, so inverted axes are handled by the values themselves
    // rather than a separate rotate-flag.
    int sug_x_left   = (raw_x_at[0] + raw_x_at[2]) / 2;   // TL + BL
    int sug_x_right  = (raw_x_at[1] + raw_x_at[3]) / 2;   // TR + BR
    int sug_y_top    = (raw_y_at[0] + raw_y_at[1]) / 2;   // TL + TR
    int sug_y_bottom = (raw_y_at[2] + raw_y_at[3]) / 2;   // BL + BR

    Serial.println("[CAL] ===== CALIBRATION COMPLETE =====");
    Serial.printf ("[CAL] TL: x=%4d  y=%4d\n", raw_x_at[0], raw_y_at[0]);
    Serial.printf ("[CAL] TR: x=%4d  y=%4d\n", raw_x_at[1], raw_y_at[1]);
    Serial.printf ("[CAL] BL: x=%4d  y=%4d\n", raw_x_at[2], raw_y_at[2]);
    Serial.printf ("[CAL] BR: x=%4d  y=%4d\n", raw_x_at[3], raw_y_at[3]);
    Serial.println("[CAL] Paste these into c5_boot.cpp:");
    Serial.printf ("[CAL]   static int RAW_X_AT_LEFT   = %4d;   // was %d\n", sug_x_left,   RAW_X_AT_LEFT);
    Serial.printf ("[CAL]   static int RAW_X_AT_RIGHT  = %4d;   // was %d\n", sug_x_right,  RAW_X_AT_RIGHT);
    Serial.printf ("[CAL]   static int RAW_Y_AT_TOP    = %4d;   // was %d\n", sug_y_top,    RAW_Y_AT_TOP);
    Serial.printf ("[CAL]   static int RAW_Y_AT_BOTTOM = %4d;   // was %d\n", sug_y_bottom, RAW_Y_AT_BOTTOM);

    RAW_X_AT_LEFT   = sug_x_left;
    RAW_X_AT_RIGHT  = sug_x_right;
    RAW_Y_AT_TOP    = sug_y_top;
    RAW_Y_AT_BOTTOM = sug_y_bottom;

    // Results screen
    gfx->fillScreen(0x0000);
    gfx->setTextSize(2);
    gfx->setTextColor(0x07E0);
    {
        const char *t = "CALIBRATED";
        int w = (int)strlen(t) * 12;
        gfx->setCursor((SCREEN_W - w) / 2, 18);
        gfx->print(t);
    }
    gfx->setTextSize(1);
    gfx->setTextColor(0xC618);
    gfx->setCursor(12, 56);  gfx->print("Corner readings:");
    char buf[48];
    snprintf(buf, sizeof(buf), "TL: %4d , %4d", raw_x_at[0], raw_y_at[0]);
    gfx->setCursor(24, 76);  gfx->print(buf);
    snprintf(buf, sizeof(buf), "TR: %4d , %4d", raw_x_at[1], raw_y_at[1]);
    gfx->setCursor(24, 90);  gfx->print(buf);
    snprintf(buf, sizeof(buf), "BL: %4d , %4d", raw_x_at[2], raw_y_at[2]);
    gfx->setCursor(24, 104); gfx->print(buf);
    snprintf(buf, sizeof(buf), "BR: %4d , %4d", raw_x_at[3], raw_y_at[3]);
    gfx->setCursor(24, 118); gfx->print(buf);

    gfx->setTextColor(0x07FF);
    gfx->setCursor(12, 144); gfx->print("New constants (live):");
    snprintf(buf, sizeof(buf), "X L:%4d  R:%4d", sug_x_left, sug_x_right);
    gfx->setCursor(24, 164); gfx->print(buf);
    snprintf(buf, sizeof(buf), "Y T:%4d  B:%4d", sug_y_top, sug_y_bottom);
    gfx->setCursor(24, 178); gfx->print(buf);

    gfx->setTextColor(0xFD20);
    gfx->setCursor(12, 206); gfx->print("To persist across boots:");
    gfx->setTextColor(0xC618);
    gfx->setCursor(12, 222); gfx->print("copy serial output into");
    gfx->setCursor(12, 236); gfx->print("src/c5_boot.cpp");

    gfx->setTextColor(0x07E0);
    const char *back = "TAP TO RETURN";
    gfx->setCursor((SCREEN_W - (int)strlen(back) * 6) / 2, 280);
    gfx->print(back);

    bool was = false;
    while (true) {
        int16_t tx, ty;
        bool t = c5_touch_read(&tx, &ty);
        if (!t && was) break;
        was = t;
        delay(30);
        yield();
    }
}

static void c5_open_arcade() {
    static const C5SubItem items[] = {
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
    c5_sub_launcher("ARCADE", items, sizeof(items) / sizeof(items[0]));
}

static void c5_open_puzzles() {
    static const C5SubItem items[] = {
        { "2048",       run_2048 },
        { "MINESWEEPER",run_minesweeper },
        { "CONNECT 4",  run_connect4 },
        { "SIMON",      run_simon },
        { "SOLITAIRE",  run_solitaire },
        { "CHESS",      run_chess },
    };
    c5_sub_launcher("PUZZLES", items, sizeof(items) / sizeof(items[0]));
}

static void c5_open_utilities() {
    static const C5SubItem items[] = {
        { "CLOCK",     pm_run_clock },
        { "TIMER",     pm_run_timer },
        { "STOPWATCH", pm_run_stopwatch },
        { "CALC",      pm_run_calculator },
        { "UNITS",     pm_run_units },
    };
    c5_sub_launcher("UTILITIES", items, sizeof(items) / sizeof(items[0]));
}

static void c5_open_personal() {
    static const C5SubItem items[] = {
        { "NOTES",    pm_run_notes },
        { "CONTACTS", pm_run_contacts },
        { "CALENDAR", pm_run_calendar },
    };
    c5_sub_launcher("PERSONAL", items, sizeof(items) / sizeof(items[0]));
}

static void c5_open_reference() {
    static const C5SubItem items[] = {
        { "SURVIVAL",  c5_run_survival },
        { "E-READER",  run_ereader },
        { "MEDICAL",   c5_run_medical },
        { "HISTORY",   c5_run_history },
    };
    c5_sub_launcher("REFERENCE", items, sizeof(items) / sizeof(items[0]));
}

static void c5_open_cyber() {
    static const C5SubItem items[] = {
        { "WARDRIVE",  c5_run_wardrive },   // 2.4G + 5G dual-band engine
        { "5G SCAN",   c5_run_5g_scan  },   // 5GHz-only live (C5-exclusive)
        { "TRACKERS",  pm_run_tracker_scan },
    };
    c5_sub_launcher("CYBER", items, sizeof(items) / sizeof(items[0]));
}

static void c5_open_system() {
    static const C5SubItem items[] = {
        { "WIFI",      c5_run_wifi_setup },  // scan + on-screen-keyboard
        { "SD FILES",  run_wifi_filemgr },
        { "CALIBRATE", c5_run_calibration }, // XPT2046 corner-tap walkthrough
        { "ABOUT",     c5_run_about },       // real device-info screen
    };
    c5_sub_launcher("SYSTEM", items, sizeof(items) / sizeof(items[0]));
}

static void c5_open_classics() {
    static const C5SubItem items[] = {
        { "SOLITAIRE",   run_solitaire },
        { "MINESWEEPER", run_minesweeper },
    };
    c5_sub_launcher("CLASSICS", items, sizeof(items) / sizeof(items[0]));
}

// ─────────────────────────────────────────────
//  MAIN LAUNCHER
// ─────────────────────────────────────────────
void c5_launcher() {
    Serial.println("[C5] Launcher starting");
    c5_compute_layout();

    gfx->fillScreen(0x0000);
    c5_draw_header();
    c5_draw_footer();
    c5_draw_all_tiles();

    int pressed_idx = -1;
    unsigned long last_status = millis();

    while (true) {
        int16_t tx, ty;
        bool touched = c5_touch_read(&tx, &ty);

        if (touched) {
            int idx = c5_hit_test(tx, ty);
            if (idx >= 0 && idx != pressed_idx) {
                if (pressed_idx >= 0) c5_draw_tile(pressed_idx, false);
                c5_draw_tile(idx, true);
                pressed_idx = idx;
                Serial.printf("[C5] Tile pressed: %s (idx=%d)\n",
                              C5_CATEGORIES[idx].label, idx);
            }
        } else if (pressed_idx >= 0) {
            int dispatched = pressed_idx;
            Serial.printf("[C5] Tile released: %s\n",
                          C5_CATEGORIES[dispatched].label);
            c5_draw_tile(pressed_idx, false);
            pressed_idx = -1;

            switch (dispatched) {
                case  0: c5_open_system();    break;
                case  1: c5_open_cyber();     break;   // includes WARDRIVE + 5G SCAN + TRACKERS
                case  2: run_weather();       break;   // Open-Meteo, WiFi-fetched
                case  3: c5_open_utilities(); break;
                case  4: c5_open_personal();  break;
                case  5: c5_open_reference(); break;
                case  6: c5_stub_gps();       break;
                case  7: run_rss();           break;   // rss.cpp portrait branch
                case  8: c5_open_classics();  break;
                case  9: c5_open_arcade();    break;
                case 10: c5_open_puzzles();   break;
                case 11: run_tetris();        break;
            }

            gfx->fillScreen(0x0000);
            c5_draw_header();
            c5_draw_footer();
            c5_draw_all_tiles();
            last_status = millis();
        }

        if (millis() - last_status > 3000) {
            c5_draw_header();
            last_status = millis();
        }

        delay(20);
        yield();
    }
}

// ─────────────────────────────────────────────
//  C5 INPUT/AUDIO STUBS (same pattern as C28P)
// ─────────────────────────────────────────────
#include "gamepad.h"

char get_keypress() { return 0; }

#include "trackball.h"
void init_trackball() {}
TrackballState update_trackball_game() { return TrackballState{0, 0, false}; }

GamepadState g_gamepad = {};
void gamepad_init() {}
bool gamepad_poll() { return false; }
bool gamepad_pair() { return false; }
bool gamepad_is_paired() { return false; }
void gamepad_forget() {}
void gamepad_disconnect() {}

// Audio: the C5 board's I2S audio path is not wired in this revision.
// pm_game_audio_* calls compile against the device-agnostic stubs in
// game_audio.cpp the same way Maxine does. SFX are silent until a real
// audio backend is added.

#endif // DEVICE_C5
