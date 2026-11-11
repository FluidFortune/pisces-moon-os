// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// nrf24_scan.cpp — see nrf24_scan.h
//
// 2.4 GHz channel-activity scanner using the nRF24's RPD bit. Sweeps the
// 126 channels (2400-2525 MHz), listens briefly on each, and plots a live
// bar per channel plus a peak-hold line. Receive-only. Encoder changes the
// per-channel dwell; push resets the peaks; side key exits.
//
// Reference: LilyGO factory nRF24 example (RadioLib nRF24 driver).

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <RadioLib.h>
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "hal_pins.h"
#include "nrf24_scan.h"
#include "tembed_input.h"

extern Arduino_GFX*       gfx;
extern SemaphoreHandle_t  spi_mutex;

namespace {

inline bool busTake(uint32_t ms = 1000) {
    return spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}
inline void busGive() { if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex); }

// nRF24 on the shared SPI bus. Module slots: CS, IRQ, CE, GPIO. The IRQ
// line isn't wired on the T-Embed (NRF24_IRQ == -1), so pass RADIOLIB_NC.
nRF24 radio = new Module(NRF24_CS, RADIOLIB_NC, NRF24_CE, RADIOLIB_NC, SPI);

constexpr int kNumChannels = 126;          // 2400..2525 MHz
uint8_t  g_hits[kNumChannels];             // live activity (decays)
uint8_t  g_peak[kNumChannels];             // peak-hold
bool     g_radioOk = false;

// Per-channel listen window. Longer catches more bursty traffic but
// slows the sweep. Encoder cycles these.
const uint16_t kDwellUs[] = { 120, 300, 600, 1200 };
const int kDwellCount = sizeof(kDwellUs) / sizeof(kDwellUs[0]);
int g_dwellIdx = 1;

// ── Layout (320x170) ──
constexpr uint16_t COL_BG   = 0x0000;
constexpr uint16_t COL_HDR  = 0x7BEF;
constexpr uint16_t COL_DIM  = 0x4208;
constexpr uint16_t COL_BAR  = 0x07FF;
constexpr uint16_t COL_PEAK = 0xFD20;
constexpr uint16_t COL_TXT  = 0xFFFF;

constexpr int kGraphX = 8;
constexpr int kGraphY = 40;
constexpr int kGraphW = 300;               // ~2.38 px per channel
constexpr int kGraphH = 96;
constexpr int kBase   = kGraphY + kGraphH;  // baseline y

int chanToX(int ch) { return kGraphX + (ch * kGraphW) / kNumChannels; }

int g_initErr = 0;

void configureRadio() {
    g_radioOk = false;
    if (!busTake(2000)) return;
    // RadioLib 6.6.0 reads registers without a status byte, so LilyGO's
    // RadioLib-7 workaround (spiConfig status width = 0) isn't needed here.
    int st = radio.begin();                 // 2400 MHz, 1 Mbps, defaults
    g_initErr = st;
    if (st == RADIOLIB_ERR_NONE) {
        radio.setAddressWidth(5);
        g_radioOk = true;
    }
    busGive();
    Serial.printf("[nRF24] scanner init -> %s (%d)\n", g_radioOk ? "ok" : "FAILED", st);
}

// Bare SPI probe, independent of RadioLib: clock out R_REGISTER(SETUP_AW)
// and read back STATUS (shifted out during the command byte) and the
// register. A live nRF24 answers STATUS ~0x0E and SETUP_AW 0x01..0x03;
// 0xFF/0x00 on both means nothing is driving MISO — no module seated.
void probeChip(uint8_t& status, uint8_t& setupAw) {
    status = setupAw = 0xFF;
    if (!busTake(500)) return;
    pinMode(NRF24_CE, OUTPUT); digitalWrite(NRF24_CE, LOW);
    pinMode(NRF24_CS, OUTPUT);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(NRF24_CS, LOW);
    status  = SPI.transfer(0x00 | 0x03);    // R_REGISTER | SETUP_AW
    setupAw = SPI.transfer(0xFF);
    digitalWrite(NRF24_CS, HIGH);
    SPI.endTransaction();
    busGive();
}

void drawInitFailed() {
    uint8_t status, aw;
    probeChip(status, aw);
    gfx->fillRect(0, 30, 320, 126, COL_BG);
    gfx->setTextColor(0xF800); gfx->setTextSize(2);
    gfx->setCursor(6, 36); gfx->print("nRF24 not detected");
    gfx->setTextSize(1); gfx->setTextColor(COL_TXT);
    gfx->setCursor(6, 62);  gfx->print("On the Plus it's a factory-fitted shield in");
    gfx->setCursor(6, 74);  gfx->print("the case (CS 44, CE 43). Run Hardware Test");
    gfx->setCursor(6, 86);  gfx->print("for detail. PUSH to retry.");
    bool silent = (status == 0xFF && aw == 0xFF) || (status == 0x00 && aw == 0x00);
    gfx->setTextColor(silent ? 0xFD20 : 0x07FF);
    gfx->setCursor(6, 106);
    gfx->printf("SPI probe: STATUS %02X  SETUP_AW %02X", status, aw);
    gfx->setCursor(6, 118);
    gfx->print(silent ? "-> nothing answering on the bus"
                      : "-> something answers; check module / wiring");
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, 132); gfx->printf("RadioLib begin() = %d", g_initErr);
    gfx->fillRect(0, 156, 320, 14, COL_BG);
    gfx->setCursor(6, 158); gfx->print("PUSH=retry   SIDE=back");
}

