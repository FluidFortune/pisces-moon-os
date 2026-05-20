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
 * PROJECT: PISCES MOON OS v1.2.0 "Multi-Device"
 * HARDWARE: LilyGo T-Deck (ESP32-S3)
 * ARCHITECTURE: Dual-Core Cybernetic Environment
 * HYBRID POWER: Pin 10 drives the screen. PMU drives the GPS.
 */

#include <Arduino.h>
#include <WiFi.h>            // Added for non-blocking WiFi status checks
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <SdFat.h>           
#include <TinyGPSPlus.h>     
#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_CARDPUTER_ADV)
#include <Arduino_GFX_Library.h>
#endif
#ifdef DEVICE_TLORAPAGER
    #include "pm_disp_tlorapager.h"
#endif
#include <XPowersLib.h>
#include <esp_task_wdt.h>    // WDT feed — prevents Guru Meditation during long setup()
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h> // SemaphoreHandle_t — spi_mutex for SD bus arbitration
#include <soc/gpio_struct.h> // GPIO matrix introspection (func_out_sel_cfg)
#include <esp_arduino_version.h> // ESP_ARDUINO_VERSION_MAJOR/MINOR/PATCH

// --- PISCES MOON CUSTOM HEADERS ---
#include "touch.h"       
#include "trackball.h"   // Needed for early GPIO0 init
#include "launcher.h"    
#include "wardrive.h"    
    #include "keyboard.h"    
    #include "pm_input.h"
    #include "gemini_client.h" 
#include "wifi_manager.h" 
#include "database.h"
#include "ghost_partition.h"  // Ghost Partition / PIN router system
#ifdef DEVICE_CARDPUTER_ADV
#include "pi4ioe_cap.h"       // Cap LoRa-1262 I2C expander (RF switch enable)
#endif

// --- GLOBAL VARIABLES TO SATISFY THE LINKER ---
bool exitApp = false; 
XPowersAXP2101 PMU; 
SdFat sd;           
TinyGPSPlus gps;    

// --- SYSTEM STATE FLAGS (PISCES ARCHITECTURE) ---
bool isWiFiConnected = false;
const unsigned long WIFI_TIMEOUT_MS = 5000;  // 5 seconds max for autoconnect

// --- DEVICE-SPECIFIC HARDWARE PINMAPS ---
// Selected at build time by platformio.ini build_flags

#ifdef DEVICE_TDECK_PLUS
// --- T-DECK HARDWARE PINMAP ---
#define BOARD_POWERON     10    
#define BOARD_LORA_CS     9     
#define BOARD_TFT_BL      42    

// SPI Bus for Display & SD Card
#define BOARD_TFT_DC      11
#define BOARD_TFT_CS      12
#define BOARD_TFT_MOSI    41
#define BOARD_TFT_MISO    38
#define BOARD_TFT_SCK     40
#define BOARD_TFT_RST     -1    

// Storage & I2C Peripherals
#define BOARD_SD_CS       39
#define BOARD_I2C_SDA     18
#define BOARD_I2C_SCL     8     
#endif // DEVICE_TDECK_PLUS

#ifdef DEVICE_TLORAPAGER
// --- T-LORA PAGER HARDWARE PINMAP ---
// Source: T-LoraPager V1.0 SCH 25-06-13.pdf + LilyGoLib wiki
// No T-Deck PMU (uses BQ25896 over XL9555 expander instead)
// No GT911 touch
// SPI bus shared by LCD + LoRa + SD + NFC
#define BOARD_POWERON     -1    // No direct power pin — via XL9555
#define BOARD_LORA_CS     36
#define BOARD_TFT_BL      42    // AW9364 backlight driver enable

// SPI Bus — shared by LCD, LoRa, SD, NFC
#define BOARD_TFT_DC      37
#define BOARD_TFT_CS      38
#define BOARD_TFT_MOSI    34
#define BOARD_TFT_MISO    33
#define BOARD_TFT_SCK     35
#define BOARD_TFT_RST     -1    // Wired to XL9555 P06 (LCD_RST)

// Storage & I2C Peripherals
#define BOARD_SD_CS       21
#define BOARD_I2C_SDA     3     // NOTE: swapped vs T-Deck
#define BOARD_I2C_SCL     2

// T-LoraPager additional pins
#define BOARD_NFC_CS      39
#define BOARD_LORA_IRQ    14
#define BOARD_LORA_RST    47
#define BOARD_LORA_BUSY   48
#define BOARD_GPS_TX_PIN  4     // GPS module TX → ESP32 RX
#define BOARD_GPS_RX_PIN  12    // GPS module RX → ESP32 TX
#define BOARD_ENC_A       40
#define BOARD_ENC_B       41
#define BOARD_ENC_BTN     7
#define BOARD_KEY_INT     6

// XL9555 GPIO expander (I2C 0x20) — controls all peripheral power
// VERIFIED FROM SCHEMATIC: T-Lora_Pager_V1_0_SCH_25-06-13.pdf page 3
//   Port 0 → bits 0-7:  P00=M_EN, P01=SHUTDOWN, P02=KEY_RST, P03=LORA_EN,
//                       P04=GPS_EN, P05=RF_EN (NFC), P06=LCD_RST, P07=GPS_RST
//   Port 1 → bits 8-15: P10=KEY_EN, P11=NRF_CE, P12=SD_DET (in),
//                       P13=SPI_PULLUP_EN, P14=SD_EN, P15-P17=NC
#define IOEXP_I2C_ADDR    0x20
#define XL9555_KEY_RST    2     // P02 — Keyboard (TCA8418) reset (active-LOW)
#define XL9555_LORA_EN    3     // P03 — LoRa power
#define XL9555_GPS_EN     4     // P04 — GPS power
#define XL9555_NFC_EN     5     // P05 — NFC power (RF_EN)
#define XL9555_LCD_RST    6     // P06 — LCD reset
#define XL9555_GPS_RST    7     // P07 — GPS reset (out of reset = HIGH)
#define XL9555_KB_EN      8     // P10 — Keyboard power
#define XL9555_NRF_CE     9     // P11 — nRF24L01 CE (shield only)
#define XL9555_SPI_PULLUP 11    // P13 — SPI pull-up enable
#define XL9555_SD_EN      12    // P14 — SD card power
#endif // DEVICE_TLORAPAGER

#ifdef DEVICE_CARDPUTER_ADV
// --- CARDPUTER ADV HARDWARE PINMAP ---
// Source: platformio.ini Cardputer ADV build flags.
#define BOARD_POWERON     -1
#define BOARD_LORA_CS     PIN_LORA_CS
#define BOARD_LORA_RST    PIN_LORA_RST
#define BOARD_LORA_IRQ    PIN_LORA_IRQ
#define BOARD_LORA_BUSY   PIN_LORA_BUSY

#define BOARD_TFT_BL      PIN_LCD_BL
#define BOARD_TFT_DC      PIN_LCD_DC
#define BOARD_TFT_CS      PIN_LCD_CS
#define BOARD_TFT_MOSI    PIN_LCD_MOSI
#define BOARD_TFT_MISO    -1
#define BOARD_TFT_SCK     PIN_LCD_SCK
#define BOARD_TFT_RST     PIN_LCD_RST

#define BOARD_SD_CS       PIN_SD_CS
#define BOARD_I2C_SDA     PIN_I2C_SDA
#define BOARD_I2C_SCL     PIN_I2C_SCL
#define BOARD_GPS_TX_PIN  PIN_GPS_TX
#define BOARD_GPS_RX_PIN  PIN_GPS_RX
#define BOARD_KEY_INT     PIN_KEY_INT
#endif // DEVICE_CARDPUTER_ADV

// --- DRIVER INSTANTIATION ---
#ifdef DEVICE_TDECK_PLUS
Arduino_DataBus *bus = new Arduino_HWSPI(BOARD_TFT_DC, BOARD_TFT_CS, BOARD_TFT_SCK, BOARD_TFT_MOSI, BOARD_TFT_MISO, &SPI, true);
Arduino_GFX *gfx = new Arduino_ST7789(bus, BOARD_TFT_RST, 1 /* Landscape */, true /* IPS */);
#endif

#ifdef DEVICE_CARDPUTER_ADV
Arduino_DataBus *bus = new Arduino_HWSPI(BOARD_TFT_DC, BOARD_TFT_CS, BOARD_TFT_SCK, BOARD_TFT_MOSI, BOARD_TFT_MISO, &SPI, true);
Arduino_GFX *gfx = new Arduino_ST7789(bus, BOARD_TFT_RST, 1 /* Landscape */, true /* IPS */, 135, 240, 52, 40, 53, 40);
SPIClass cardputerSdSPI(HSPI);
#endif

#ifdef DEVICE_TLORAPAGER
// T-LoraPager: ST7796 480x222 user-facing landscape with y-offset 49.
//
// Pisces Moon's PMDispTLoRaPager is a STANDALONE driver — no
// Arduino_GFX, no Arduino_DataBus, no inheritance from any
// third-party display library. Owns SPI directly, runs
// LilyGo's official 19-command init sequence in begin(),
// implements its own drawing primitives, line/circle/triangle
// algorithms, and text rendering with an embedded 5x7 font.
//
// We dropped Arduino_GFX entirely because its base-class
// clipping and address-window logic was producing a split
// image — half the screen would render correctly while the
// other half would silently drop draw calls. Direct ownership
// of every pixel write makes the bug surface visible.
PMDispTLoRaPager *gfx = new PMDispTLoRaPager(
    BOARD_TFT_SCK, BOARD_TFT_MISO, BOARD_TFT_MOSI,
    BOARD_TFT_CS, BOARD_TFT_DC, BOARD_TFT_BL,
    40000000UL, SPI);
#endif

// --- MULTI-THREADING HANDLES ---
TaskHandle_t GhostTask;

// --- SPI BUS MUTEX ---
// Shared between Core 0 (wardrive SD writes) and Core 1 (system_app, wifi_filemgr SD reads).
// Any code touching sd.* must take this mutex first and release immediately after.
// On T-LoraPager this is ALSO injected into the display driver via setSharedMutex()
// so display+SD+LoRa+NFC all coordinate through one mutex (SPI Bus Treaty).
SemaphoreHandle_t spi_mutex = nullptr;

// ── DEFERRED SD READINESS FLAG ─────────────────────────────────────
// On T-LoraPager, SD mount is deferred to a background task because
// the card needs several seconds after power-on to respond, and nothing
// during boot actually needs SD access. Apps that need SD read this
// flag at launch and either wait briefly or display "SD initializing".
// On T-Deck Plus, this is set synchronously during boot.
//
// std::atomic<bool> would be ideal but volatile bool is sufficient
// here since we only have one writer (the late-init task or boot path)
// and many readers (apps), and the value transitions exactly once
// from false → true.
volatile bool g_sd_ready = false;

