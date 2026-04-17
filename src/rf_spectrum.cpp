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
 * PISCES MOON OS — RF SPECTRUM VISUALIZER v1.0
 * Uses SX1262 RSSI to sweep across a configurable frequency range
 * and render a scrolling waterfall + bar chart on the 320×240 display.
 *
 * How it works:
 *   For each frequency step the SX1262 is briefly tuned, a carrier
 *   sense / RSSI measurement is taken, then the radio moves to the
 *   next step. One full sweep takes ~sweep_steps × dwell_ms.
 *
 * Display:
 *   Top 180px — scrolling waterfall (each row is one sweep, oldest
 *   scrolls up and off). Color-mapped: blue=quiet → green → yellow → red.
 *   Bottom 40px — peak-hold bar chart of current sweep.
 *
 * SPI Bus Treaty:
 *   Sets lora_voice_active = true for the duration of the session.
 *   wardrive_task checks this before SD writes.
 *
 * Controls:
 *   Trackball L/R = shift center frequency ±1MHz
 *   Trackball U/D = zoom in/out (fewer/more steps)
 *   + / - keys    = adjust dwell time (sensitivity vs speed)
 *   Q / tap header = exit
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <RadioLib.h>
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "lora_voice.h"    // for lora_voice_active SPI Treaty flag
#include "rf_spectrum.h"

extern Arduino_GFX *gfx;
extern volatile bool lora_voice_active;

// ─────────────────────────────────────────────
//  SX1262 PINS (T-Deck Plus)
// ─────────────────────────────────────────────
#define RF_CS   9
#define RF_DIO1 45
#define RF_RST  17
#define RF_BUSY 13

// ─────────────────────────────────────────────
//  DISPLAY GEOMETRY
// ─────────────────────────────────────────────
#define RF_HEADER_H  24
#define RF_WATER_Y   RF_HEADER_H
#define RF_WATER_H   168
#define RF_BAR_Y     (RF_WATER_Y + RF_WATER_H)
#define RF_BAR_H     (240 - RF_BAR_Y)
#define RF_W         320

// ─────────────────────────────────────────────
//  SWEEP CONFIG
// ─────────────────────────────────────────────
#define RF_STEPS_DEFAULT 64    // Steps across displayed band
#define RF_STEPS_MIN     16
#define RF_STEPS_MAX     128
#define RF_DWELL_DEFAULT 3     // ms per step (RadioLib RSSI settle time)
#define RF_DWELL_MIN     1
#define RF_DWELL_MAX     20

