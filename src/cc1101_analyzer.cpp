// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// cc1101_analyzer.cpp — see cc1101_analyzer.h
//
// Receive-only sub-GHz analyzer for the T-Embed CC1101. Two modes:
//   RSSI   — live signal-strength meter at the selected band. Answers
//            "is my remote transmitting here, and how strong?"
//   SNIFF  — raw OOK pulse capture. Shows the burst length/timing and
//            whether the burst CHANGES from press to press (rolling)
//            or repeats (fixed). It never transmits or replays.
//
// Encoder rotate = change frequency. Encoder push = switch mode.
// Side key = exit.

#ifdef DEVICE_TEMBED_CC1101

#include <Arduino.h>
#include <RadioLib.h>
#include <Arduino_GFX_Library.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "hal_pins.h"
#include "cc1101_analyzer.h"
#include "tembed_input.h"

extern Arduino_GFX*       gfx;
extern SemaphoreHandle_t  spi_mutex;

namespace {

// ── Bus Treaty helpers ───────────────────────────────────
inline bool busTake(uint32_t ms = 1000) {
    return spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(ms)) == pdTRUE;
}
inline void busGive() {
    if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
}

// ── Radio ────────────────────────────────────────────────
// Shares the board SPI bus with the display, SD and nRF24; the Bus
// Treaty mutex serializes access. GDO0 is RadioLib's packet IRQ pin;
// GDO2 is the async-serial data line we watch in SNIFF mode.
// RadioLib 6.6.0 makes both CC1101::SPIsetRegValue and getMod() protected,
// so the raw register access the OOK sniff mode needs can't be reached from
// outside. A thin subclass re-exposes the register writer (a derived class
// may use its own base's protected members). Receive-only: no TX is added.
class CC1101Sniff : public CC1101 {
public:
    CC1101Sniff(Module* m) : CC1101(m) {}
    using CC1101::SPIsetRegValue;     // promote to public on this subclass
};

CC1101Sniff radio = new Module(CC1101_CS, CC1101_GDO0, RADIOLIB_NC, CC1101_GDO2, SPI);

struct Band { float mhz; const char* label; uint8_t path; };
// path: antenna switch select (see setRfPath). 390 MHz rides the 433
// RF path — both sit in the CC1101's 387–464 MHz sub-band.
const Band kBands[] = {
    {315.00f, "315.00", 0},
    {390.00f, "390.00", 1},   // Genie / many US openers
    {433.92f, "433.92", 1},
    {868.00f, "868.00", 2},
};
const int kBandCount = sizeof(kBands) / sizeof(kBands[0]);
int  g_band = 1;             // default 390 MHz

enum Mode { MODE_RSSI = 0, MODE_SNIFF = 1 };
Mode g_mode = MODE_SNIFF;
bool g_radioOk = false;

// Drive the SKY-style antenna switch for the selected RF path.
//   315 : SW1=HIGH SW0=LOW      433/390 : SW1=HIGH SW0=HIGH
//   868 : SW1=LOW  SW0=HIGH
void setRfPath(uint8_t path) {
    pinMode(CC1101_SW1, OUTPUT);
    pinMode(CC1101_SW0, OUTPUT);
    switch (path) {
        case 0: digitalWrite(CC1101_SW1, HIGH); digitalWrite(CC1101_SW0, LOW);  break;
        case 2: digitalWrite(CC1101_SW1, LOW);  digitalWrite(CC1101_SW0, HIGH); break;
        default: digitalWrite(CC1101_SW1, HIGH); digitalWrite(CC1101_SW0, HIGH); break;
    }
}

// ── SNIFF: raw OOK edge capture ──────────────────────────
constexpr size_t   kRingSize       = 512;
constexpr uint32_t kBurstSilenceUs = 6000;    // gap that ends a burst
constexpr uint32_t kMinPulseUs     = 80;      // reject sub-80us noise
constexpr uint32_t kMaxPulseUs     = 30000;

volatile uint32_t  s_ring[kRingSize];
volatile uint16_t  s_head = 0, s_tail = 0;
volatile uint32_t  s_lastEdgeUs = 0;

void IRAM_ATTR onEdge() {
    uint32_t now = micros();
    uint32_t dt  = now - s_lastEdgeUs;
    s_lastEdgeUs = now;
    uint16_t next = (s_head + 1) % kRingSize;
    if (next != s_tail) {           // drop on overflow rather than block
        s_ring[s_head] = dt;
        s_head = next;
    }
}