// One full sweep of all channels, folding results into g_hits/g_peak.
void sweep() {
    const uint16_t dwell = kDwellUs[g_dwellIdx];
    for (int ch = 0; ch < kNumChannels; ch++) {
        bool detected = false;
        if (busTake(50)) {
            radio.standby();
            radio.setFrequency(2400 + ch);
            radio.startReceive();
            busGive();
        }
        delayMicroseconds(dwell);           // listen (bus free during wait)
        if (busTake(50)) {
            detected = radio.isCarrierDetected();
            radio.standby();
            busGive();
        }
        // Decay then bump: live bar rises on hits, fades otherwise.
        if (g_hits[ch] > 0) g_hits[ch]--;
        if (detected && g_hits[ch] < 12) g_hits[ch] += 4;
        if (g_hits[ch] > g_peak[ch]) g_peak[ch] = g_hits[ch];
    }
}

void drawChrome() {
    gfx->fillScreen(COL_BG);
    gfx->setTextColor(COL_HDR); gfx->setTextSize(2);
    gfx->setCursor(6, 6); gfx->print("nRF24 SCAN");   // band shown on the axis; dwell readout owns x>=170
    gfx->drawFastHLine(0, 28, 320, COL_DIM);
    gfx->setTextColor(COL_DIM); gfx->setTextSize(1);
    gfx->setCursor(6, 158); gfx->print("TURN=dwell  PUSH=reset peaks  SIDE=back");
    // Baseline + a couple of channel ticks (0, 2440, 2480, 2525).
    gfx->drawFastHLine(kGraphX, kBase, kGraphW, COL_DIM);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(kGraphX, kBase + 3);          gfx->print("2400");
    gfx->setCursor(kGraphX + kGraphW - 24, kBase + 3); gfx->print("2525");
}

void drawBars() {
    // Clear graph area (not the chrome).
    gfx->fillRect(kGraphX, kGraphY, kGraphW, kGraphH, COL_BG);
    for (int ch = 0; ch < kNumChannels; ch++) {
        int x = chanToX(ch);
        int w = chanToX(ch + 1) - x; if (w < 1) w = 1;
        if (g_peak[ch] > 0) {
            int ph = (g_peak[ch] * kGraphH) / 12; if (ph > kGraphH) ph = kGraphH;
            gfx->drawFastHLine(x, kBase - ph, w, COL_PEAK);
        }
        if (g_hits[ch] > 0) {
            int bh = (g_hits[ch] * kGraphH) / 12; if (bh > kGraphH) bh = kGraphH;
            gfx->fillRect(x, kBase - bh, w, bh, COL_BAR);
        }
    }
    // Dwell readout.
    gfx->fillRect(170, 6, 150, 16, COL_BG);
    gfx->setTextColor(COL_TXT); gfx->setTextSize(1);
    gfx->setCursor(200, 10); gfx->printf("dwell %uus", (unsigned)kDwellUs[g_dwellIdx]);
}

} // namespace

// Hardware Test hook: run the same RadioLib begin() the scanner uses and
// return its status code (0 = chip found and configured), then idle it.
int nrf24_radiolib_begin_code() {
    configureRadio();
    if (g_radioOk && busTake(500)) { radio.standby(); busGive(); }
    return g_initErr;
}

void nrf24_scan_run() {
    for (int i = 0; i < kNumChannels; i++) { g_hits[i] = 0; g_peak[i] = 0; }
    tembed_input_flush();
    drawChrome();
    configureRadio();

    while (!g_radioOk) {
        drawInitFailed();
        for (;;) {
            if (tembed_btn_back()) return;
            if (tembed_btn_select()) break;
            delay(10);
        }
        drawChrome();
        configureRadio();
    }

    uint32_t lastDraw = 0;
    for (;;) {
        if (tembed_btn_back()) break;

        int d = tembed_encoder_delta();
        if (d != 0) {
            g_dwellIdx += (d > 0 ? 1 : -1);
            if (g_dwellIdx < 0) g_dwellIdx = kDwellCount - 1;
            if (g_dwellIdx >= kDwellCount) g_dwellIdx = 0;
        }
        if (tembed_btn_select()) {
            for (int i = 0; i < kNumChannels; i++) g_peak[i] = 0;
        }

        sweep();

        uint32_t now = millis();
        if (now - lastDraw > 90) { drawBars(); lastDraw = now; }
    }

    if (busTake(500)) { radio.standby(); busGive(); }
}

#endif // DEVICE_TEMBED_CC1101