#ifdef DEVICE_TLORAPAGER
// Background task that mounts SD after boot completes.
// Runs on Core 1 at low priority (1) so it never blocks user input.
// Waits 3 seconds for the card to settle, then retries mount up to
// 10 times at 1-second intervals (matching LilyGo's SD_Test pattern).
static void late_sd_init_task(void *param) {
    Serial.println("[SD-LATE] Background SD mount task started");
    // Give the card time to fully stabilize after all the boot-time
    // SPI traffic (display init, etc.)
    vTaskDelay(3000 / portTICK_PERIOD_MS);

    // Cold-restart the SPI peripheral before SD operations.
    // Display init left it configured with the display's clock divider,
    // mode, and DMA settings. SPI.end() releases the GPIO matrix and
    // tears down the peripheral; SPI.begin() reacquires it cleanly.
    // This is the surgical equivalent of LilyGo's "init display first,
    // then SPI.begin() fresh" pattern from their LilyGoLoRaPager::begin().
    if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
    Serial.println("[SD-LATE] Cold-restarting SPI peripheral");
    SPI.end();
    vTaskDelay(20 / portTICK_PERIOD_MS);
    SPI.begin(BOARD_TFT_SCK, BOARD_TFT_MISO, BOARD_TFT_MOSI);
    vTaskDelay(20 / portTICK_PERIOD_MS);
    if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);

    bool mounted = false;
    for (int attempt = 1; attempt <= 10 && !mounted; attempt++) {
        // Reassert shared-bus CS HIGH + LORA_RST HIGH before each attempt.
        // This matches LilyGo's installSD() pattern.
        if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
        pinMode(BOARD_LORA_RST, OUTPUT);
        digitalWrite(BOARD_LORA_RST, HIGH);
        digitalWrite(BOARD_LORA_CS, HIGH);
        digitalWrite(BOARD_SD_CS,   HIGH);
        digitalWrite(BOARD_TFT_CS,  HIGH);
        digitalWrite(BOARD_NFC_CS,  HIGH);
        if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
        vTaskDelay(10 / portTICK_PERIOD_MS);

        mounted = ghost_partition_mount_public(BOARD_SD_CS, SPI);
        if (mounted) {
            Serial.printf("[SD-LATE] Mounted on attempt %d\n", attempt);
            break;
        }
        Serial.printf("[SD-LATE] Attempt %d/10 failed — retrying in 1s\n", attempt);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    if (mounted) {
        g_sd_ready = true;
        Serial.println("[SD-LATE] SD ready — initializing vault");
        if (init_database()) {
            Serial.println("[SD-LATE] Vault initialized");
        } else {
            Serial.println("[SD-LATE] Vault init failed (SD mounted but write failed?)");
        }
    } else {
        Serial.println("[SD-LATE] SD mount failed after 10 attempts — SD unavailable");
    }
    vTaskDelete(NULL);
}
#endif