// Rolling burst assembly + "fixed vs rolling" detection.
uint16_t s_curPulses = 0;
uint32_t s_curMinUs = 0, s_curMaxUs = 0;
uint32_t s_curSig   = 0;            // order-sensitive hash of pulse widths
uint32_t s_burstCount = 0;

uint16_t s_lastPulses = 0;
uint32_t s_lastDurMs  = 0;
uint32_t s_lastSig    = 0;
bool     s_haveLast   = false;

uint32_t s_prevSig = 0;            // signature of the burst before last
bool     s_havePrev = false;
enum Verdict { VERDICT_NONE, VERDICT_FIXED, VERDICT_ROLLING };
Verdict  s_verdict = VERDICT_NONE;

uint32_t s_burstStartUs = 0;

void resetSniffState() {
    s_head = s_tail = 0;
    s_curPulses = 0; s_curSig = 0; s_curMinUs = 0; s_curMaxUs = 0;
    s_burstCount = 0; s_haveLast = false; s_havePrev = false;
    s_verdict = VERDICT_NONE; s_prevSig = 0; s_lastSig = 0;
}

// Fold one captured edge-gap into the in-progress burst.
void feedPulse(uint32_t dt) {
    if (dt < kMinPulseUs || dt > kMaxPulseUs) return;
    if (s_curPulses == 0) { s_burstStartUs = micros(); s_curMinUs = s_curMaxUs = dt; }
    if (dt < s_curMinUs) s_curMinUs = dt;
    if (dt > s_curMaxUs) s_curMaxUs = dt;
    // Quantize to ~50us buckets so timing jitter doesn't change the
    // signature, but a genuinely different code does.
    uint32_t q = dt / 50;
    s_curSig = s_curSig * 2654435761u + (q + 1);
    if (s_curPulses < 0xFFFF) s_curPulses++;
}

// Close the current burst (called when silence is seen) and update the
// fixed-vs-rolling verdict by comparing it to the previous burst.
void closeBurst() {
    if (s_curPulses < 8) {          // too short to be a real code frame
        s_curPulses = 0; s_curSig = 0;
        return;
    }
    s_lastPulses = s_curPulses;
    s_lastDurMs  = (micros() - s_burstStartUs) / 1000;
    s_lastSig    = s_curSig;
    s_haveLast   = true;
    s_burstCount++;

    if (s_havePrev) {
        // Compare this frame to the one before it. Identical → the code
        // is being repeated (fixed / same keypress held). Different →
        // the code changed between presses (rolling).
        s_verdict = (s_lastSig == s_prevSig) ? VERDICT_FIXED : VERDICT_ROLLING;
    }
    s_prevSig  = s_lastSig;
    s_havePrev = true;

    s_curPulses = 0; s_curSig = 0;
}

// Drain the ISR ring into burst assembly; call often from the loop.
void pumpSniff() {
    while (s_tail != s_head) {
        uint32_t dt = s_ring[s_tail];
        s_tail = (s_tail + 1) % kRingSize;
        feedPulse(dt);
    }
    // Silence since the last edge ends the burst.
    if (s_curPulses > 0) {
        noInterrupts();
        uint32_t last = s_lastEdgeUs;
        interrupts();
        if (micros() - last > kBurstSilenceUs) closeBurst();
    }
}

// ── Radio configuration per mode ─────────────────────────
void configureRadio() {
    g_radioOk = false;
    detachInterrupt(digitalPinToInterrupt(CC1101_GDO2));
    setRfPath(kBands[g_band].path);

    if (!busTake(2000)) return;
    int st = radio.begin();                 // defaults, then we override
    if (st == RADIOLIB_ERR_NONE) st = radio.setFrequency(kBands[g_band].mhz);
    if (st == RADIOLIB_ERR_NONE) st = radio.setOOK(true);

    if (st == RADIOLIB_ERR_NONE) {
        if (g_mode == MODE_SNIFF) {
            // Wide, raw async OOK: no sync word, data straight out on the
            // GDO pins so we can time the pulses ourselves.
            radio.setRxBandwidth(270.0f);
            radio.setBitRate(5.0f);
            // Raw register pokes via the CC1101Sniff subclass, which
            // re-exposes CC1101::SPIsetRegValue (protected in RadioLib 6.6.0).
            radio.SPIsetRegValue(0x12, 0x00, 2, 0);   // MDMCFG2: no sync
            radio.SPIsetRegValue(0x00, 0x0D);         // IOCFG2 = async serial
            radio.SPIsetRegValue(0x02, 0x0D);         // IOCFG0 = async serial
            radio.SPIsetRegValue(0x08, 0x30 | 0x02);  // PKTCTRL0 async serial
            radio.receiveDirect();
        } else {
            radio.setRxBandwidth(135.0f);
            radio.setBitRate(4.0f);
            radio.startReceive();
        }
        g_radioOk = true;
    }
    busGive();

    if (g_mode == MODE_SNIFF) {
        resetSniffState();
        s_lastEdgeUs = micros();
        attachInterrupt(digitalPinToInterrupt(CC1101_GDO2), onEdge, CHANGE);
    }
    Serial.printf("[CC1101] cfg %s @ %.2f MHz -> %s\n",
                  g_mode == MODE_SNIFF ? "SNIFF" : "RSSI",
                  kBands[g_band].mhz, g_radioOk ? "ok" : "FAILED");
}

