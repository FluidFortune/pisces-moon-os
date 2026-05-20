// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef HAL_DISPLAY_H
#define HAL_DISPLAY_H

// ============================================================
//  hal_display.h — Per-device display initialization
//
//  Per-device display init. T-Deck/Cardputer use Arduino_GFX;
//  T-LoRaPager uses the standalone PMDispTLoRaPager driver.
//
//  All apps use the global `gfx` pointer after init.
//  Resolution constants SCREEN_W / SCREEN_H come from
//  hal_pins.h which is set by the build environment.
//
//  T-Deck Plus:   ST7789,  320x240, SPI
//  T-LoraPager:   ST7796U, 480x222, SPI (x-offset 49)
//  Cardputer ADV: ST7789,  240x135, SPI (TBD)
// ============================================================

#include "hal_pins.h"
#include "spi_treaty.h"

// Global GFX pointer — used by all apps, set by pm_display_init()
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
extern PMDispTLoRaPager *gfx;
#else
#include <Arduino_GFX_Library.h>
extern Arduino_GFX *gfx;
#endif


// ── T-DECK PLUS — ST7789 320×240 ─────────────────────────
#ifdef DEVICE_TDECK_PLUS

static inline bool pm_display_init() {
    Arduino_DataBus *bus = new Arduino_ESP32SPI(
        LCD_DC,   // DC
        LCD_CS,   // CS
        SPI_SCK,  // SCK
        SPI_MOSI, // MOSI
        SPI_MISO  // MISO
    );
    gfx = new Arduino_ST7789(
        bus,
        LCD_RST,  // RST (-1 = none)
        0,        // rotation
        true,     // IPS
        SCREEN_W,
        SCREEN_H
    );
    if (!gfx->begin()) {
        Serial.println("[DISPLAY] ST7789 init failed");
        return false;
    }
    gfx->fillScreen(BLACK);
    Serial.printf("[DISPLAY] ST7789 %dx%d ready\n",
                  SCREEN_W, SCREEN_H);
    return true;
}

#endif // DEVICE_TDECK_PLUS


// ── T-LORA PAGER — ST7796U 480×222 ───────────────────────
// The ST7796U is used here in 480x222 landscape mode.
//
// LilyGoLib's landscape rotations use x-offset 49, y-offset 0.
// Applying that 49-pixel gap to Y produces a split/wrapped image.
//
// Backlight is controlled via GPIO42 → AW9364 driver.
// AW9364 is a 16-level LED driver — we init it to level 8
// (50%) at boot and the brightness app can adjust it.
// AW9364 enable: GPIO42 HIGH = backlight on.
// The 16 brightness levels are set by pulse-counting on
// the EN pin (each LOW→HIGH pulse increments one level,
// cycling after 16). For simplicity we use analogWrite
// on GPIO42 as a PWM dimmer.
#ifdef DEVICE_TLORAPAGER

static inline bool pm_display_init() {
    if (!gfx) {
        gfx = new PMDispTLoRaPager(
            SPI_SCK, SPI_MISO, SPI_MOSI,
            LCD_CS, LCD_DC, LCD_BL,
            40000000UL, SPI);
    }

    if (!gfx->begin()) {
        Serial.println("[DISPLAY] standalone ST7796U init failed");
        return false;
    }

    gfx->fillScreen(0x0000);
    Serial.printf("[DISPLAY] standalone ST7796U %dx%d ready\n",
                  SCREEN_W, SCREEN_H);
    return true;
}

// Keyboard backlight — separate GPIO46 → direct PWM
static inline void pm_kb_backlight(uint8_t level) {
    // level 0-255
    analogWrite(KB_BL, level);
}

#endif // DEVICE_TLORAPAGER


// ── CARDPUTER ADV — ST7789 240×135 ───────────────────────
// Pin assignments TBD — hardware in transit
// M5Stack Cardputer uses ST7789 with specific x/y offsets
// Typical Cardputer offsets: x=40, y=53 for 240×135 window
#ifdef DEVICE_CARDPUTER_ADV

static inline bool pm_display_init() {
    // TODO: fill in when hardware arrives and pinout confirmed
    Serial.println("[DISPLAY] Cardputer ADV display init TBD");
    return false;
}

#endif // DEVICE_CARDPUTER_ADV


// ── Brightness helper — works on all devices ─────────────
// level: 0-255
static inline void pm_display_brightness(uint8_t level) {
#if defined(DEVICE_TDECK_PLUS)
    analogWrite(LCD_BL, level);
#elif defined(DEVICE_TLORAPAGER)
    // AW9364 via GPIO42 PWM
    analogWrite(LCD_BL, level);
#elif defined(DEVICE_CARDPUTER_ADV)
    // TODO
    (void)level;
#endif
}

#endif // HAL_DISPLAY_H