// ─────────────────────────────────────────────
//  RSSI → COLOR MAPPING
//  Maps RSSI (-120 to -20 dBm) to RGB565
// ─────────────────────────────────────────────
static uint16_t rfRssiColor(float rssi) {
    // Clamp to range
    float norm = (rssi + 120.0f) / 100.0f;   // 0.0 (weakest) to 1.0 (strongest)
    if (norm < 0.0f) norm = 0.0f;
    if (norm > 1.0f) norm = 1.0f;

    // Blue → Cyan → Green → Yellow → Red
    uint8_t r, g, b;
    if (norm < 0.25f) {
        float t = norm / 0.25f;
        r = 0; g = (uint8_t)(255 * t); b = 255;
    } else if (norm < 0.5f) {
        float t = (norm - 0.25f) / 0.25f;
        r = 0; g = 255; b = (uint8_t)(255 * (1.0f - t));
    } else if (norm < 0.75f) {
        float t = (norm - 0.5f) / 0.25f;
        r = (uint8_t)(255 * t); g = 255; b = 0;
    } else {
        float t = (norm - 0.75f) / 0.25f;
        r = 255; g = (uint8_t)(255 * (1.0f - t)); b = 0;
    }
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// ─────────────────────────────────────────────
//  STATE
// ─────────────────────────────────────────────
static SX1262 rfRadio = new Module(RF_CS, RF_DIO1, RF_RST, RF_BUSY);
static bool   rfRadioInit = false;

static float   rfCenterMHz = 915.0f;
static float   rfSpanMHz   = 10.0f;   // Total span in MHz
static int     rfSteps     = RF_STEPS_DEFAULT;
static int     rfDwellMs   = RF_DWELL_DEFAULT;

// Waterfall buffer — PSRAM allocated in run_rf_spectrum(), freed on exit.
// 168 rows × 320 pixels × 2 bytes = 107KB — cannot live in BSS.
static uint16_t* rfWaterfall = nullptr;
static int       rfWaterRow  = 0;
static bool      rfWaterFull = false;

// Peak hold per column — PSRAM allocated alongside waterfall
static float* rfPeak     = nullptr;
static float* rfPeakHold = nullptr;

// ─────────────────────────────────────────────
//  HEADER
// ─────────────────────────────────────────────
static void rfDrawHeader() {
    gfx->fillRect(0, 0, 320, RF_HEADER_H, 0x0010);
    gfx->drawFastHLine(0, RF_HEADER_H - 1, 320, 0x07E0);
    gfx->setTextSize(1);
    gfx->setTextColor(0x07E0);
    gfx->setCursor(4, 4);
    gfx->print("RF SPECTRUM");
    gfx->setTextColor(0x4208);
    gfx->setCursor(4, 13);
    float startF = rfCenterMHz - rfSpanMHz / 2.0f;
    float endF   = rfCenterMHz + rfSpanMHz / 2.0f;
    gfx->printf("%.1f-%.1f MHz  %d steps  %dms", startF, endF, rfSteps, rfDwellMs);
    gfx->setTextColor(0x4208);
    gfx->setCursor(264, 8);
    gfx->print("[Q=EXIT]");
}

// ─────────────────────────────────────────────
//  RENDER ONE SWEEP ROW INTO WATERFALL
// ─────────────────────────────────────────────
static void rfRenderWaterfallRow(float* rssiValues, int steps) {
    // Map steps → RF_W pixels into current waterfall row
    uint16_t* row = rfWaterfall + (rfWaterRow % RF_WATER_H) * RF_W;
    for (int px = 0; px < RF_W; px++) {
        int stepIdx = (px * steps) / RF_W;
        if (stepIdx >= steps) stepIdx = steps - 1;
        row[px] = rfRssiColor(rssiValues[stepIdx]);
    }
    rfWaterRow++;
    if (rfWaterRow >= RF_WATER_H) rfWaterFull = true;
}

// ─────────────────────────────────────────────
//  DRAW WATERFALL (full refresh)
// ─────────────────────────────────────────────
static void rfDrawWaterfall() {
    int totalRows = rfWaterFull ? RF_WATER_H : rfWaterRow;
    int startRow  = rfWaterFull ? (rfWaterRow % RF_WATER_H) : 0;

    for (int y = 0; y < totalRows; y++) {
        int srcRow = (startRow + y) % RF_WATER_H;
        int dstY   = RF_WATER_Y + (RF_WATER_H - totalRows) + y;
        gfx->draw16bitBeRGBBitmap(0, dstY, rfWaterfall + srcRow * RF_W, RF_W, 1);
    }
}

// ─────────────────────────────────────────────
//  DRAW BAR CHART (bottom strip)
// ─────────────────────────────────────────────
static void rfDrawBars(float* rssiValues, int steps) {
    gfx->fillRect(0, RF_BAR_Y, RF_W, RF_BAR_H, 0x0000);

    float rssiMin = -120.0f;
    float rssiMax = -20.0f;

    for (int px = 0; px < RF_W; px++) {
        int stepIdx = (px * steps) / RF_W;
        if (stepIdx >= steps) stepIdx = steps - 1;
        float rssi = rssiValues[stepIdx];

        // Update peak hold
        if (rssi > rfPeakHold[px]) rfPeakHold[px] = rssi;

        float norm = (rssi - rssiMin) / (rssiMax - rssiMin);
        if (norm < 0) norm = 0; if (norm > 1) norm = 1;
        int barH = (int)(norm * RF_BAR_H);
        if (barH < 1) barH = 1;

        uint16_t col = rfRssiColor(rssi);
        gfx->drawFastVLine(px, RF_BAR_Y + RF_BAR_H - barH, barH, col);

        // Peak hold dot
        float pnorm = (rfPeakHold[px] - rssiMin) / (rssiMax - rssiMin);
        if (pnorm > 1) pnorm = 1;
        int peakY = RF_BAR_Y + RF_BAR_H - (int)(pnorm * RF_BAR_H);
        if (peakY >= RF_BAR_Y && peakY < RF_BAR_Y + RF_BAR_H)
            gfx->drawPixel(px, peakY, 0xFFFF);
    }

    // Frequency axis labels (start / center / end)
    gfx->setTextSize(1);
    gfx->setTextColor(0x2945);
    float startF = rfCenterMHz - rfSpanMHz / 2.0f;
    float endF   = rfCenterMHz + rfSpanMHz / 2.0f;
    gfx->setCursor(2,  RF_BAR_Y + 1); gfx->printf("%.0fM", startF);
    gfx->setCursor(138, RF_BAR_Y + 1); gfx->printf("%.0f", rfCenterMHz);
    gfx->setCursor(278, RF_BAR_Y + 1); gfx->printf("%.0fM", endF);
}

// ─────────────────────────────────────────────
//  PERFORM ONE SWEEP
// ─────────────────────────────────────────────
// Sweep buffer — PSRAM allocated
static float* rfSweepValues = nullptr;

static void rfSweep() {
    float startF = rfCenterMHz - rfSpanMHz / 2.0f;
    float stepSz = rfSpanMHz / (float)rfSteps;

    for (int i = 0; i < rfSteps; i++) {
        float freq = startF + i * stepSz;
        // Retune to this frequency — FSK mode, just need RSSI
        rfRadio.setFrequency(freq);
        delay(rfDwellMs);
        float rssi = rfRadio.getRSSI();
        rfSweepValues[i] = rssi;
        // Update peak
        int px = (i * RF_W) / rfSteps;
        if (rssi > rfPeak[px]) rfPeak[px] = rssi;
        yield();
    }
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_rf_spectrum() {
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0x07E0);
    gfx->setTextSize(2);
    gfx->setCursor(10, 50); gfx->print("RF SPECTRUM");
    gfx->setTextSize(1);
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(10, 80); gfx->print("Allocating buffers...");

    // Allocate display buffers in PSRAM — too large for BSS
    size_t waterSize  = (size_t)RF_WATER_H * RF_W * sizeof(uint16_t);
    size_t peakSize   = RF_W * sizeof(float);
    size_t sweepSize  = RF_STEPS_MAX * sizeof(float);

    rfWaterfall   = (uint16_t*)ps_malloc(waterSize);
    rfPeak        = (float*)ps_malloc(peakSize);
    rfPeakHold    = (float*)ps_malloc(peakSize);
    rfSweepValues = (float*)ps_malloc(sweepSize);

    if (!rfWaterfall || !rfPeak || !rfPeakHold || !rfSweepValues) {
        gfx->setTextColor(0xF800);
        gfx->setCursor(10, 100); gfx->print("Out of PSRAM. Tap to exit.");
        while (true) {
            if (get_keypress()) break;
            int16_t tx, ty; if (get_touch(&tx, &ty)) break;
            delay(50);
        }
        free(rfWaterfall); free(rfPeak); free(rfPeakHold); free(rfSweepValues);
        rfWaterfall = nullptr; rfPeak = nullptr;
        rfPeakHold = nullptr; rfSweepValues = nullptr;
        return;
    }

    gfx->setCursor(10, 80); gfx->print("Initializing SX1262...");
    delay(200);

    // Init radio in FSK mode for RSSI sensing
    if (!rfRadioInit) {
        int state = rfRadio.beginFSK(rfCenterMHz, 9.6, 19.5, 200.0, 10, 16);
        if (state != RADIOLIB_ERR_NONE) {
            gfx->setTextColor(0xF800);
            gfx->setCursor(10, 100); gfx->printf("Radio init FAILED: %d", state);
            gfx->setTextColor(0x4208);
            gfx->setCursor(10, 116); gfx->print("Tap or Q to exit.");
            while (true) {
                if (get_keypress()) return;
                int16_t tx, ty; if (get_touch(&tx, &ty)) return;
                delay(50);
            }
        }
        rfRadioInit = true;
    }

    // SPI Bus Treaty
    lora_voice_active = true;

    // Init state
    rfWaterRow = 0; rfWaterFull = false;
    memset(rfWaterfall, 0, waterSize);
    for (int i = 0; i < RF_W; i++) { rfPeak[i] = -120.0f; rfPeakHold[i] = -120.0f; }

    rfDrawHeader();
    gfx->fillRect(0, RF_WATER_Y, RF_W, RF_WATER_H, 0x0000);
    gfx->fillRect(0, RF_BAR_Y,   RF_W, RF_BAR_H,   0x0000);

    unsigned long lastHeaderRefresh = 0;
    // Peak hold decay interval (decay peaks slowly over time)
    unsigned long lastPeakDecay = 0;

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        if (get_touch(&tx, &ty) && ty < RF_HEADER_H) {
            while(get_touch(&tx,&ty)){delay(10);} break;
        }
        if (k == 'q' || k == 'Q') break;

        // Frequency shift
        if (tb.x == -1) { rfCenterMHz -= 1.0f; if (rfCenterMHz < 150.0f) rfCenterMHz = 150.0f; rfDrawHeader(); }
        if (tb.x ==  1) { rfCenterMHz += 1.0f; if (rfCenterMHz > 960.0f) rfCenterMHz = 960.0f; rfDrawHeader(); }

        // Zoom (span)
        if (tb.y == -1) { rfSpanMHz = max(1.0f, rfSpanMHz - 1.0f); rfDrawHeader(); }
        if (tb.y ==  1) { rfSpanMHz = min(50.0f, rfSpanMHz + 1.0f); rfDrawHeader(); }

        // Dwell time
        if (k == '+' || k == '=') { rfDwellMs = min(RF_DWELL_MAX, rfDwellMs + 1); rfDrawHeader(); }
        if (k == '-')             { rfDwellMs = max(RF_DWELL_MIN, rfDwellMs - 1); rfDrawHeader(); }

        // Step count zoom
        if (k == '[') { rfSteps = max(RF_STEPS_MIN, rfSteps / 2); rfDrawHeader(); }
        if (k == ']') { rfSteps = min(RF_STEPS_MAX, rfSteps * 2); rfDrawHeader(); }

        // Peak hold decay every 5 sweeps
        if (millis() - lastPeakDecay > 2000) {
            for (int i = 0; i < RF_W; i++) {
                rfPeakHold[i] -= 2.0f;
                if (rfPeakHold[i] < -120.0f) rfPeakHold[i] = -120.0f;
            }
            lastPeakDecay = millis();
        }

        // Perform sweep and render
        rfSweep();
        rfRenderWaterfallRow(rfSweepValues, rfSteps);
        rfDrawWaterfall();
        rfDrawBars(rfSweepValues, rfSteps);

        yield();
    }

    // Release SPI Treaty
    lora_voice_active = false;
    rfRadio.standby();

    // Free PSRAM buffers
    free(rfWaterfall);   rfWaterfall   = nullptr;
    free(rfPeak);        rfPeak        = nullptr;
    free(rfPeakHold);    rfPeakHold    = nullptr;
    free(rfSweepValues); rfSweepValues = nullptr;

    gfx->fillScreen(0x0000);
}