// --- CORE 0: THE GHOST ENGINE ---
void core0GhostTask(void * parameter) {
    Serial.println("[SYSTEM] Ghost Engine spawned on Core 0");
    vTaskDelay(2000 / portTICK_PERIOD_MS); 
    for(;;) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

// ─────────────────────────────────────────────
//  BIOS BOOT SCREEN v2 — CYBERPUNK TERMINAL
//
//  Bug fix vs v1:
//    auto_connect_wifi() called BETWEEN print() calls corrupted
//    the GFX cursor mid-line via WiFi SDK SPI state changes.
//    Fix: render the complete line FIRST, then call auto_connect_wifi().
// ─────────────────────────────────────────────

#define BOOT_BG         0x0000
#define BOOT_HEADER_BG  0x0121
#define BOOT_DIVIDER    0x0240
#define BOOT_SECTION    0x0180
#define BOOT_LABEL      0x03E0
#define BOOT_ADDR       0x0280
#define BOOT_TS         0x0160
#define BOOT_OK         0x07E0
#define BOOT_OK_BG      0x00C0
#define BOOT_ACTIVE     0x07FF
#define BOOT_ACTIVE_BG  0x0011
#define BOOT_WARN       0xFFE0
#define BOOT_WARN_BG    0x0840
#define BOOT_FAIL       0xF800
#define BOOT_FAIL_BG    0x4000
#define BOOT_PROGRESS   0x03A0

#ifndef PM_TLORA_DISPLAY_DIAG
#define PM_TLORA_DISPLAY_DIAG 0
#endif

static int bootY = 0;

static int16_t bootW() { return gfx->width(); }
static int16_t bootH() { return gfx->height(); }

#ifdef DEVICE_CARDPUTER_ADV
// ─────────────────────────────────────────────
//  CARDPUTER BIOS AUTO-SCROLL
//
//  The Cardputer's 240×135 screen can only fit ~9 BIOS lines in
//  the visible body region. Without scrolling, anything past line
//  9 renders below the screen and is lost.
//
//  Mechanism:
//    Every drawBootLine() / drawBootSection() call records the
//    entry into a small ring buffer in RAM. Before rendering, we
//    check whether bootY would overflow the body region; if it
//    would, we clear the body, replay the last ~9 entries shifted
//    up by one slot, and write the new entry at the bottom slot.
//
//    T-Deck and Pager code paths are unchanged — the entire block
//    is gated by #ifdef DEVICE_CARDPUTER_ADV.
//
//  Memory: ~12 entries × ~80 bytes = ~1 KB worst case. Bounded
//  and only allocated on Cardputer.
// ─────────────────────────────────────────────

#define CP_BOOT_LINES_VISIBLE  9
#define CP_BOOT_RING_SIZE      12
#define CP_BOOT_BODY_TOP       18
#define CP_BOOT_LINE_H         10
#define CP_BOOT_SECTION_H      13      // 2 dividers + 9px label
#define CP_BOOT_FOOTER_TOP    (135 - 12)

enum CpBootEntryKind {
    CP_BE_NONE = 0,
    CP_BE_SECTION,
    CP_BE_LINE,
};

struct CpBootEntry {
    uint8_t kind;
    char    sec_name[28];   // for CP_BE_SECTION
    char    ts[8];          // for CP_BE_LINE
    char    label[32];
    char    detail[24];
    uint8_t tag_type;
    char    tag_text[12];
};

static CpBootEntry cp_boot_ring[CP_BOOT_RING_SIZE];
static int         cp_boot_ring_count = 0;

// Render a single CP_BE_LINE entry at the specified y coordinate.
// Mirrors the standard drawBootLine geometry but uses the entry's
// stored fields instead of function args, and an explicit y instead
// of bootY.
static void cp_render_line_at(const CpBootEntry &e, int y) {
    int16_t W = bootW();
    gfx->setTextSize(1);
    gfx->setTextColor(BOOT_TS);
    gfx->setCursor(4, y);
    gfx->print(e.ts);
    gfx->setTextColor(BOOT_LABEL);
    gfx->setCursor(36, y);
    gfx->print(e.label);
    if (e.detail[0] != '\0') {
        gfx->setTextColor(BOOT_ADDR);
        gfx->setCursor((W >= 420) ? W - 150 : 174, y);
        gfx->print(e.detail);
    }
    uint16_t tagFg, tagBg;
    switch (e.tag_type) {
        case 1:  tagFg = BOOT_ACTIVE; tagBg = BOOT_ACTIVE_BG; break;
        case 2:  tagFg = BOOT_WARN;   tagBg = BOOT_WARN_BG;   break;
        case 3:  tagFg = BOOT_FAIL;   tagBg = BOOT_FAIL_BG;   break;
        default: tagFg = BOOT_OK;     tagBg = BOOT_OK_BG;     break;
    }
    int tagW = max(24, (int)(strlen(e.tag_text) * 6) + 6);
    int tagX = W - tagW - 3;
    gfx->fillRect(tagX, y - 1, tagW, 9, tagBg);
    gfx->drawRect(tagX, y - 1, tagW, 9, tagFg);
    gfx->setTextColor(tagFg);
    gfx->setCursor(tagX + 3, y);
    gfx->print(e.tag_text);
}

// Render a single CP_BE_SECTION entry at the specified y coordinate.
// Section visual: divider line at y, "// name" label at y+2, divider
// at y+11. Total vertical cost: 13px (CP_BOOT_SECTION_H).
static void cp_render_section_at(const CpBootEntry &e, int y) {
    int16_t W = bootW();
    gfx->drawFastHLine(0, y, W, BOOT_DIVIDER);
    gfx->setTextColor(BOOT_SECTION);
    gfx->setTextSize(1);
    gfx->setCursor(4, y + 2);
    gfx->print("// ");
    gfx->print(e.sec_name);
    gfx->drawFastHLine(0, y + 11, W, BOOT_DIVIDER);
}

// Repaint the body region from the last N entries in the ring.
// Triggered when a new entry would overflow the visible area.
static void cp_boot_repaint_visible() {
    // Wipe the body region only (preserve header at y=0..17).
    gfx->fillRect(0, CP_BOOT_BODY_TOP, bootW(),
                  CP_BOOT_FOOTER_TOP - CP_BOOT_BODY_TOP, BOOT_BG);

    // Walk backward from the end, summing height, until we'd
    // exceed the visible budget. Then replay forward from that point.
    int budget = CP_BOOT_FOOTER_TOP - CP_BOOT_BODY_TOP;
    int first = cp_boot_ring_count;
    int total = 0;
    while (first > 0) {
        const CpBootEntry &e = cp_boot_ring[first - 1];
        int h = (e.kind == CP_BE_SECTION) ? CP_BOOT_SECTION_H : CP_BOOT_LINE_H;
        if (total + h > budget) break;
        total += h;
        first--;
    }

    int y = CP_BOOT_BODY_TOP;
    for (int i = first; i < cp_boot_ring_count; i++) {
        const CpBootEntry &e = cp_boot_ring[i];
        if (e.kind == CP_BE_SECTION) {
            cp_render_section_at(e, y);
            y += CP_BOOT_SECTION_H;
        } else if (e.kind == CP_BE_LINE) {
            cp_render_line_at(e, y);
            y += CP_BOOT_LINE_H;
        }
    }
    bootY = y;
}

// Record an entry into the ring with oldest-falls-off semantics.
static void cp_boot_record(const CpBootEntry &e) {
    if (cp_boot_ring_count < CP_BOOT_RING_SIZE) {
        cp_boot_ring[cp_boot_ring_count++] = e;
    } else {
        for (int i = 0; i < CP_BOOT_RING_SIZE - 1; i++) {
            cp_boot_ring[i] = cp_boot_ring[i + 1];
        }
        cp_boot_ring[CP_BOOT_RING_SIZE - 1] = e;
    }
}

// Returns true if a write of needed_h pixels at the current bootY
// would overflow the visible content area.
static bool cp_boot_needs_scroll(int needed_h) {
    return (bootY + needed_h) > CP_BOOT_FOOTER_TOP;
}

#endif // DEVICE_CARDPUTER_ADV

static void drawBootHeader() {
    int16_t W = bootW();
    gfx->fillRect(0, 0, W, 14, BOOT_HEADER_BG);
    gfx->drawFastHLine(0, 14, W, BOOT_DIVIDER);
    gfx->setTextSize(1);
    gfx->setTextColor(BOOT_OK);
    gfx->setCursor(4, 4);
    gfx->print("PISCES MOON OS");
    gfx->setTextColor(BOOT_SECTION);
    const char *bios = "BIOS v1.2.0 / ESP32-S3";
    gfx->setCursor(max(96, W - (int)strlen(bios) * 6 - 4), 4);
    gfx->print(bios);
    bootY = 18;
}

static void drawBootSection(const char* name) {
#ifdef DEVICE_CARDPUTER_ADV
    // Record the section in the ring buffer for replay.
    CpBootEntry e = {};
    e.kind = CP_BE_SECTION;
    strncpy(e.sec_name, name, sizeof(e.sec_name) - 1);
    cp_boot_record(e);

    // If this section would overflow, scroll first.
    if (cp_boot_needs_scroll(CP_BOOT_SECTION_H)) {
        cp_boot_repaint_visible();
        return;     // repaint handled this section
    }
    cp_render_section_at(e, bootY);
    bootY += CP_BOOT_SECTION_H;
    return;
#else
    int16_t W = bootW();
    gfx->drawFastHLine(0, bootY, W, BOOT_DIVIDER);
    bootY += 2;
    gfx->setTextColor(BOOT_SECTION);
    gfx->setTextSize(1);
    gfx->setCursor(4, bootY);
    gfx->print("// ");
    gfx->print(name);
    bootY += 9;
    gfx->drawFastHLine(0, bootY, W, BOOT_DIVIDER);
    bootY += 2;
#endif
}

static void drawBootLine(const char* timestamp, const char* label,
                         const char* detail, int tagType, const char* tagText) {
#ifdef DEVICE_CARDPUTER_ADV
    // Record the line in the ring buffer for replay.
    CpBootEntry e = {};
    e.kind = CP_BE_LINE;
    if (timestamp) strncpy(e.ts,    timestamp, sizeof(e.ts) - 1);
    if (label)     strncpy(e.label, label,     sizeof(e.label) - 1);
    if (detail)    strncpy(e.detail, detail,   sizeof(e.detail) - 1);
    e.tag_type = (uint8_t)tagType;
    if (tagText)   strncpy(e.tag_text, tagText, sizeof(e.tag_text) - 1);
    cp_boot_record(e);

    // If this line would overflow, scroll first.
    if (cp_boot_needs_scroll(CP_BOOT_LINE_H)) {
        cp_boot_repaint_visible();
        return;     // repaint handled this line
    }
    cp_render_line_at(e, bootY);
    bootY += CP_BOOT_LINE_H;
    return;
#else
    int16_t W = bootW();
    gfx->setTextSize(1);
    gfx->setTextColor(BOOT_TS);
    gfx->setCursor(4, bootY);
    gfx->print(timestamp);
    gfx->setTextColor(BOOT_LABEL);
    gfx->setCursor(36, bootY);
    gfx->print(label);
    if (detail != nullptr) {
        gfx->setTextColor(BOOT_ADDR);
        gfx->setCursor((W >= 420) ? W - 150 : 174, bootY);
        gfx->print(detail);
    }
    uint16_t tagFg, tagBg;
    switch (tagType) {
        case 1:  tagFg = BOOT_ACTIVE; tagBg = BOOT_ACTIVE_BG; break;
        case 2:  tagFg = BOOT_WARN;   tagBg = BOOT_WARN_BG;   break;
        case 3:  tagFg = BOOT_FAIL;   tagBg = BOOT_FAIL_BG;   break;
        default: tagFg = BOOT_OK;     tagBg = BOOT_OK_BG;     break;
    }
    int tagW = max(24, (int)(strlen(tagText) * 6) + 6);
    int tagX = W - tagW - 3;
    gfx->fillRect(tagX, bootY - 1, tagW, 9, tagBg);
    gfx->drawRect(tagX, bootY - 1, tagW, 9, tagFg);
    gfx->setTextColor(tagFg);
    gfx->setCursor(tagX + 3, bootY);
    gfx->print(tagText);
    bootY += 10;
#endif
}

static void drawBootProgress(int percent) {
    int16_t W = bootW();
    int barX = 4, barY = bootY + 2, barW = W - 8, barH = 5;
    gfx->drawRect(barX, barY, barW, barH, BOOT_DIVIDER);
    int fillW = (barW - 2) * percent / 100;
    gfx->fillRect(barX + 1, barY + 1, fillW, barH - 2, BOOT_PROGRESS);
    gfx->setTextColor(BOOT_SECTION);
    gfx->setTextSize(1);
    gfx->setCursor(4, barY + 7);
    gfx->print("LOADING PISCES SHELL");
    char pctStr[8];
    snprintf(pctStr, sizeof(pctStr), "%d%%", percent);
    gfx->setCursor(W - 20, barY + 7);
    gfx->print(pctStr);
    bootY = barY + 17;
}

static void drawBootFooter() {
    int16_t W = bootW();
    int16_t footerY = bootH() - 11;
    gfx->fillRect(0, footerY, W, 11, BOOT_HEADER_BG);
    gfx->drawFastHLine(0, footerY, W, BOOT_DIVIDER);
    gfx->setTextSize(1);
    gfx->setTextColor(BOOT_SECTION);
    gfx->setCursor(4, footerY + 3);
    gfx->print("CORE1 READY / CORE0 ACTIVE");
    gfx->setTextColor(BOOT_OK);
    gfx->setCursor(W - 72, footerY + 3);
    gfx->print("[ BOOT OK ]");
}

// ─────────────────────────────────────────────
//  SPLASH SCREEN
// ─────────────────────────────────────────────
void drawCircuitBackground() {
    int16_t W = bootW();
    int16_t H = bootH();
    gfx->fillScreen(0x0000);
    uint16_t gridColor  = 0x0120;
    uint16_t traceColor = 0x0300;
    for (int x = 0; x < W; x += 20) gfx->drawFastVLine(x, 0, H, gridColor);
    for (int y = 0; y < H; y += 20) gfx->drawFastHLine(0, y, W, gridColor);
    gfx->drawFastHLine(0,   10, 80,  traceColor);
    gfx->drawFastVLine(80,  10, 20,  traceColor);
    gfx->drawFastHLine(80,  30, 60,  traceColor);
    gfx->drawFastHLine(200, 10, 120, traceColor);
    gfx->drawFastVLine(200, 10, 15,  traceColor);
    gfx->drawFastHLine(220, 25, 60,  traceColor);
    gfx->drawFastHLine(60,  50, 40,  traceColor);
    gfx->drawFastVLine(60,  40, 10,  traceColor);
    gfx->drawFastHLine(240, 40, 80,  traceColor);
    gfx->drawFastVLine(280, 25, 15,  traceColor);
    gfx->drawFastHLine(0,   200, 60,  traceColor);
    gfx->drawFastVLine(60,  200, 20,  traceColor);
    gfx->drawFastHLine(60,  220, 100, traceColor);
    gfx->drawFastHLine(180, 210, 80,  traceColor);
    gfx->drawFastVLine(180, 210, 20,  traceColor);
    gfx->drawFastHLine(240, 230, 80,  traceColor);
    gfx->drawFastHLine(100, 195, 60,  traceColor);
    gfx->drawFastVLine(260, 195, 25,  traceColor);
    uint16_t padColor = 0x0460;
    gfx->fillRect(78,  28, 4, 4, padColor);
    gfx->fillRect(138, 28, 4, 4, padColor);
    gfx->fillRect(58,  48, 4, 4, padColor);
    gfx->fillRect(198, 23, 4, 4, padColor);
    gfx->fillRect(278, 23, 4, 4, padColor);
    gfx->fillRect(58,  198, 4, 4, padColor);
    gfx->fillRect(158, 218, 4, 4, padColor);
    gfx->fillRect(178, 208, 4, 4, padColor);
    gfx->fillRect(258, 193, 4, 4, padColor);
}

void drawOctagonFrame() {
    int16_t W = bootW();
    int16_t H = bootH();
    uint16_t frameColor = 0x0600;
    int cut = 18;
    int x1 = 8, y1 = 8;
    int x2 = W - 9, y2 = H - 9;
    gfx->drawFastHLine(x1 + cut, y1,       x2 - x1 - cut*2, frameColor);
    gfx->drawFastHLine(x1 + cut, y2,       x2 - x1 - cut*2, frameColor);
    gfx->drawFastVLine(x1,       y1 + cut, y2 - y1 - cut*2, frameColor);
    gfx->drawFastVLine(x2,       y1 + cut, y2 - y1 - cut*2, frameColor);
    gfx->drawLine(x1, y1 + cut, x1 + cut, y1, frameColor);
    gfx->drawLine(x2 - cut, y1, x2, y1 + cut, frameColor);
    gfx->drawLine(x1, y2 - cut, x1 + cut, y2, frameColor);
    gfx->drawLine(x2 - cut, y2, x2, y2 - cut, frameColor);
    gfx->drawFastHLine(x1 + cut + 1, y1 + 1, x2 - x1 - cut*2 - 2, frameColor);
    gfx->drawFastHLine(x1 + cut + 1, y2 - 1, x2 - x1 - cut*2 - 2, frameColor);
    gfx->drawFastVLine(x1 + 1, y1 + cut + 1, y2 - y1 - cut*2 - 2, frameColor);
    gfx->drawFastVLine(x2 - 1, y1 + cut + 1, y2 - y1 - cut*2 - 2, frameColor);
}

void drawChipIcon(int cx, int cy) {
    uint16_t body  = 0x0600;
    uint16_t pins  = 0x07E0;
    uint16_t inner = 0x0300;
    gfx->fillRect(cx - 12, cy - 8, 24, 16, body);
    gfx->drawRect(cx - 12, cy - 8, 24, 16, pins);
    gfx->drawFastHLine(cx - 8, cy - 3, 16, inner);
    gfx->drawFastHLine(cx - 8, cy,     16, inner);
    gfx->drawFastHLine(cx - 8, cy + 3, 16, inner);
    for (int i = -5; i <= 5; i += 4) gfx->drawFastHLine(cx - 16, cy + i, 4, pins);
    for (int i = -5; i <= 5; i += 4) gfx->drawFastHLine(cx + 12, cy + i, 4, pins);
    uint16_t tipColors[] = {0xF800, 0x07E0, 0x001F, 0xFFE0};
    for (int i = 0; i < 4; i++) {
        int py = cy - 5 + (i * 4);
        gfx->drawPixel(cx - 17, py, tipColors[i]);
        gfx->drawPixel(cx + 16, py, tipColors[3 - i]);
    }
}

void showRainbowSplash() {
    int16_t W = bootW();
    int16_t H = bootH();
    drawCircuitBackground();
    delay(150);
    drawOctagonFrame();
    delay(150);

    int centerX = W / 2;

#ifdef DEVICE_CARDPUTER_ADV
    // ── Cardputer 240×135 splash layout ──────────────────────
    // Tight vertical budget — order top-to-bottom:
    //   chip icon centered at y=22  (icon is ~16px tall, sits y=14-30)
    //   title baseline y=40         (size 3 text is ~24px tall → y=40-64)
    //   line1 (Powered by Gemini) at y=78
    //   line2 (Limited only by...) at y=98
    //   version footer at y=118
    drawChipIcon(centerX, 22);
    delay(200);

    const char* title = "Pisces Moon.";
    const char* line1 = "Powered by Gemini.";
    const char* line2 = "Limited only by your imagination.";
    uint16_t spectrum[] = {
        0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF,
    };
    int specLen = 8;

    int titleY = 40;
    int titleCharW = 18;
    int titleX = (W - (int)strlen(title) * titleCharW) / 2;

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

    int l1x = (W - (int)strlen(line1) * 6) / 2;
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(l1x, 78);
    gfx->print(line1);
    delay(400);

    int l2x = (W - (int)strlen(line2) * 6) / 2;
    gfx->setTextColor(0xC618);
    gfx->setCursor(l2x, 98);
    gfx->print(line2);
    delay(400);

    gfx->setTextColor(0x0480);
    const char* version = "v1.2.0 MULTI-DEVICE";
    int vx = W - (int)strlen(version) * 6 - 6;
    gfx->setCursor(vx, 118);
    gfx->print(version);

    esp_task_wdt_reset();
    delay(2500);
    esp_task_wdt_reset();
#else
    // ── T-Deck Plus / T-LoRa Pager splash (unchanged) ────────
    drawChipIcon(centerX, 72);
    delay(200);

    const char* title = "Pisces Moon.";
    const char* line1 = "Powered by Gemini.";
    const char* line2 = "Limited only by your imagination.";
    uint16_t spectrum[] = {
        0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF,
    };
    int specLen = 8;

    int titleY = min<int16_t>(110, H / 2 - 8);
    for (int cycle = 0; cycle < 12; cycle++) {
        gfx->setCursor((W - (int)strlen(title) * 18) / 2, titleY);
        gfx->setTextSize(3);
        for (int i = 0; i < (int)strlen(title); i++) {
            gfx->setTextColor(spectrum[(i + cycle) % specLen]);
            gfx->print(title[i]);
        }
        delay(80);
    }

    gfx->setCursor((W - (int)strlen(title) * 18) / 2, titleY);
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

    int l1x = (W - strlen(line1) * 6) / 2;
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(l1x, min<int16_t>(148, titleY + 38));
    gfx->print(line1);
    delay(400);

    int l2x = (W - strlen(line2) * 6) / 2;
    gfx->setTextColor(0xC618);
    gfx->setCursor(l2x, min<int16_t>(164, titleY + 54));
    gfx->print(line2);
    delay(400);

    gfx->setTextColor(0x0480);
    gfx->setCursor(max(4, W - 120), H - 20);
    gfx->print("v1.2.0 MULTI-DEVICE");

    esp_task_wdt_reset();
    delay(2500);
    esp_task_wdt_reset();
#endif
}

// --- GLOBALLY ACCESSIBLE UTILITIES ---
String get_text_input(int x, int y) {
    String input = "";
    gfx->setCursor(x, y);
    gfx->setTextColor(0xFFFF); 
#ifdef DEVICE_TLORAPAGER
    bool active = false;
    gfx->setTextColor(0x7BEF);
    gfx->print(PM_TEXT_COPY);
#endif
    while(true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 30) {
            while(get_touch(&tx, &ty)) { delay(10); yield(); } 
            return "##EXIT##"; 
        }
#ifdef DEVICE_TLORAPAGER
        TrackballState tb = update_trackball();
        char c = get_keypress();
        if (!active) {
            if (pm_is_exit_key(c)) return "##EXIT##";
            if (tb.clicked) {
                active = true;
                gfx->fillRect(x, y, gfx->width() - x, 16, 0x0000);
                gfx->setCursor(x, y);
                gfx->setTextColor(0xFFFF);
                gfx->print(input);
            }
            delay(15);
            yield();
            continue;
        }
        if (tb.clicked) break;
#else
        char c = get_keypress();
#endif
        if (c == 13 || c == 10) { 
            break;
        } else if (c == 8 || c == 127) { 
            if (input.length() > 0) {
                input.remove(input.length() - 1);
                gfx->fillRect(x, y, gfx->width() - x, 16, 0x0000);
                gfx->setCursor(x, y);
                gfx->print(input);
            }
        } else if (c >= 32 && c <= 126) { 
            input += c;
            gfx->print(c);
        }
        delay(15);
        yield();
    }
    return input;
}


