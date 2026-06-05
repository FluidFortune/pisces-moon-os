// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_mic_probe.cpp — C28P microphone capture DIAGNOSTIC
//
//  WHY THIS EXISTS:
//
//  Audio recording was the long-standing blocked feature on the C28P:
//  the ES8311's ADC bring-up was incomplete (missing power-on byte
//  0x80, missing ADC-clock enable in clkmgr1, missing ADC EQ/gain
//  registers). This probe is the validating measurement: a one-tap
//  3-second capture with on-screen RMS and a serial dump.
//
//  v1.3 REFACTOR:
//
//  Originally this file performed its own ES8311 register sequence
//  and its own i2s_driver_install(I2S_NUM_0, RX). After running, the
//  driver was left uninstalled and the c28p_audio.cpp static flag
//  i2s_initialized stayed at true — so game audio thought it was
//  ready and silently failed until reboot.
//
//  The fix is the C28P audio HAL with explicit modes (see
//  include/c28p_audio.h). This file now calls:
//
//      c28p_audio_enter(C28P_AUDIO_RECORD)   to start
//      c28p_audio_release()                  to stop
//
//  Both the codec sequence and the I2S install/uninstall happen
//  inside the HAL. The same provenance-verified register values
//  the probe was constructed from (esp-bsp es8311_init +
//  es8311_microphone_config + coeff_div[] row for 4.096 MHz / 16 kHz)
//  now live in c28p_audio.cpp's es8311_init() — see that file for
//  the line-by-line cross-reference. The probe just measures.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <driver/i2s.h>
#include <math.h>
#include "hal_pins.h"
#include "c28p_audio.h"
#include "pm_storage.h"

extern Arduino_GFX *gfx;
extern bool c28p_touch_read(int16_t *x, int16_t *y);

// ── Capture config ──
#define MP_SAMPLE_RATE   16000
#define MP_CAPTURE_MS    3000UL
#define MP_READ_BYTES    2048
#define MP_WAV_PATH      "/mic_probe.wav"

// ── Colors ──
#define MP_BG     0x0000
#define MP_HDR    0x0841
#define MP_CYAN   0x07FF
#define MP_GREEN  0x07E0
#define MP_AMBER  0xFD20
#define MP_RED    0xF800
#define MP_WHITE  0xFFFF
#define MP_DIM    0x4208

// UI geometry (240x320 portrait)
#define MP_HEADER_H   14
#define MP_BTN_Y      250
#define MP_BTN_H      44
#define MP_METER_Y    150
#define MP_METER_H    24

// ─────────────────────────────────────────────
//  WAV header (16k/16-bit/mono PCM)
// ─────────────────────────────────────────────
struct MP_WAV {
    char     riff[4]  = {'R','I','F','F'};
    uint32_t fileSize = 0;
    char     wave[4]  = {'W','A','V','E'};
    char     fmt[4]   = {'f','m','t',' '};
    uint32_t fmtSize  = 16;
    uint16_t fmtTag   = 1;
    uint16_t ch       = 1;
    uint32_t rate     = MP_SAMPLE_RATE;
    uint32_t byteRate = MP_SAMPLE_RATE * 2;
    uint16_t align    = 2;
    uint16_t bits     = 16;
    char     data[4]  = {'d','a','t','a'};
    uint32_t dataSize = 0;
};

// ─────────────────────────────────────────────
//  UI helpers
// ─────────────────────────────────────────────
static void mp_header() {
    gfx->fillRect(0, 0, 240, MP_HEADER_H, MP_HDR);
    gfx->setTextSize(1);
    gfx->setTextColor(MP_CYAN);
    gfx->setCursor(4, 3);
    gfx->print("MIC PROBE");
    gfx->setTextColor(MP_DIM);
    gfx->setCursor(190, 3);
    gfx->print("< EXIT");
}

static void mp_button(const char* label, uint16_t col) {
    gfx->fillRect(20, MP_BTN_Y, 200, MP_BTN_H, 0x18C3);
    gfx->drawRect(20, MP_BTN_Y, 200, MP_BTN_H, col);
    gfx->setTextSize(2);
    gfx->setTextColor(col);
    int w = strlen(label) * 12;
    gfx->setCursor((240 - w) / 2, MP_BTN_Y + (MP_BTN_H - 14) / 2);
    gfx->print(label);
}

