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
 * PROJECT: PISCES MOON OS v0.9
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
#include <Arduino_GFX_Library.h>
#include <XPowersLib.h>
#include <esp_task_wdt.h>    // WDT feed — prevents Guru Meditation during long setup()
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h> // SemaphoreHandle_t — spi_mutex for SD bus arbitration

// --- PISCES MOON CUSTOM HEADERS ---
#include "touch.h"       
#include "trackball.h"   // Needed for early GPIO0 init
#include "launcher.h"    
#include "wardrive.h"    
#include "keyboard.h"    
#include "gemini_client.h" 
#include "wifi_manager.h" 
#include "database.h"
#include "ghost_partition.h"  // Ghost Partition / PIN router system
#include "gamepad.h"          // 8BitDo Zero 2 BLE HID driver

// --- GLOBAL VARIABLES TO SATISFY THE LINKER ---
bool exitApp = false; 
XPowersAXP2101 PMU; 
SdFat sd;           
TinyGPSPlus gps;    

// --- SYSTEM STATE FLAGS (PISCES ARCHITECTURE) ---
bool isWiFiConnected = false;
const unsigned long WIFI_TIMEOUT_MS = 5000;  // 5 seconds max for autoconnect

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

// --- DRIVER INSTANTIATION ---
Arduino_DataBus *bus = new Arduino_HWSPI(BOARD_TFT_DC, BOARD_TFT_CS, BOARD_TFT_SCK, BOARD_TFT_MOSI, BOARD_TFT_MISO, &SPI, true);
Arduino_GFX *gfx = new Arduino_ST7789(bus, BOARD_TFT_RST, 1 /* Landscape */, true /* IPS */);

// --- MULTI-THREADING HANDLES ---
TaskHandle_t GhostTask;

// --- SPI BUS MUTEX ---
// Shared between Core 0 (wardrive SD writes) and Core 1 (system_app, wifi_filemgr SD reads).
// Any code touching sd.* must take this mutex first and release immediately after.
SemaphoreHandle_t spi_mutex = nullptr;

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

static int bootY = 0;

static void drawBootHeader() {
    gfx->fillRect(0, 0, 320, 14, BOOT_HEADER_BG);
    gfx->drawFastHLine(0, 14, 320, BOOT_DIVIDER);
    gfx->setTextSize(1);
    gfx->setTextColor(BOOT_OK);
    gfx->setCursor(4, 4);
    gfx->print("PISCES MOON OS");
    gfx->setTextColor(BOOT_SECTION);
    gfx->setCursor(192, 4);
    gfx->print("BIOS v0.9 / ESP32-S3");
    bootY = 18;
}

static void drawBootSection(const char* name) {
    gfx->drawFastHLine(0, bootY, 320, BOOT_DIVIDER);
    bootY += 2;
    gfx->setTextColor(BOOT_SECTION);
    gfx->setTextSize(1);
    gfx->setCursor(4, bootY);
    gfx->print("// ");
    gfx->print(name);
    bootY += 9;
    gfx->drawFastHLine(0, bootY, 320, BOOT_DIVIDER);
    bootY += 2;
}