// --- T-LORA PAGER XL9555 POWER MANAGEMENT ---
// On T-LoraPager every peripheral is power-gated through an XL9555 I2C
// GPIO expander at 0x20. They all boot in OFF state and must be enabled
// via I2C writes before SPI/UART/I2S work will function.
//
// Shadow registers track current state so we can flip individual bits
// without disturbing others.
#ifdef DEVICE_TLORAPAGER
static uint8_t _xl9555_p0 = 0x00;
static uint8_t _xl9555_p1 = 0x00;

static void xl9555_write_p0(uint8_t v) {
    _xl9555_p0 = v;
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(0x02); // output port 0 register
    Wire.write(v);
    Wire.endTransmission();
}
static void xl9555_write_p1(uint8_t v) {
    _xl9555_p1 = v;
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(0x03); // output port 1 register
    Wire.write(v);
    Wire.endTransmission();
}
void xl9555_set_bit(uint8_t bit, bool en) {
    if (bit < 8) {
        uint8_t v = en ? (_xl9555_p0 | (1<<bit)) : (_xl9555_p0 & ~(1<<bit));
        xl9555_write_p0(v);
    } else {
        uint8_t b = bit - 8;
        uint8_t v = en ? (_xl9555_p1 | (1<<b)) : (_xl9555_p1 & ~(1<<b));
        xl9555_write_p1(v);
    }
}
static void xl9555_init() {
    // Configure all XL9555 pins as outputs EXCEPT P12 (SD_DET) which is
    // a hardware input from the SD slot's card-detect switch.
    // P12 = port 1, bit 2 = absolute bit 10.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(0x06); Wire.write(0x00); // config port 0 = all outputs
    Wire.endTransmission();
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(0x07); Wire.write(0x04); // config port 1: bit 2 (P12) = INPUT, rest output
    Wire.endTransmission();
    xl9555_write_p0(0x00);
    xl9555_write_p1(0x00);
    Serial.println("[HAL] XL9555 init — all peripherals off (P12 SD_DET as INPUT)");
}
static void xl9555_pin_input(uint8_t bit) {
    // Configure a single XL9555 pin as INPUT (high-impedance).
    // LilyGo's installSD path puts EXPANDS_SD_PULLEN in INPUT mode
    // rather than driving it HIGH — an external pull-up sets the level.
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    if (bit < 8) {
        // Read current port 0 config (reg 0x06), set bit, write back
        Wire.write(0x06);
        Wire.endTransmission(false);
        Wire.requestFrom(IOEXP_I2C_ADDR, 1);
        uint8_t cfg = Wire.available() ? Wire.read() : 0;
        cfg |= (1 << bit);
        Wire.beginTransmission(IOEXP_I2C_ADDR);
        Wire.write(0x06);
        Wire.write(cfg);
        Wire.endTransmission();
    } else {
        uint8_t b = bit - 8;
        Wire.write(0x07);
        Wire.endTransmission(false);
        Wire.requestFrom(IOEXP_I2C_ADDR, 1);
        uint8_t cfg = Wire.available() ? Wire.read() : 0;
        cfg |= (1 << b);
        Wire.beginTransmission(IOEXP_I2C_ADDR);
        Wire.write(0x07);
        Wire.write(cfg);
        Wire.endTransmission();
    }
}