// ── Rendering ────────────────────────────────────────────
constexpr uint16_t COL_BG    = 0x0000;
constexpr uint16_t COL_HDR   = 0x7BEF;
constexpr uint16_t COL_HI    = 0x07FF;
constexpr uint16_t COL_OK    = 0x07E0;
constexpr uint16_t COL_WARN  = 0xFD20;
constexpr uint16_t COL_ROLL  = 0xF81F;
constexpr uint16_t COL_DIM   = 0x4208;
constexpr uint16_t COL_TXT   = 0xFFFF;

float   g_rssi = -120.0f;
float   g_rssiPeak = -120.0f;
uint32_t g_lastPeakMs = 0;

void drawChrome() {
    gfx->fillScreen(COL_BG);
    gfx->setTextColor(COL_HDR);
    gfx->setTextSize(2);
    gfx->setCursor(6, 6);
    gfx->print("CC1101 ANALYZER  (RX only)");
    gfx->drawFastHLine(0, 26, 320, COL_DIM);
    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(6, 158);
    gfx->print("TURN=freq   PUSH=mode   SIDE=back");
}

void drawRssiBar(int y, float dbm) {
    // Map -100..-30 dBm to 0..300 px.
    int w = (int)((dbm + 100.0f) / 70.0f * 300.0f);
    if (w < 0) w = 0; if (w > 300) w = 300;
    gfx->fillRect(6, y, 308, 16, COL_BG);
    gfx->drawRect(6, y, 302, 16, COL_DIM);
    uint16_t c = dbm > -55 ? COL_OK : (dbm > -80 ? COL_WARN : COL_ROLL);
    gfx->fillRect(7, y + 1, w, 14, c);
}

void render() {
    // Frequency + mode line
    gfx->fillRect(0, 30, 320, 24, COL_BG);
    gfx->setTextSize(3);
    gfx->setTextColor(COL_HI);
    gfx->setCursor(6, 30);
    gfx->print(kBands[g_band].label);
    gfx->setTextSize(2);
    gfx->setTextColor(COL_TXT);
    gfx->setCursor(150, 34);
    gfx->print("MHz");
    gfx->setTextColor(g_mode == MODE_SNIFF ? COL_WARN : COL_OK);
    gfx->setCursor(230, 34);
    gfx->print(g_mode == MODE_SNIFF ? "SNIFF" : "RSSI");

    if (!g_radioOk) {
        gfx->setTextSize(2);
        gfx->setTextColor(COL_ROLL);
        gfx->setCursor(6, 80);
        gfx->print("RADIO INIT FAILED");
        return;
    }

    if (g_mode == MODE_RSSI) {
        gfx->setTextSize(2);
        gfx->setTextColor(COL_TXT);
        gfx->fillRect(0, 64, 320, 20, COL_BG);
        gfx->setCursor(6, 64);
        gfx->printf("%d dBm  peak %d", (int)g_rssi, (int)g_rssiPeak);
        drawRssiBar(92, g_rssi);
        drawRssiBar(118, g_rssiPeak);
        gfx->setTextSize(1);
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(6, 140);
        gfx->print("Press your remote. A jump = TX on this band.");
    } else {
        // SNIFF
        gfx->fillRect(0, 60, 320, 92, COL_BG);
        gfx->setTextSize(2);
        gfx->setTextColor(COL_TXT);
        gfx->setCursor(6, 62);
        gfx->printf("bursts: %lu", (unsigned long)s_burstCount);

        if (s_haveLast) {
            gfx->setCursor(6, 84);
            gfx->printf("last: %u pulses", s_lastPulses);
            gfx->setCursor(6, 104);
            gfx->printf("%lu ms  %lu-%lu us",
                        (unsigned long)s_lastDurMs,
                        (unsigned long)s_curMinUs, (unsigned long)s_curMaxUs);
        } else {
            gfx->setCursor(6, 84);
            gfx->setTextColor(COL_DIM);
            gfx->print("waiting for a burst...");
        }

        // Fixed vs rolling verdict.
        gfx->setTextSize(2);
        gfx->setCursor(6, 128);
        switch (s_verdict) {
            case VERDICT_ROLLING:
                gfx->setTextColor(COL_ROLL);
                gfx->print("ROLLING (changes)");
                break;
            case VERDICT_FIXED:
                gfx->setTextColor(COL_OK);
                gfx->print("FIXED (repeats)");
                break;
            default:
                gfx->setTextColor(COL_DIM);
                gfx->print("press twice to classify");
                break;
        }
    }
}

} // namespace