static void mp_meter(uint16_t level /*0..32767*/) {
    gfx->drawRect(18, MP_METER_Y, 204, MP_METER_H, MP_DIM);
    gfx->fillRect(19, MP_METER_Y + 1, 202, MP_METER_H - 2, MP_BG);
    int w = (int)((uint32_t)level * 202 / 32767);
    if (w > 202) w = 202;
    uint16_t c = level > 22000 ? MP_RED : (level > 9000 ? MP_AMBER : MP_GREEN);
    if (w > 0) gfx->fillRect(19, MP_METER_Y + 1, w, MP_METER_H - 2, c);
}

static void mp_status(const String& s, uint16_t col, int line) {
    int y = MP_HEADER_H + 8 + line * 14;
    gfx->fillRect(0, y, 240, 14, MP_BG);
    gfx->setTextSize(1);
    gfx->setTextColor(col);
    gfx->setCursor(6, y);
    gfx->print(s);
}

// ─────────────────────────────────────────────
//  One 3-second capture pass with full measurement
//
//  Assumes c28p_audio_enter(C28P_AUDIO_RECORD) has been called
//  successfully — the I2S RX driver is owned by the HAL and the
//  codec is already producing samples on I2S_NUM_0.
// ─────────────────────────────────────────────
static void mp_run_capture() {
    const uint32_t totalSamples = (MP_SAMPLE_RATE * MP_CAPTURE_MS) / 1000UL;
    const uint32_t totalBytes   = totalSamples * 2;

    int16_t* pcm = (int16_t*)ps_malloc(totalBytes);
    if (!pcm) {
        mp_status("PSRAM alloc failed", MP_RED, 0);
        Serial.println("[MICPROBE] ps_malloc failed");
        return;
    }

    mp_button("CAPTURING", MP_AMBER);
    Serial.println("[MICPROBE] ===== capture start =====");

    uint32_t got = 0;
    uint32_t totalRead = 0;
    uint32_t startMs = millis();
    uint32_t lastMeter = 0;

    while (got < totalSamples && (millis() - startMs) < (MP_CAPTURE_MS + 1000)) {
        size_t bytesRead = 0;
        uint32_t remain = (totalSamples - got) * 2;
        uint32_t want = remain < MP_READ_BYTES ? remain : MP_READ_BYTES;
        esp_err_t err = i2s_read(I2S_NUM_0, (uint8_t*)(pcm + got), want,
                                 &bytesRead, pdMS_TO_TICKS(100));
        if (err == ESP_OK && bytesRead > 0) {
            int n = bytesRead / 2;
            uint16_t blockPeak = 0;
            for (int i = 0; i < n; i++) {
                int16_t v = pcm[got + i];
                uint16_t a = (uint16_t)(v < 0 ? -v : v);
                if (a > blockPeak) blockPeak = a;
            }
            got += n;
            totalRead += bytesRead;
            if (millis() - lastMeter > 80) {
                mp_meter(blockPeak);
                lastMeter = millis();
            }
        }
        yield();
    }

    // ── Measurement over the whole take ──
    int16_t mn = 32767, mx = -32768;
    double sumSq = 0;
    uint32_t zc = 0;
    int16_t prev = 0;
    for (uint32_t i = 0; i < got; i++) {
        int16_t v = pcm[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sumSq += (double)v * (double)v;
        if (i > 0 && ((prev < 0 && v >= 0) || (prev >= 0 && v < 0))) zc++;
        prev = v;
    }
    double rms = got ? sqrt(sumSq / (double)got) : 0.0;

    Serial.printf("[MICPROBE] bytesRead=%lu  samples=%lu\n",
                  (unsigned long)totalRead, (unsigned long)got);
    Serial.printf("[MICPROBE] min=%d  max=%d  RMS=%.1f  zeroCrossings=%lu\n",
                  mn, mx, rms, (unsigned long)zc);
    Serial.print("[MICPROBE] first16: ");
    for (int i = 0; i < 16 && i < (int)got; i++) Serial.printf("%d ", pcm[i]);
    Serial.println();

    const char* verdict;
    if (got == 0)                     verdict = "NO DATA  (i2s_read returned nothing)";
    else if (mx - mn < 8)             verdict = "FLAT     (ADC not producing audio)";
    else if (rms < 30.0)              verdict = "NEAR-SILENT (bytes flow, little energy)";
    else                              verdict = "ENERGY PRESENT (capture path is alive)";
    Serial.printf("[MICPROBE] VERDICT: %s\n", verdict);
    Serial.println("[MICPROBE] ===== capture end =====");

    // ── Write WAV to SD via pm_storage (best-effort) ──
    bool wavOk = false;
    if (pm_storage::ready()) {
        pm_storage::File f = pm_storage::open(MP_WAV_PATH, pm_storage::Mode::Write);
        if (f) {
            MP_WAV hdr;
            hdr.dataSize = got * 2;
            hdr.fileSize = hdr.dataSize + sizeof(MP_WAV) - 8;
            f.write((const uint8_t*)&hdr, sizeof(hdr));
            f.write((const uint8_t*)pcm, got * 2);
            f.flush();
            f.close();
            wavOk = true;
        }
    }

    free(pcm);

    // ── On-screen summary ──
    gfx->fillRect(0, MP_HEADER_H, 240, MP_BTN_Y - MP_HEADER_H, MP_BG);
    mp_status(String("bytes: ") + (unsigned long)totalRead, MP_WHITE, 0);
    mp_status(String("min/max: ") + mn + " / " + mx, MP_WHITE, 1);
    mp_status(String("RMS: ") + String(rms, 1), rms > 30 ? MP_GREEN : MP_AMBER, 2);
    mp_status(String("zero-cross: ") + (unsigned long)zc, MP_WHITE, 3);
    mp_status(String(verdict), (mx - mn < 8) ? MP_RED : MP_GREEN, 5);
    mp_status(wavOk ? "saved /mic_probe.wav" : "SD write skipped", MP_DIM, 7);
    mp_meter(rms > 32767 ? 32767 : (uint16_t)rms);
    mp_button("TAP: RUN AGAIN", MP_CYAN);
}

// ─────────────────────────────────────────────
//  Entry point
// ─────────────────────────────────────────────
void run_c28p_mic_probe() {
    gfx->fillScreen(MP_BG);
    mp_header();
    mp_status("ES8311 ADC bring-up...", MP_AMBER, 0);

    // Hand off codec + I2S setup to the audio HAL. RECORD mode also
    // disables the amplifier (the speaker driving the mic preamp is
    // a feedback loop) and discards a few DMA windows of warmup.
    if (!c28p_audio_enter(C28P_AUDIO_RECORD)) {
        mp_status("Audio HAL RECORD entry FAILED", MP_RED, 1);
        mp_status("(I2C codec? RX driver install?)", MP_DIM, 2);
        mp_status("Tap EXIT.", MP_DIM, 4);
        int16_t tx, ty;
        while (true) {
            if (c28p_touch_read(&tx, &ty) && ty < MP_HEADER_H) break;
            delay(20); yield();
        }
        gfx->fillScreen(MP_BG);
        return;
    }

    gfx->fillRect(0, MP_HEADER_H, 240, MP_BTN_Y - MP_HEADER_H, MP_BG);
    mp_status("Tap button to capture 3 seconds.", MP_CYAN, 0);
    mp_status("Speak / tap the mic during capture.", MP_DIM, 1);
    mp_meter(0);
    mp_button("TAP TO CAPTURE", MP_GREEN);

    // Tap-to-run loop. Header tap exits.
    bool wasTouched = false;
    while (true) {
        int16_t tx, ty;
        bool touched = c28p_touch_read(&tx, &ty);
        if (touched && !wasTouched) {
            if (ty < MP_HEADER_H) break;
            if (ty >= MP_BTN_Y && ty < MP_BTN_Y + MP_BTN_H) {
                mp_run_capture();
            }
        }
        wasTouched = touched;
        delay(20);
        yield();
    }

    // Drop the HAL all the way back to IDLE — driver uninstalled, amp
    // off, codec state preserved (it stays initialised so re-entering
    // another mode is a single i2s_install call away). Game audio can
    // resume in the same boot without a reset.
    c28p_audio_release();
    gfx->fillScreen(MP_BG);
    Serial.println("[MICPROBE] exit (HAL returned to IDLE; safe to use game tones)");
}

#endif // DEVICE_C28P