// ─────────────────────────────────────────────────────────────────
//  xl9555_readback_dump()
//  Reads the XL9555 INPUT registers (0x00, 0x01) which reflect the
//  actual electrical state of every pin regardless of direction.
//  Compares against the shadow registers we've been writing.
//
//  If shadow says 1 (we asked for HIGH) and the input register says 0,
//  the chip didn't accept the write, the pin is shorted to GND, or
//  the I/O config was reset.
//
//  If shadow says 1 AND the input register says 1, the XL9555 is
//  doing its job — and any downstream failure (e.g., SD card not
//  responding) is past the expander.
// ─────────────────────────────────────────────────────────────────
static void xl9555_readback_dump() {
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(0x00);  // Input port 0
    uint8_t err = Wire.endTransmission(false);
    if (err != 0) {
        Serial.printf("[XL9555-DIAG] I2C error %d while addressing reg 0x00\n", err);
        return;
    }
    Wire.requestFrom(IOEXP_I2C_ADDR, 1);
    uint8_t in_p0 = Wire.available() ? Wire.read() : 0xFF;

    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(0x01);  // Input port 1
    err = Wire.endTransmission(false);
    if (err != 0) {
        Serial.printf("[XL9555-DIAG] I2C error %d while addressing reg 0x01\n", err);
        return;
    }
    Wire.requestFrom(IOEXP_I2C_ADDR, 1);
    uint8_t in_p1 = Wire.available() ? Wire.read() : 0xFF;

    // Also read back the config registers to confirm output mode is set
    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(0x06);
    Wire.endTransmission(false);
    Wire.requestFrom(IOEXP_I2C_ADDR, 1);
    uint8_t cfg_p0 = Wire.available() ? Wire.read() : 0xFF;

    Wire.beginTransmission(IOEXP_I2C_ADDR);
    Wire.write(0x07);
    Wire.endTransmission(false);
    Wire.requestFrom(IOEXP_I2C_ADDR, 1);
    uint8_t cfg_p1 = Wire.available() ? Wire.read() : 0xFF;

    Serial.println("[XL9555-DIAG] ====== XL9555 STATE DUMP ======");
    Serial.printf ("[XL9555-DIAG] Port 0  shadow=0x%02X  input=0x%02X  cfg=0x%02X (0=output)\n",
                   _xl9555_p0, in_p0, cfg_p0);
    Serial.printf ("[XL9555-DIAG] Port 1  shadow=0x%02X  input=0x%02X  cfg=0x%02X (0=output)\n",
                   _xl9555_p1, in_p1, cfg_p1);

    // Build a "by name" readback for every enable bit we care about
    struct PinInfo { const char* name; uint8_t bit; const char* purpose; };
    static const PinInfo pins[] = {
        { "KEY_RST",    2,  "Keyboard reset (out of reset = HIGH)" },
        { "LORA_EN",    3,  "LoRa power"                            },
        { "GPS_EN",     4,  "GPS power"                             },
        { "NFC_EN",     5,  "NFC power"                             },
        { "LCD_RST",    6,  "LCD reset (out of reset = HIGH)"       },
        { "GPS_RST",    7,  "GPS reset (out of reset = HIGH)"       },
        { "KB_EN",      8,  "Keyboard power"                        },
        { "NRF_CE",     9,  "nRF24L01 CE"                           },
        { "SD_DET",     10, "Card detect (LOW = card inserted)"     },
        { "SPI_PULLUP", 11, "SPI pull-up enable (INPUT mode)"       },
        { "SD_EN",      12, "**SD CARD POWER**"                     },
    };
    for (size_t i = 0; i < sizeof(pins)/sizeof(pins[0]); i++) {
        uint8_t bit = pins[i].bit;
        bool shadow, actual, cfg_output;
        if (bit < 8) {
            shadow     = (_xl9555_p0 >> bit) & 1;
            actual     = (in_p0 >> bit) & 1;
            cfg_output = !((cfg_p0 >> bit) & 1);  // cfg bit 0 = output, 1 = input
        } else {
            uint8_t b = bit - 8;
            shadow     = (_xl9555_p1 >> b) & 1;
            actual     = (in_p1 >> b) & 1;
            cfg_output = !((cfg_p1 >> b) & 1);
        }
        const char* dir = cfg_output ? "out" : "INPUT";
        const char* status;
        if (shadow == actual) {
            status = (shadow ? "OK-HIGH" : "OK-LOW");
        } else {
            status = "MISMATCH";
        }
        Serial.printf("[XL9555-DIAG]   bit %2d %-12s %-5s shadow=%d actual=%d  %s  (%s)\n",
                      bit, pins[i].name, dir, shadow ? 1 : 0, actual ? 1 : 0,
                      status, pins[i].purpose);
    }
    Serial.println("[XL9555-DIAG] ===============================");
}

static void xl9555_boot_sequence() {
    // SD first — filesystem needed before Ghost Engine starts logging.
    // SD card needs ~150-250ms after power-on before SPI commands work.
    xl9555_set_bit(XL9555_SD_EN, true);
    delay(250);
    Serial.println("[HAL] SD power ON (P14)");
    // LoRa — Ghost Engine needs SX1262 reachable
    xl9555_set_bit(XL9555_LORA_EN, true);
    delay(20);
    Serial.println("[HAL] LoRa power ON (P03)");
    // GPS — held in reset then released
    xl9555_set_bit(XL9555_GPS_EN, true);
    delay(20);
    xl9555_set_bit(XL9555_GPS_RST, true); // out of reset (P07)
    Serial.println("[HAL] GPS power ON (P04) + RST released (P07)");
    // Keyboard — TCA8418. Power up rail first, then release reset.
    // KEY_RST is active-LOW: set HIGH to bring chip out of reset.
    xl9555_set_bit(XL9555_KB_EN, true);
    delay(20);
    Serial.println("[HAL] Keyboard power ON (P10)");
    xl9555_set_bit(XL9555_KEY_RST, true);  // out of reset (P02)
    delay(50);
    Serial.println("[HAL] Keyboard reset released (P02)");
    // LCD reset release — held LOW at boot, needs release before display init
    xl9555_set_bit(XL9555_LCD_RST, true);
    delay(20);
    Serial.println("[HAL] LCD reset released (P06)");
    // SPI pull-up enable — LilyGo's installSD sets this pin as INPUT,
    // letting an external pull-up define the level. On this hardware the
    // line drifts LOW after boot (likely a power-management circuit
    // re-asserts it), which disables the SD bus pull-ups and kills the
    // SD card.
    //
    // We override LilyGo's pattern and drive P13 OUTPUT HIGH explicitly.
    // The pin is still in OUTPUT mode by default from xl9555_init (which
    // configured all pins as outputs). We just set the bit HIGH.
    xl9555_set_bit(XL9555_SPI_PULLUP, true);
    delay(5);
    Serial.println("[HAL] SPI pull-up enable forced HIGH (P13, OUTPUT)");

    // P11 (bit 9) — LilyGo names this EXPANDS_GPIO_EN, NOT NRF_CE.
    // It is asserted HIGH at boot in their reference firmware regardless
    // of whether an nRF24 shield is connected. This bit may control a
    // level-shifter or buffer enable on the expansion bus, including
    // possibly the SD card data path. Pisces Moon was leaving this LOW
    // (treating it as nRF24-only). We follow LilyGo's pattern instead.
    xl9555_set_bit(XL9555_NRF_CE, true);
    delay(5);
    Serial.println("[HAL] Expansion GPIO enable forced HIGH (P11, was thought to be NRF_CE)");

    // NFC — last, not needed for boot, low priority
    xl9555_set_bit(XL9555_NFC_EN, true);
    delay(20);
    Serial.println("[HAL] NFC power ON (P05)");
}
#endif // DEVICE_TLORAPAGER

// ---------------------------------------------------------
// --- ISOLATED SUBSYSTEMS (NON-BLOCKING / ON-DEMAND) ---
// ---------------------------------------------------------
void setupWiFiAndScanner() {
    Serial.println("[SYSTEM] Attempting Wi-Fi Autoconnect...");
    // Call your existing wifi manager function 
    auto_connect_wifi(); 

    unsigned long startAttemptTime = millis();

    // NON-BLOCKING WAIT: Will give up after WIFI_TIMEOUT_MS
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < WIFI_TIMEOUT_MS) {
        delay(100); 
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("[SYSTEM] Wi-Fi Connected.");
        isWiFiConnected = true;
    } else {
        Serial.println("[SYSTEM] Autoconnect Timeout. Dropping to Wardriving/Scanner Mode.");
        isWiFiConnected = false;
        WiFi.disconnect(); 
        // Wardrive core takes over via init_wardrive_core() which was already activated in setup
    }
}


// --- SYSTEM INITIALIZATION (CORE 1) ---
void setup() {
    Serial.begin(115200);
    // Give USB-CDC serial a moment to attach so we don't lose the first
    // ~half-second of boot output to a still-enumerating host.
    delay(1500);
    Serial.println("[SYSTEM] === BOOT ===");
    esp_task_wdt_reset();

    // ── SPI Bus Treaty — create the shared mutex FIRST ─────
    // Display, SD, LoRa, NFC all coordinate through this on T-LoraPager.
    // On T-Deck it's used by Core 0 wardrive vs Core 1 SD reads.
    spi_mutex = xSemaphoreCreateRecursiveMutex();
    Serial.println("[SYSTEM] SPI mutex created (Treaty).");

#ifdef DEVICE_TDECK_PLUS
    // 1. GUARANTEED SCREEN WAKE-UP
    pinMode(BOARD_POWERON, OUTPUT);
    digitalWrite(BOARD_POWERON, HIGH);
    delay(50); 

    // 2. SPI BUS PROTECTION
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH); 
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_TFT_CS, OUTPUT);
    digitalWrite(BOARD_TFT_CS, HIGH);

    // 2.5 EARLY TRACKBALL INIT
    // GPIO0 (TRK_CLICK) is the ESP32-S3 boot-strapping pin. Initialize
    // with INPUT_PULLUP before any radio/WiFi work to prevent phantom clicks.
    init_trackball();
    ghost_partition_check_boot_keys();   // Read GPIO combo before SD mount

    // 3. MASTER SPI INITIALIZATION 
    SPI.begin(BOARD_TFT_SCK, BOARD_TFT_MISO, BOARD_TFT_MOSI);
    
    // 4. SILENT VRAM FLUSH
    // Backlight stays OFF during init to prevent white flash.
    pinMode(BOARD_TFT_BL, OUTPUT);
    digitalWrite(BOARD_TFT_BL, LOW);   // Backlight OFF during flush

    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->fillScreen(0x0000);
    delay(50);

    // 5. IGNITE BACKLIGHT
    digitalWrite(BOARD_TFT_BL, HIGH);
#endif // DEVICE_TDECK_PLUS

#ifdef DEVICE_CARDPUTER_ADV
    // Cardputer ADV: display owns its LCD SPI pins; SD/LoRa cap use a
    // separate shared SPI pin group and remain app/SD-init responsibilities.
    //
    // SPI BUS ROUTING (important for Mesh Messenger):
    //   FSPI / default `SPI` instance  → LCD only        (pins 35/36)
    //   HSPI / `cardputerSdSPI`        → SD + Cap LoRa   (pins 14/39/40)
    //   The Cap LoRa SX1262 is on the HSPI bus alongside SD card.
    //   Both share the bus per SPI Bus Treaty (chip-select differs).
    //   Mesh Messenger MUST use cardputerSdSPI, NOT default SPI, when
    //   talking to the SX1262 on Cardputer.
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_TFT_CS, OUTPUT);
    digitalWrite(BOARD_TFT_CS, HIGH);

    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    init_keyboard();

    // ── Cap LoRa-1262 RF switch enable ──────────────────────────────
    // The Cap LoRa-1262 routes the SX1262's RF signal through an
    // antenna switch gated by P0 of a PI4IOE5V6408 I/O expander at
    // I2C 0x43. Without writing P0 HIGH, the radio is electrically
    // isolated from the antenna and TX/RX produces nothing on-air.
    //
    // pi4ioe_cap_init() returns false harmlessly when no expander is
    // present (Cap LoRa-868 variant or no Cap attached), so calling
    // this unconditionally is safe.
    pi4ioe_cap_init();

    SPI.begin(BOARD_TFT_SCK, BOARD_TFT_MISO, BOARD_TFT_MOSI);
    pinMode(BOARD_TFT_BL, OUTPUT);
    digitalWrite(BOARD_TFT_BL, LOW);

    gfx->begin();
    gfx->fillScreen(0x0000);
    gfx->fillScreen(0x0000);
    delay(50);
    digitalWrite(BOARD_TFT_BL, HIGH);
    Serial.println("[HAL] Cardputer ADV display init complete");
