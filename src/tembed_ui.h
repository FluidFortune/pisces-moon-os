// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  tembed_ui.h — shared look + helpers for T-Embed apps
//
//  One palette and one header/footer layout so every app on the
//  320x170 encoder-only screen reads the same way:
//    y   0..26   title bar (size-2 text) + divider at y=26
//    y  28..154  app body
//    y 156..170  footer hint (size-1 text)
//
//  Also declares the app entry points and the shared services
//  (text entry, time sync, Bus Treaty helpers) that live in the
//  tembed_*.cpp files.
// ─────────────────────────────────────────────

#pragma once

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern Arduino_GFX*      gfx;
extern SemaphoreHandle_t spi_mutex;

namespace tui {

constexpr uint16_t BG   = 0x0000;
constexpr uint16_t HDR  = 0x7BEF;   // grey title
constexpr uint16_t DIM  = 0x4208;   // dividers, hints
constexpr uint16_t TXT  = 0xFFFF;
constexpr uint16_t HI   = 0x07FF;   // cyan accent
constexpr uint16_t GOOD   = 0x07E0;   // green
constexpr uint16_t WARN = 0xFD20;   // amber
constexpr uint16_t BAD  = 0xF800;   // red
constexpr uint16_t MAG  = 0xF81F;   // magenta
constexpr uint16_t SEL  = 0x001F;   // selection fill (blue)
constexpr uint16_t SUBT = 0x52AA;   // subdued text

constexpr int W = 320;
constexpr int H = 170;
constexpr int BODY_Y = 30;
constexpr int FOOT_Y = 158;

inline void header(const char* title) {
    gfx->fillScreen(BG);
    gfx->setTextColor(HDR);
    gfx->setTextSize(2);
    gfx->setCursor(6, 6);
    gfx->print(title);
    gfx->drawFastHLine(0, 26, W, DIM);
}

// Right-aligned status text in the title bar (e.g. "SD", "WiFi").
inline void headerTag(const char* tag, uint16_t color) {
    int w = (int)strlen(tag) * 6;
    gfx->fillRect(W - 90, 0, 90, 25, BG);
    gfx->setTextSize(1);
    gfx->setTextColor(color);
    gfx->setCursor(W - 6 - w, 10);
    gfx->print(tag);
}

inline void footer(const char* hint) {
    gfx->fillRect(0, FOOT_Y - 2, W, H - (FOOT_Y - 2), BG);
    gfx->setTextColor(DIM);
    gfx->setTextSize(1);
    gfx->setCursor(6, FOOT_Y);
    gfx->print(hint);
}

// Clear a body band before redrawing it.
inline void clearBand(int y, int h) { gfx->fillRect(0, y, W, h, BG); }

// Four-step signal bars, 14x10, for an RSSI in dBm.
inline void rssiBars(int x, int y, int rssi) {
    int lvl = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : rssi >= -90 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int bh = 3 + i * 2;
        uint16_t c = (i < lvl) ? (lvl >= 3 ? GOOD : lvl == 2 ? WARN : BAD) : DIM;
        gfx->fillRect(x + i * 4, y + 10 - bh, 3, bh, c);
    }
}

// Horizontal meter: value in [lo,hi] mapped onto w pixels.
inline void meter(int x, int y, int w, int h, float v, float lo, float hi, uint16_t color) {
    int fill = (int)((v - lo) / (hi - lo) * (w - 2));
    if (fill < 0) fill = 0;
    if (fill > w - 2) fill = w - 2;
    gfx->drawRect(x, y, w, h, DIM);
    gfx->fillRect(x + 1, y + 1, fill, h - 2, color);
    gfx->fillRect(x + 1 + fill, y + 1, (w - 2) - fill, h - 2, BG);
}

// Bus Treaty: SD, CC1101, nRF24 and the LCD share one SPI bus.
inline bool busTake(uint32_t ms = 1000) {
    return spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}
inline void busGive() { if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex); }

} // namespace tui

// ── Shared services ─────────────────────────────────────────────

// Encoder text entry (tembed_textin.cpp). Blocks until DONE (returns
// true, `out` filled) or a long-press of the side key (returns false,
// `out` untouched). `initial` may be nullptr.
bool tembed_text_input(const char* title, const char* subtitle,
                       char* out, size_t outlen, const char* initial);

// Time (tembed_clock.cpp). Applies the saved timezone; sync needs WiFi.
void tembed_time_apply_tz();
bool tembed_time_sync(uint32_t timeout_ms);     // true once the clock is set
bool tembed_time_valid();

// Hardware Test hooks (implemented in the owning apps).
bool tembed_nfc_probe(uint32_t* fw);                 // tembed_nfc.cpp
bool tembed_battery_read(int* soc, int* mv, int* ma); // tembed_clock.cpp
int  nrf24_radiolib_begin_code();                     // nrf24_scan.cpp

// ── App entry points ────────────────────────────────────────────
void tembed_nfc_run();          // tembed_nfc.cpp
void tembed_wifi_run();         // tembed_wifi.cpp
void tembed_ble_radar_run();    // tembed_ble_radar.cpp
void tembed_survey_run();       // tembed_survey.cpp
void tembed_clock_run();        // tembed_clock.cpp
void tembed_selftest_run();     // tembed_selftest.cpp

#endif // DEVICE_TEMBED_CC1101