static void drawBootLine(const char* timestamp, const char* label,
                         const char* detail, int tagType, const char* tagText) {
    gfx->setTextSize(1);
    gfx->setTextColor(BOOT_TS);
    gfx->setCursor(4, bootY);
    gfx->print(timestamp);
    gfx->setTextColor(BOOT_LABEL);
    gfx->setCursor(36, bootY);
    gfx->print(label);
    if (detail != nullptr) {
        gfx->setTextColor(BOOT_ADDR);
        gfx->setCursor(174, bootY);
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
    int tagX = 320 - tagW - 3;
    gfx->fillRect(tagX, bootY - 1, tagW, 9, tagBg);
    gfx->drawRect(tagX, bootY - 1, tagW, 9, tagFg);
    gfx->setTextColor(tagFg);
    gfx->setCursor(tagX + 3, bootY);
    gfx->print(tagText);
    bootY += 10;
}

static void drawBootProgress(int percent) {
    int barX = 4, barY = bootY + 2, barW = 312, barH = 5;
    gfx->drawRect(barX, barY, barW, barH, BOOT_DIVIDER);
    int fillW = (barW - 2) * percent / 100;
    gfx->fillRect(barX + 1, barY + 1, fillW, barH - 2, BOOT_PROGRESS);
    gfx->setTextColor(BOOT_SECTION);
    gfx->setTextSize(1);
    gfx->setCursor(4, barY + 7);
    gfx->print("LOADING PISCES SHELL");
    char pctStr[8];
    snprintf(pctStr, sizeof(pctStr), "%d%%", percent);
    gfx->setCursor(300, barY + 7);
    gfx->print(pctStr);
    bootY = barY + 17;
}

static void drawBootFooter() {
    gfx->fillRect(0, 229, 320, 11, BOOT_HEADER_BG);
    gfx->drawFastHLine(0, 229, 320, BOOT_DIVIDER);
    gfx->setTextSize(1);
    gfx->setTextColor(BOOT_SECTION);
    gfx->setCursor(4, 232);
    gfx->print("CORE1 READY / CORE0 ACTIVE");
    gfx->setTextColor(BOOT_OK);
    gfx->setCursor(248, 232);
    gfx->print("[ BOOT OK ]");
}

// ─────────────────────────────────────────────
//  SPLASH SCREEN
// ─────────────────────────────────────────────
void drawCircuitBackground() {
    gfx->fillScreen(0x0000);
    uint16_t gridColor  = 0x0120;
    uint16_t traceColor = 0x0300;
    for (int x = 0; x < 320; x += 20) gfx->drawFastVLine(x, 0, 240, gridColor);
    for (int y = 0; y < 240; y += 20) gfx->drawFastHLine(0, y, 320, gridColor);
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
    uint16_t frameColor = 0x0600;
    int cut = 18;
    int x1 = 8, y1 = 8;
    int x2 = 311, y2 = 231;
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
    drawCircuitBackground();
    delay(150);
    drawOctagonFrame();
    delay(150);

    int centerX = 160;
    drawChipIcon(centerX, 72);
    delay(200);

    const char* title = "Pisces Moon.";
    const char* line1 = "Powered by Gemini.";
    const char* line2 = "Limited only by your imagination.";
    uint16_t spectrum[] = {
        0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x001F, 0xF81F, 0xFFFF,
    };
    int specLen = 8;

    int titleY = 110;
    for (int cycle = 0; cycle < 12; cycle++) {
        gfx->setCursor(52, titleY);
        gfx->setTextSize(3);
        for (int i = 0; i < (int)strlen(title); i++) {
            gfx->setTextColor(spectrum[(i + cycle) % specLen]);
            gfx->print(title[i]);
        }
        delay(80);
    }

    gfx->setCursor(52, titleY);
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

    int l1x = (320 - strlen(line1) * 6) / 2;
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(l1x, 148);
    gfx->print(line1);
    delay(400);

    int l2x = (320 - strlen(line2) * 6) / 2;
    gfx->setTextColor(0xC618);
    gfx->setCursor(l2x, 164);
    gfx->print(line2);
    delay(400);

    gfx->setTextColor(0x0480);
    gfx->setCursor(248, 220);
    gfx->print("v0.9 ALPHA");

    esp_task_wdt_reset();
    delay(2500);
    esp_task_wdt_reset();
}

// --- GLOBALLY ACCESSIBLE UTILITIES ---
String get_text_input(int x, int y) {
    String input = "";
    gfx->setCursor(x, y);
    gfx->setTextColor(0xFFFF); 
    while(true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 30) {
            while(get_touch(&tx, &ty)) { delay(10); yield(); } 
            return "##EXIT##"; 
        }
        char c = get_keypress();
        if (c == 13 || c == 10) { 
            break;
        } else if (c == 8 || c == 127) { 
            if (input.length() > 0) {
                input.remove(input.length() - 1);
                gfx->fillRect(x, y, 320 - x, 16, 0x0000); 
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
    esp_task_wdt_reset();

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
    // The display VRAM contains garbage at boot — turning the
    // backlight on before fillScreen completes shows that garbage
    // as a white flash. Fill black first, settle, then ignite.
    pinMode(BOARD_TFT_BL, OUTPUT);
    digitalWrite(BOARD_TFT_BL, LOW);   // Backlight OFF during flush

    gfx->begin();
    gfx->fillScreen(0x0000);           // Write black to all VRAM
    gfx->fillScreen(0x0000);           // Second pass for reliability
    delay(50);                         // Let SPI transaction complete

    // 5. IGNITE BACKLIGHT — VRAM is clean black now
    digitalWrite(BOARD_TFT_BL, HIGH);

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

    // 6. PMU
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
    esp_task_wdt_reset();

    drawBootLine("00:03", "CPU ESP32-S3 240MHz", nullptr, 0, "DONE");
    delay(80);
    drawBootLine("00:04", "I2C SDA:18 SCL:8",    nullptr, 0, "OK");
    delay(80);

    // Touch
    bool touchOk = init_touch();
    drawBootLine("00:05", "GT911 Touch Pulse",    nullptr, touchOk ? 0 : 3, touchOk ? "OK" : "FAIL");
    delay(80);
    esp_task_wdt_reset();

    // SD card — Ghost Partition aware mount
    // Falls back gracefully to single partition if Ghost not enabled/detected.
    bool sdMounted = ghost_partition_mount_public(BOARD_SD_CS, SPI);
    drawBootLine("00:06", "SD_CARD0 GPIO:39",     nullptr, sdMounted ? 0 : 3, sdMounted ? "OK" : "FAIL");
    delay(80);

    if (sdMounted) {
        bool dbOk = init_database();
        drawBootLine("00:07", "SQLite Vault",      nullptr, dbOk ? 0 : 3, dbOk ? "OK" : "FAIL");
        delay(80);
    }
    esp_task_wdt_reset();

    drawBootSection("PROCESS SPAWN");

    // SPI mutex must exist before wardrive Core 0 starts touching the SD bus
    spi_mutex = xSemaphoreCreateMutex();
    Serial.println("[SYSTEM] SPI mutex created.");

    init_wardrive_core();
    drawBootLine("00:08", "WARDRIVE_CORE",         nullptr, 1, "ACTIVE");
    delay(60);

    // GAMEPAD INIT — must come AFTER init_wardrive_core() which owns the
    // NimBLE stack. gamepad_init() attaches to the existing NimBLE instance.
    // If a paired MAC is saved on SD it silently attempts reconnect at boot.
    gamepad_init();
    drawBootLine("00:09", "GAMEPAD_BLE",           nullptr,
                 gamepad_is_paired() ? 1 : 2,
                 gamepad_is_paired() ? "RECONNECT" : "STANDBY");

    init_gemini();
    drawBootLine("00:10", "GEMINI_CLIENT",         nullptr, 1, "ACTIVE");
    delay(60);

    // CRITICAL: render line FIRST, then call auto_connect_wifi() wrapper
    // WiFi SDK calls corrupt GFX cursor if text follows on same line
    drawBootLine("00:11", "WiFi Auto-Connect",     nullptr, 2, "TRIGGERED");
    delay(60);
    setupWiFiAndScanner(); // Now uses the non-blocking wrapper function
    esp_task_wdt_reset();

    delay(200);
    xTaskCreatePinnedToCore(core0GhostTask, "GhostTask", 10000, NULL, 1, &GhostTask, 0);
    drawBootLine("00:12", "CORE_0_GHOST",          nullptr, 1, "ACTIVE");
    delay(60);

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