#endif // DEVICE_CARDPUTER_ADV

#ifdef DEVICE_TLORAPAGER
    // ── T-LoraPager boot sequence ──────────────────────────
    // 1. SPI bus chip-select protection — all CS lines HIGH first
    //    Per LilyGo's initShareSPIPins() reference: include LORA_RST.
    //    The SX1262 must be out of reset (RST HIGH) for its SPI pins to
    //    tri-state properly. If LORA_RST is floating LOW, the chip is in
    //    reset and may weakly drive MISO, contesting the shared bus.
    pinMode(BOARD_LORA_CS, OUTPUT); digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS,   OUTPUT); digitalWrite(BOARD_SD_CS,   HIGH);
    pinMode(BOARD_TFT_CS,  OUTPUT); digitalWrite(BOARD_TFT_CS,  HIGH);
    pinMode(BOARD_NFC_CS,  OUTPUT); digitalWrite(BOARD_NFC_CS,  HIGH);
    pinMode(BOARD_LORA_RST,OUTPUT); digitalWrite(BOARD_LORA_RST,HIGH);

    // 2. I2C bus up first — XL9555 needs to power-gate everything else
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    delay(50);

    // 3. XL9555 init + boot sequence — turn on SD, LoRa, GPS, KB, NFC
    //    All bit numbers VERIFIED from schematic page 3.
    xl9555_init();
    xl9555_boot_sequence();

    // 3a. XL9555 STATE DUMP — verify every rail is actually HIGH.
    //     This is the truth source for "did the SD card actually get power?"
    //     Compares shadow registers to actual input register state.
    xl9555_readback_dump();

    // 4. Keyboard hardware init (TCA8418 matrix config)
    //    Must come AFTER xl9555_boot_sequence so KEY_EN rail is hot.
    init_keyboard();

    // 5. Encoder + synthesizer init
    init_trackball();

    // 6. SPI bus init
    SPI.begin(BOARD_TFT_SCK, BOARD_TFT_MISO, BOARD_TFT_MOSI);

    // 6a. GPIO MATRIX INTROSPECTION
    //     Read the GPIO matrix and IO_MUX registers DIRECTLY to confirm
    //     which peripheral function actually owns pins 33/34/35 after
    //     SPI.begin(). If func_sel == 256 (or whatever the "GPIO" sentinel
    //     is) the pin is NOT routed to SPI — SPI.begin silently failed
    //     to claim it, and our SD transactions are going nowhere.
    //
    //     Reference: ESP32-S3 TRM section 6 (IO MUX and GPIO matrix).
    //     GPIO_FUNCx_OUT_SEL_CFG_REG  selects which peripheral OUTPUT
    //     signal drives each pad. 256 = direct GPIO (no peripheral).
    //     Anything else = a peripheral signal index.
    //
    //     For FSPI on ESP32-S3 (the SPI controller Arduino's SPI uses):
    //       FSPICLK_out  = signal 63
    //       FSPID_out    = signal 64   (MOSI direction)
    //       FSPIQ_in     = signal 64   (MISO direction)
    //     If we see 63/64 on those pins, SPI claimed them correctly.
    //     If we see 256 or something else, SPI.begin() failed silently.
    {
        auto func_out_of = [](int pin) -> uint32_t {
            return GPIO.func_out_sel_cfg[pin].func_sel;
        };
        // Input multiplexer: for each peripheral input signal index,
        // GPIO.func_in_sel_cfg[signal].func_sel says WHICH PIN feeds it.
        // For SPI2_Q_in (MISO direction) the signal index is 102.
        auto func_in_pin_for_signal = [](int signal_idx) -> uint32_t {
            return GPIO.func_in_sel_cfg[signal_idx].func_sel;
        };
        Serial.println("[SPI-INTROSPECT] === GPIO matrix after SPI.begin() ===");
        Serial.printf("[SPI-INTROSPECT] GPIO%-2d (SCK)  func_out_sel=%lu\n",
                      BOARD_TFT_SCK,  (unsigned long)func_out_of(BOARD_TFT_SCK));
        Serial.printf("[SPI-INTROSPECT] GPIO%-2d (MOSI) func_out_sel=%lu\n",
                      BOARD_TFT_MOSI, (unsigned long)func_out_of(BOARD_TFT_MOSI));
        Serial.printf("[SPI-INTROSPECT] GPIO%-2d (MISO) func_out_sel=%lu\n",
                      BOARD_TFT_MISO, (unsigned long)func_out_of(BOARD_TFT_MISO));
        Serial.printf("[SPI-INTROSPECT] GPIO%-2d (SD_CS-manual) func_out_sel=%lu\n",
                      BOARD_SD_CS,    (unsigned long)func_out_of(BOARD_SD_CS));
        // CRITICAL: MISO is an INPUT. The output-select reads 256 (plain GPIO)
        // because nothing is driving it from the chip. What matters is which
        // pin the SPI2 peripheral's input multiplexer is reading FROM.
        // Signal 102 = SPI2_Q_in (MISO) on ESP32-S3.
        Serial.printf("[SPI-INTROSPECT] SPI2_Q_in (MISO input) is wired from GPIO%lu\n",
                      (unsigned long)func_in_pin_for_signal(102));
        Serial.println("[SPI-INTROSPECT] Expected: GPIO33 (== BOARD_TFT_MISO)");
        // Arduino-ESP32 core version + default SPI pin macros.
        // If the T_LORA_PAGER board variant didn't load, these will show
        // the esp32-s3-devkitc-1 defaults (SS=10, MOSI=11, MISO=13, SCK=12)
        // — NOT the T-LoraPager's actual pins. That would prove the
        // variant header is being ignored by the core.
        Serial.printf("[SPI-INTROSPECT] ESP_ARDUINO_VERSION=%d.%d.%d  (LilyGo requires >= 3.3.0)\n",
                      ESP_ARDUINO_VERSION_MAJOR,
                      ESP_ARDUINO_VERSION_MINOR,
                      ESP_ARDUINO_VERSION_PATCH);
        Serial.printf("[SPI-INTROSPECT] Variant default SPI macros: SCK=%d MISO=%d MOSI=%d SS=%d\n",
                      SCK, MISO, MOSI, SS);
        Serial.println("[SPI-INTROSPECT] If SS=10/MOSI=11/MISO=13/SCK=12, the T_LORA_PAGER variant did NOT load");
        Serial.println("[SPI-INTROSPECT] (devkit defaults active — variant header silently ignored by core)");
        Serial.println("[SPI-INTROSPECT] Output legend: 101=SPI2_CLK, 103=SPI2_D (MOSI),");
        Serial.println("[SPI-INTROSPECT]                256=plain GPIO (correct for MISO+CS)");
        Serial.println("[SPI-INTROSPECT] ===========================================");
    }

    // 7. Display init.
    // PMDispTLoRaPager::begin() runs LilyGo's full 19-command
    // init sequence internally and applies the 480x222
    // user-facing landscape tuple: MADCTL=0xE8, y-offset 49.
    // Backlight is left OFF; we enable it after a blank fill.
    gfx->begin();
    gfx->setSharedMutex(spi_mutex);   // Join the SPI Bus Treaty
    gfx->fillScreen(0x0000);
    gfx->fillScreen(0x0000);
    delay(50);

    // 8. Backlight ON — display ready
    gfx->setBacklight(true);
    Serial.println("[HAL] T-LoraPager display init complete");
#endif // DEVICE_TLORAPAGER

    delay(150);
    esp_task_wdt_reset();

    // ─────────────────────────────────────────────
    //  BIOS BOOT SCREEN — all text rendered BEFORE
    //  corresponding init calls to prevent GFX cursor
    //  corruption from WiFi SDK SPI state changes.
    // ─────────────────────────────────────────────
    drawBootHeader();
    drawBootSection("MEMORY MAP");

    drawBootLine("00:00", "HIMEM.SYS",         "0x00008000", 0, "OK");
    delay(60);
    drawBootLine("00:01", "Shadow RAM",         "0x00010000", 0, "OK");
    delay(60);
    drawBootLine("00:02", "L2 Cache",           "0x00045000", 0, "OK");
    delay(60);

    drawBootSection("PERIPHERALS");

    // 6. PMU (T-Deck only — T-LoraPager uses BQ25896 via XL9555 instead)
#ifdef DEVICE_TDECK_PLUS
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    if (PMU.begin(Wire, AXP2101_SLAVE_ADDRESS, BOARD_I2C_SDA, BOARD_I2C_SCL)) {
        PMU.clearIrqStatus();
        PMU.enableBattVoltageMeasure(); 
        PMU.enableVbusVoltageMeasure();
        PMU.enableSystemVoltageMeasure();
        PMU.setChargingLedMode(XPOWERS_CHG_LED_OFF);
        PMU.setALDO4Voltage(3300); PMU.enableALDO4();
        PMU.setBLDO1Voltage(3300); PMU.enableBLDO1();
        Serial.println("[SYSTEM] PMU activated.");
    }
#endif
    esp_task_wdt_reset();

    drawBootLine("00:03", "CPU ESP32-S3 240MHz", nullptr, 0, "DONE");
    delay(80);
#ifdef DEVICE_TDECK_PLUS
    drawBootLine("00:04", "I2C SDA:18 SCL:8",    nullptr, 0, "OK");
#elif defined(DEVICE_TLORAPAGER)
    drawBootLine("00:04", "I2C SDA:3 SCL:2",     nullptr, 0, "OK");
#else
    drawBootLine("00:04", "I2C SDA:8 SCL:9",     nullptr, 0, "OK");
#endif
    delay(80);

    // Touch (T-Deck has GT911; T-LoraPager has no touch — uses encoder + keyboard)
#ifdef DEVICE_TDECK_PLUS
    bool touchOk = init_touch();
    drawBootLine("00:05", "GT911 Touch Pulse",    nullptr, touchOk ? 0 : 3, touchOk ? "OK" : "FAIL");
#elif defined(DEVICE_TLORAPAGER)
    drawBootLine("00:05", "ROTARY ENCODER",      nullptr, 0, "OK");
#else
    drawBootLine("00:05", "TCA8418 Keyboard",     nullptr, 0, "OK");
#endif
    delay(80);
    esp_task_wdt_reset();

    // SD card — Ghost Partition aware mount
    // Falls back gracefully to single partition if Ghost not enabled/detected.