void cc1101_analyzer_run() {
    drawChrome();
    configureRadio();
    render();

    uint32_t lastRssiMs = 0;
    uint32_t lastDrawMs = 0;

    for (;;) {
        if (tembed_btn_back()) break;

        int d = tembed_encoder_delta();
        if (d != 0) {
            g_band += (d > 0 ? 1 : -1);
            if (g_band < 0) g_band = kBandCount - 1;
            if (g_band >= kBandCount) g_band = 0;
            configureRadio();
            render();
        }

        if (tembed_btn_select()) {
            g_mode = (g_mode == MODE_RSSI) ? MODE_SNIFF : MODE_RSSI;
            configureRadio();
            render();
        }

        uint32_t now = millis();

        if (g_mode == MODE_SNIFF) {
            pumpSniff();
            if (now - lastDrawMs > 120) { render(); lastDrawMs = now; }
        } else {
            if (now - lastRssiMs > 50) {
                lastRssiMs = now;
                if (busTake(100)) {
                    g_rssi = radio.getRSSI();
                    busGive();
                }
                if (g_rssi > g_rssiPeak) { g_rssiPeak = g_rssi; g_lastPeakMs = now; }
                if (now - g_lastPeakMs > 3000) g_rssiPeak = g_rssi;  // decay
            }
            if (now - lastDrawMs > 100) { render(); lastDrawMs = now; }
        }

        delay(2);
    }

    // Leave the radio idle on the way out.
    detachInterrupt(digitalPinToInterrupt(CC1101_GDO2));
    if (busTake(500)) { radio.standby(); busGive(); }
}

// ── Survey hooks ─────────────────────────────────────────────────
// Plain RSSI receive (same settings as the analyzer's RSSI mode), no
// GDO2 interrupt. The analyzer's SNIFF mode reprograms IOCFG/PKTCTRL
// registers, so begin() always starts from radio.begin() defaults.
bool cc1101_survey_begin() {
    detachInterrupt(digitalPinToInterrupt(CC1101_GDO2));
    setRfPath(kBands[0].path);
    if (!busTake(2000)) return false;
    int st = radio.begin();
    if (st == RADIOLIB_ERR_NONE) st = radio.setOOK(true);
    if (st == RADIOLIB_ERR_NONE) st = radio.setRxBandwidth(135.0f);
    if (st == RADIOLIB_ERR_NONE) st = radio.setBitRate(4.0f);
    if (st == RADIOLIB_ERR_NONE) st = radio.setFrequency(kBands[0].mhz);
    if (st == RADIOLIB_ERR_NONE) st = radio.startReceive();
    busGive();
    Serial.printf("[CC1101] survey begin -> %d\n", st);
    return st == RADIOLIB_ERR_NONE;
}

int cc1101_survey_bands() { return kBandCount; }

const char* cc1101_survey_label(int band) {
    return (band >= 0 && band < kBandCount) ? kBands[band].label : "?";
}

bool cc1101_survey_tune(int band) {
    if (band < 0 || band >= kBandCount) return false;
    setRfPath(kBands[band].path);
    if (!busTake(200)) return false;
    radio.standby();
    int st = radio.setFrequency(kBands[band].mhz);
    if (st == RADIOLIB_ERR_NONE) st = radio.startReceive();
    busGive();
    return st == RADIOLIB_ERR_NONE;
}

float cc1101_survey_rssi() {
    float r = -127.0f;
    if (busTake(20)) { r = radio.getRSSI(); busGive(); }
    return r;
}

void cc1101_survey_end() {
    if (busTake(500)) { radio.standby(); busGive(); }
}

#endif // DEVICE_TEMBED_CC1101
