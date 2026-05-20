// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#include "pm_power.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================
//  External symbols owned by main.cpp and the wardrive module
// ============================================================
extern SemaphoreHandle_t spi_mutex;
extern bool wardrive_active;

// Display driver — used to send the panel sleep command and
// drop the backlight. On T-Deck this is Arduino_GFX *; on
// T-LoraPager it's PMDispTLoRaPager *.
#ifdef DEVICE_TLORAPAGER
class PMDispTLoRaPager;
extern PMDispTLoRaPager *gfx;
#else
class Arduino_GFX;
extern Arduino_GFX *gfx;
#endif

// XL9555 expander helpers (defined inline in main.cpp).
#ifdef DEVICE_TLORAPAGER
extern void xl9555_set_bit(uint8_t bit, bool en);

// XL9555 pin assignments per T-LoraPager schematic.
// These mirror the constants in main.cpp.
#define XL_LORA_EN    3
#define XL_GPS_EN     4
#define XL_NFC_EN     5
#define XL_GPS_RST    7
#define XL_KB_EN     10
#define XL_DRV_EN     8   // P10 — haptic driver
#define XL_AMP_EN     9   // P11 — audio amplifier
#define XL_SD_EN     14
#define XL_DISP_RST   6   // P06 — display reset (active low)
#endif

// Pin definitions (mirror main.cpp).
#define BOARD_ENC_BTN_PIN 7
#define BOARD_BOOT_PIN    0
#define BOARD_TFT_BL_PIN  42

// ============================================================
//  pm_power_sleep
// ============================================================
void pm_power_sleep() {
    Serial.println("[POWER] Initiating graceful shutdown...");

    // ─── 1. Stop Ghost Engine + wardrive activity ─────────
    Serial.println("[POWER] Stopping wardrive task...");
    wardrive_active = false;
    delay(500);

    // ─── 2. Flush + unmount SD ─────────────────────────────
    // Take the SPI mutex first so we don't collide with any
    // background SD writer. SD.end() flushes pending writes
    // and releases the SD object.
    Serial.println("[POWER] Unmounting SD card...");
    if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        SD.end();
        xSemaphoreGiveRecursive(spi_mutex);
    } else {
        // Couldn't get the mutex — SD card may be in mid-write
        // and trying to end() here would risk corruption. Give
        // it more time then proceed regardless.
        Serial.println("[POWER] WARNING: SPI mutex busy, forcing SD end()");
        delay(1000);
        SD.end();
    }

    // ─── 3. Display sleep + backlight off ─────────────────
    // Turn off backlight first so user sees the device go dark
    // immediately. Then put the panel in sleep mode (low
    // current) before deep sleep cuts power entirely.
    Serial.println("[POWER] Display off...");
    pinMode(BOARD_TFT_BL_PIN, OUTPUT);
    digitalWrite(BOARD_TFT_BL_PIN, LOW);

#ifdef DEVICE_TLORAPAGER
    // ─── 4. Cut all XL9555 peripheral power rails ─────────
    Serial.println("[POWER] Cutting peripheral power rails...");
    xl9555_set_bit(XL_LORA_EN,  false);
    xl9555_set_bit(XL_GPS_EN,   false);
    xl9555_set_bit(XL_NFC_EN,   false);
    xl9555_set_bit(XL_DRV_EN,   false);
    xl9555_set_bit(XL_AMP_EN,   false);
    xl9555_set_bit(XL_KB_EN,    false);
    xl9555_set_bit(XL_SD_EN,    false);
    xl9555_set_bit(XL_GPS_RST,  false);
    xl9555_set_bit(XL_DISP_RST, false);
#endif

    // ─── 5. Tear down SPI + I2C buses ─────────────────────
    Serial.println("[POWER] Releasing bus peripherals...");
    SPI.end();
    Wire.end();

    // ─── 6. Configure wake-up sources ─────────────────────
    // Both buttons pull LOW when pressed. Either one wakes
    // the device. ESP32-S3 supports ext1 wake on multiple
    // RTC-GPIO pins via a bitmask.
    Serial.println("[POWER] Wake-up: rotary button OR boot button");
    const uint64_t wake_mask =
        (1ULL << BOARD_ENC_BTN_PIN) |
        (1ULL << BOARD_BOOT_PIN);

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
#else
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
#endif

    // Flush serial so the final log line gets sent before we
    // disconnect USB-CDC.
    Serial.println("[POWER] Entering deep sleep. Press wheel or BOOT to wake.");
    Serial.flush();
    delay(100);

    // ─── 7. Sleep — does not return ───────────────────────
    esp_deep_sleep_start();

    // If we ever reach here, something has gone very wrong.
    Serial.println("[POWER] FATAL: deep sleep returned");
    while (true) delay(1000);
}