#ifdef DEVICE_TLORAPAGER
    // LilyGo's initShareSPIPins() asserts every shared-bus CS pin HIGH
    // AND drives LORA_RST HIGH. The LoRa chip must be out of reset before
    // SD operations — otherwise it can contest MISO from the shared bus.
    pinMode(BOARD_LORA_RST, OUTPUT);
    digitalWrite(BOARD_LORA_RST, HIGH);
    digitalWrite(BOARD_LORA_CS, HIGH);
    digitalWrite(BOARD_SD_CS,   HIGH);
    digitalWrite(BOARD_TFT_CS,  HIGH);
    digitalWrite(BOARD_NFC_CS,  HIGH);
    delay(10);

    // ─────────────────────────────────────────────────────────────────
    //  SD DIAGNOSTIC — T-LoraPager only. Disabled by default in v1.2.0+.
    //  Define PISCES_SD_DEBUG in build_flags to re-enable.
    //
    //  This block was instrumental in discovering the Arduino-ESP32
    //  2.0.16 → 2.0.17 SPI/SD bug that prevented cold-boot SD mount on
    //  T-LoraPager. We keep the code intact for future device ports
    //  (Cardputer ADV, etc.) since SD-on-shared-SPI debugging is hard
    //  to redo from scratch.
    //
    //  When enabled, this runs three SD probes (Arduino SD @ 4MHz,
    //  SdFat @ 4MHz, SdFat @ 10MHz) and prints results to Serial. It
    //  also adds ~5s to boot time and freezes the boot screen on the
    //  Rotary Encoder line for that duration. Don't ship enabled.
    // ─────────────────────────────────────────────────────────────────
#ifdef PISCES_SD_DEBUG
    Serial.println("[SD-DIAG] === T-LoraPager SD diagnostic ===");
    Serial.printf ("[SD-DIAG] BOARD_SD_CS=%d  BOARD_TFT_SCK=%d  MISO=%d  MOSI=%d\n",
                   BOARD_SD_CS, BOARD_TFT_SCK, BOARD_TFT_MISO, BOARD_TFT_MOSI);
    Serial.printf ("[SD-DIAG] XL9555 SD_EN bit=%d (P14)\n", XL9555_SD_EN);

    // XL9555 state dump RIGHT HERE — late in boot so serial monitor has
    // definitely attached. Same function we call earlier, but the output
    // is guaranteed to be visible to the host now.
    xl9555_readback_dump();

    // ─────────────────────────────────────────────────────────────────
    //  RAW PIN WIGGLE TEST — directly toggle each SD-related GPIO and
    //  read it back via digitalRead. If we set HIGH and read LOW (or
    //  vice versa) we know the ESP32 can't actually drive that pin —
    //  pin number wrong, trace broken, or pin is strapped to something.
    //
    //  Note: digitalRead on a pin configured as OUTPUT returns the
    //  value the chip is driving. If the external load is too strong
    //  (e.g., shorted to GND), the readback may still match what we
    //  wrote because the ESP32 reads its own output buffer, not the
    //  external level. So this catches code-level bugs (wrong pin)
    //  but not all hardware shorts. Better than nothing.
    // ─────────────────────────────────────────────────────────────────
    Serial.println("[PIN-TEST] === Raw GPIO wiggle test ===");
    auto wiggle_test = [](const char* name, int pin) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);
        delay(1);
        int read_low = digitalRead(pin);
        digitalWrite(pin, HIGH);
        delay(1);
        int read_high = digitalRead(pin);
        bool ok = (read_low == 0 && read_high == 1);
        Serial.printf("[PIN-TEST] %-12s GPIO%-2d  LOW->%d HIGH->%d  %s\n",
                      name, pin, read_low, read_high, ok ? "OK" : "FAIL");
    };
    wiggle_test("BOARD_SD_CS",   BOARD_SD_CS);
    wiggle_test("BOARD_TFT_CS",  BOARD_TFT_CS);
    wiggle_test("BOARD_LORA_CS", BOARD_LORA_CS);
    wiggle_test("BOARD_NFC_CS",  BOARD_NFC_CS);
    wiggle_test("BOARD_TFT_MOSI",BOARD_TFT_MOSI);
    wiggle_test("BOARD_TFT_SCK", BOARD_TFT_SCK);

    // MISO test is special — it's an INPUT from the SD card's perspective.
    // We read the line in two configurations:
    //   1. INPUT (no pullup) — should be HIGH if card is driving it idle, or undefined if floating
    //   2. INPUT_PULLDOWN   — should be HIGH if SD card is driving it idle, LOW if no driver
    pinMode(BOARD_TFT_MISO, INPUT);
    delay(1);
    int miso_float = digitalRead(BOARD_TFT_MISO);
    pinMode(BOARD_TFT_MISO, INPUT_PULLDOWN);
    delay(1);
    int miso_pulldown = digitalRead(BOARD_TFT_MISO);
    pinMode(BOARD_TFT_MISO, INPUT_PULLUP);
    delay(1);
    int miso_pullup = digitalRead(BOARD_TFT_MISO);
    Serial.printf("[PIN-TEST] BOARD_TFT_MISO GPIO%-2d  float->%d pulldown->%d pullup->%d\n",
                  BOARD_TFT_MISO, miso_float, miso_pulldown, miso_pullup);
    Serial.println("[PIN-TEST] (MISO idle: if SD-pullups OK & card present, all 3 should be 1)");
    Serial.println("[PIN-TEST] (MISO with no driver: float=?, pulldown=0, pullup=1 — line is floating)");

    // Restore MISO to INPUT before the real probes run
    pinMode(BOARD_TFT_MISO, INPUT);

    // Reassert all CS pins HIGH before any SPI traffic
    pinMode(BOARD_LORA_CS, OUTPUT); digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS,   OUTPUT); digitalWrite(BOARD_SD_CS,   HIGH);
    pinMode(BOARD_TFT_CS,  OUTPUT); digitalWrite(BOARD_TFT_CS,  HIGH);
    pinMode(BOARD_NFC_CS,  OUTPUT); digitalWrite(BOARD_NFC_CS,  HIGH);
    Serial.println("[PIN-TEST] === End raw GPIO wiggle test ===");
    delay(20);

    // ─────────────────────────────────────────────────────────────────
    //  SD POWER CYCLE + SPI WAKE-UP SEQUENCE
    //
    //  Before the probes, we:
    //    1. Power off the SD card (SD_EN LOW)
    //    2. Wait 100ms for any residual charge to bleed off
    //    3. Power on (SD_EN HIGH) and wait 250ms for card POR
    //    4. Force CS HIGH and send 80 clocks with MOSI HIGH.
    //       This is the standard SD-in-SPI-mode init: ≥74 clocks
    //       at CS-deasserted tell the card to switch to SPI mode.
    //
    //  This handles the case where the display's SPI traffic during
    //  init may have left the SD card in a half-initialized state.
    //  After this, the card should respond to CMD0 cleanly.
    // ─────────────────────────────────────────────────────────────────
    Serial.println("[SD-DIAG] Power-cycling SD card...");
    xl9555_set_bit(XL9555_SD_EN, false);
    delay(100);
    xl9555_set_bit(XL9555_SD_EN, true);
    delay(250);

    Serial.println("[SD-DIAG] Sending 80 idle clocks with CS HIGH...");
    if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
    digitalWrite(BOARD_LORA_CS, HIGH);
    digitalWrite(BOARD_SD_CS,   HIGH);
    digitalWrite(BOARD_TFT_CS,  HIGH);
    digitalWrite(BOARD_NFC_CS,  HIGH);
    // Send 10 bytes of 0xFF at 400kHz with CS deasserted → 80 SCK pulses.
    // This is the standard SD-card SPI-mode wake-up sequence.
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; i++) SPI.transfer(0xFF);
    SPI.endTransaction();
    if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
    delay(10);

    // ─────────────────────────────────────────────────────────────────
    //  RAW CMD0 PROBE — bypass the SD libraries entirely.
    //  Manually send the GO_IDLE_STATE command and read the response.
    //
    //  CMD0 frame:  0x40 0x00 0x00 0x00 0x00 0x95
    //               (cmd) (   arg=0          ) (crc7+1)
    //
    //  Expected response from a working card: 0x01 (idle state) within
    //  8 byte-times. If we see 0xFF forever, the card never drove MISO —
    //  either it's not responding, or MISO isn't routed to GPIO33.
    // ─────────────────────────────────────────────────────────────────
    Serial.println("[SD-DIAG] Raw CMD0 probe...");
    if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(BOARD_SD_CS, LOW);
    SPI.transfer(0x40);  // CMD0
    SPI.transfer(0x00);  // arg[31:24]
    SPI.transfer(0x00);  // arg[23:16]
    SPI.transfer(0x00);  // arg[15:8]
    SPI.transfer(0x00);  // arg[7:0]
    SPI.transfer(0x95);  // CRC7
    uint8_t responses[8];
    for (int i = 0; i < 8; i++) responses[i] = SPI.transfer(0xFF);
    digitalWrite(BOARD_SD_CS, HIGH);
    SPI.endTransaction();
    if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
    Serial.printf("[SD-DIAG] CMD0 response bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  responses[0], responses[1], responses[2], responses[3],
                  responses[4], responses[5], responses[6], responses[7]);
    Serial.println("[SD-DIAG] Expected: 0x01 within 8 bytes. 0xFF×8 = card silent.");

    // Probe A — Arduino SD library @ 4 MHz, mount point "/sd" (LilyGo's exact call).
    if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
    bool probeA_ok = SD.begin(BOARD_SD_CS, SPI, 4000000U, "/sd");
    if (probeA_ok) {
        uint64_t cardSize = SD.cardSize() / (1024ULL * 1024ULL);
        Serial.printf("[SD-DIAG] A: Arduino SD @ 4MHz  PASS  (%llu MB)\n", cardSize);
        SD.end();
    } else {
        Serial.println("[SD-DIAG] A: Arduino SD @ 4MHz  FAIL");
    }
    // Re-assert CS lines after the probe
    digitalWrite(BOARD_LORA_CS, HIGH);
    digitalWrite(BOARD_SD_CS,   HIGH);
    digitalWrite(BOARD_TFT_CS,  HIGH);
    digitalWrite(BOARD_NFC_CS,  HIGH);
    if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
    delay(20);
    drawBootLine("00:06a", "SD-PROBE-A Ard@4MHz", nullptr, probeA_ok ? 0 : 3, probeA_ok ? "OK" : "FAIL");
    delay(40);

    // Probe B — SdFat @ 4 MHz (same library as our real mount, but slower clock).
    {
        SdFat probeFat;
        SdSpiConfig cfgB(BOARD_SD_CS, SHARED_SPI, SD_SCK_MHZ(4), &SPI);
        if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
        bool probeB_ok = probeFat.begin(cfgB);
        if (probeB_ok) {
            Serial.println("[SD-DIAG] B: SdFat @ 4MHz     PASS");
            probeFat.end();
        } else {
            Serial.println("[SD-DIAG] B: SdFat @ 4MHz     FAIL");
        }
        digitalWrite(BOARD_LORA_CS, HIGH);
        digitalWrite(BOARD_SD_CS,   HIGH);
        digitalWrite(BOARD_TFT_CS,  HIGH);
        digitalWrite(BOARD_NFC_CS,  HIGH);
        if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
        delay(20);
        drawBootLine("00:06b", "SD-PROBE-B Fat@4MHz", nullptr, probeB_ok ? 0 : 3, probeB_ok ? "OK" : "FAIL");
        delay(40);
    }

    // Probe C — SdFat @ 10 MHz (current production setting, for comparison).
    {
        SdFat probeFat;
        SdSpiConfig cfgC(BOARD_SD_CS, SHARED_SPI, SD_SCK_MHZ(10), &SPI);
        if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
        bool probeC_ok = probeFat.begin(cfgC);
        if (probeC_ok) {
            Serial.println("[SD-DIAG] C: SdFat @ 10MHz    PASS");
            probeFat.end();
        } else {
            Serial.println("[SD-DIAG] C: SdFat @ 10MHz    FAIL");
        }
        digitalWrite(BOARD_LORA_CS, HIGH);
        digitalWrite(BOARD_SD_CS,   HIGH);
        digitalWrite(BOARD_TFT_CS,  HIGH);
        digitalWrite(BOARD_NFC_CS,  HIGH);
        if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
        delay(20);
        drawBootLine("00:06c", "SD-PROBE-C Fat@10MHz", nullptr, probeC_ok ? 0 : 3, probeC_ok ? "OK" : "FAIL");
        delay(40);
    }

    Serial.println("[SD-DIAG] === End diagnostic — proceeding to real mount ===");
#endif  // PISCES_SD_DEBUG
#endif  // DEVICE_TLORAPAGER

#ifdef DEVICE_TDECK_PLUS
    // ── T-Deck Plus: synchronous SD mount during boot ────────────────
    // T-Deck Plus has a cleaner SPI topology (only 2 peripherals on its
    // main bus) and the SD card responds reliably on cold-boot.
    // Boot path is unchanged here — keeps v1.2.0 behavior intact.
    bool sdMounted = false;
    for (int attempt = 1; attempt <= 2 && !sdMounted; attempt++) {
        sdMounted = ghost_partition_mount_public(BOARD_SD_CS, SPI);
        if (!sdMounted && attempt < 2) {
            Serial.printf("[SD] Mount attempt %d/2 failed — retrying in 1s\n", attempt);
            delay(1000);
        }
    }
    g_sd_ready = sdMounted;
    drawBootLine("00:06", "SD_CARD0 GPIO:39", nullptr, sdMounted ? 0 : 3, sdMounted ? "OK" : "FAIL");
    delay(80);
    if (sdMounted) {
        bool dbOk = init_database();
        drawBootLine("00:07", "VAULT INIT", nullptr, dbOk ? 0 : 3, dbOk ? "OK" : "FAIL");
        delay(80);
    }
#elif defined(DEVICE_CARDPUTER_ADV)
    // ── Cardputer ADV: SD is on a separate SPI pin group ─────────────
    // LCD uses GPIO35/36/37/34/38 via the global SPI object above.
    // microSD and the LoRa cap share GPIO14/39/40 with SD_CS=12, so
    // mounting SD through the display SPI bus will never see the card.
    pinMode(BOARD_LORA_CS, OUTPUT); digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS,   OUTPUT); digitalWrite(BOARD_SD_CS,   HIGH);
    cardputerSdSPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, BOARD_SD_CS);
    delay(50);

    bool sdMounted = false;
    for (int attempt = 1; attempt <= 3 && !sdMounted; attempt++) {
        digitalWrite(BOARD_LORA_CS, HIGH);
        digitalWrite(BOARD_SD_CS,   HIGH);
        sdMounted = ghost_partition_mount_public(BOARD_SD_CS, cardputerSdSPI);
        if (!sdMounted && attempt < 3) {
            Serial.printf("[SD] Cardputer mount attempt %d/3 failed — retrying\n", attempt);
            delay(500);
        }
    }
    g_sd_ready = sdMounted;
    drawBootLine("00:06", "SD_CARD0 GPIO:12", nullptr, sdMounted ? 0 : 3, sdMounted ? "OK" : "FAIL");
    delay(80);
    if (sdMounted) {
        bool dbOk = init_database();
        drawBootLine("00:07", "VAULT INIT", nullptr, dbOk ? 0 : 3, dbOk ? "OK" : "FAIL");
        delay(80);
    }
#else
    // ── T-LoraPager: defer SD mount entirely ─────────────────────────
    // T-LoraPager has four peripherals on one shared SPI bus and the
    // card frequently doesn't respond on cold-boot for ~3-5 seconds.
    // Apps that need SD (wardrive, audio, file manager, Gemini chat
    // logging) gate on g_sd_ready and gracefully wait or degrade.
    //
    // Boot path renders the launcher fast. A late-init task spawned
    // after Ghost Engine attempts the actual mount with retries.
    g_sd_ready = false;
    drawBootLine("00:06", "SD_CARD0 (deferred)", nullptr, 1, "LATE");
    delay(80);
    drawBootLine("00:07", "VAULT (deferred)", nullptr, 1, "LATE");
    delay(80);
#endif
    esp_task_wdt_reset();

    drawBootSection("PROCESS SPAWN");

#ifdef DEVICE_CARDPUTER_ADV
    // Cardputer ADV (no PSRAM): defer wardrive task spawn until user
    // explicitly launches the wardrive app. The task's NimBLE init
    // claims ~48KB of internal SRAM which the device cannot afford
    // to lock up at boot — the launcher and other apps need that
    // headroom. run_wardrive() will call init_wardrive_core() on
    // first entry. T-Deck and Pager keep boot-spawn behavior.
    drawBootLine("00:08", "WARDRIVE_CORE",         nullptr, 2, "DEFERRED");
    delay(60);
#else
    init_wardrive_core();
    drawBootLine("00:08", "WARDRIVE_CORE",         nullptr, 1, "ACTIVE");
    delay(60);
#endif

    // Gamepad BLE auto-reconnect is deprecated. Do not touch NimBLE here;
    // wardrive owns BLE startup/order.
    drawBootLine("00:09", "GAMEPAD_BLE",           nullptr, 2, "SKIPPED");

#ifdef DEVICE_CARDPUTER_ADV
    // ── Cardputer ADV: SKIP Gemini boot-time init ────────────────────
    // The Gemini client allocates HTTPS context, JSON history buffer,
    // and NoSQL "gemini" category at init — ~15KB total. On the no-
    // PSRAM Cardputer that 15KB matters for the wardrive + NimBLE
    // budget. init_gemini() is idempotent (see gemini_client.cpp:131
    // — the chat path auto-initializes if needed), so deferring boot
    // init is safe.
    drawBootLine("00:10", "GEMINI_CLIENT",         nullptr, 2, "DEFERRED");
    Serial.println("[SYSTEM] Cardputer: Gemini client deferred — "
                   "lazy-init at first chat entry");
#else
    init_gemini();
    drawBootLine("00:10", "GEMINI_CLIENT",         nullptr, 1, "ACTIVE");
#endif
    delay(60);

    // CRITICAL: render line FIRST, then call auto_connect_wifi() wrapper
    // WiFi SDK calls corrupt GFX cursor if text follows on same line
#ifdef DEVICE_CARDPUTER_ADV
    // ── Cardputer ADV: SKIP autoconnect ──────────────────────────────
    // On the no-PSRAM Cardputer, WiFi STA mode consumes ~52KB and
    // bringing it up at boot leaves insufficient memory for the
    // wardrive + NimBLE budget OR for the Gemini client to operate.
    //
    // v1.2 introduces WiFi mode-locking (see wifi_mode.cpp) which
    // enforces that WiFi can only be in ONE of two modes at a time:
    //   - SCANNER (wardrive, packet sniffer, etc.)
    //   - CLIENT  (Gemini, Bridge HTTP, file manager, WiFi connect)
    //
    // Neither mode is initialized at boot. The user explicitly
    // launches an app, and that app triggers the appropriate WiFi
    // mode init. Saved credentials persist on SD (wifi_manager.cpp),
    // so client-mode launches auto-associate without re-prompting.
    //
    // T-Deck Plus and Pager have 8MB PSRAM and no such constraint;
    // they keep boot-time autoconnect behavior (else branch below).
    drawBootLine("00:11", "WiFi Auto-Connect",     nullptr, 2, "DEFERRED");
    delay(60);
    Serial.println("[SYSTEM] Cardputer: WiFi autoconnect deferred — "
                   "user-initiated only (mode-lock policy)");
    isWiFiConnected = false;
#else
    drawBootLine("00:11", "WiFi Auto-Connect",     nullptr, 2, "TRIGGERED");
    delay(60);
    setupWiFiAndScanner(); // Non-blocking wrapper
#endif
    esp_task_wdt_reset();

    delay(200);
    xTaskCreatePinnedToCore(core0GhostTask, "GhostTask", 10000, NULL, 1, &GhostTask, 0);
    wardrive_active = true;   // Ghost Engine starts immediately — never stops
    drawBootLine("00:12", "CORE_0_GHOST",          nullptr, 1, "ACTIVE");
    delay(60);

#ifdef DEVICE_TLORAPAGER
    // Spawn the deferred SD mount task — runs on Core 1 at low priority.
    // Apps gate on g_sd_ready before attempting SD operations.
    xTaskCreatePinnedToCore(late_sd_init_task, "SDLateInit", 8192, NULL, 1, NULL, 1);
    drawBootLine("00:13", "SD_LATE_INIT",          nullptr, 1, "SPAWNED");
    delay(60);
#endif

    drawBootProgress(100);
    drawBootFooter();

    esp_task_wdt_reset();
    delay(1400);
    esp_task_wdt_reset();

    // 10. SPLASH SCREEN
    showRainbowSplash();
    esp_task_wdt_reset();

    // 11. GHOST PARTITION PIN SCREEN
    // No-op if GHOST_PARTITION_ENABLED not defined, boot key not held,
    // or no second partition detected on the card.
    ghost_partition_run_pin_screen();
    esp_task_wdt_reset();
}

// --- MAIN EXECUTIVE LOOP ---
void loop() {
    run_launcher();
    delay(100);
}
