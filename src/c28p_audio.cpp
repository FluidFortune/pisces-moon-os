// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_audio.cpp — ES8311 codec + I2S tone playback for C28P
//
//  The C28P board pairs an Everest ES8311 audio codec (I2C @ 0x18)
//  with an FM8002E audio amplifier (gated by IO1) and a small
//  speaker on the SPEAKER 1.25mm 2P connector. Plus a MEMS mic
//  for input.
//
//  This file implements the minimum needed to play tones for
//  game_audio.cpp's playTone(freq, ms) primitive:
//    1. Codec init over I2C — sample rate 16kHz, 16-bit mono
//    2. I2S0 init on the configured pins — master mode
//    3. Tone synthesis — square wave PCM samples streamed via DMA
//
//  Square waves are used rather than sine because:
//    - Authentic to chiptune-era game audio (Tetris on Game Boy etc.)
//    - Cheap to synthesize (just toggle ±amplitude per half-period)
//    - At small speaker sizes the harmonic difference is inaudible
//
//  Tone playback is blocking — playTone(freq, ms) returns after
//  the tone duration elapses. Tetris's note schedule has gaps
//  shorter than typical block-drop intervals, so a blocking
//  implementation works fine. Async tone playback with a DMA
//  callback would be a future refinement.
//
//  This is INTENTIONALLY a minimal driver. Real audio recording
//  (mic input), arbitrary sample playback (MP3, WAV), volume
//  control beyond the codec's default gain — all deferred to
//  v1.2.2 or v1.3 when there's a real audio app to drive them.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <math.h>
#include "hal_pins.h"

// ─────────────────────────────────────────────
//  ES8311 register map (minimal subset used here)
//
//  Full datasheet: lcdwiki.com/res/PublicFile/ES8311_DS.pdf
//
//  We use the codec in master-receive / slave-transmit mode
//  with respect to I2S: the ESP32-S3 is the I2S master, generating
//  BCK and LRCK; the ES8311 receives PCM from DOUT.
// ─────────────────────────────────────────────

#define ES8311_I2C_ADDR       0x18

// Register addresses
#define ES8311_RESET_REG00    0x00
#define ES8311_CLK_MANAGER1   0x01
#define ES8311_CLK_MANAGER2   0x02
#define ES8311_CLK_MANAGER3   0x03
#define ES8311_CLK_MANAGER4   0x04
#define ES8311_CLK_MANAGER5   0x05
#define ES8311_CLK_MANAGER6   0x06
#define ES8311_CLK_MANAGER7   0x07
#define ES8311_CLK_MANAGER8   0x08
#define ES8311_SDP_IN         0x09
#define ES8311_SDP_OUT        0x0A
#define ES8311_SYSTEM_REG0D   0x0D
#define ES8311_SYSTEM_REG0E   0x0E
#define ES8311_SYSTEM_REG12   0x12
#define ES8311_SYSTEM_REG13   0x13
#define ES8311_SYSTEM_REG14   0x14
#define ES8311_DAC_VOL_REG32  0x32   // DAC volume (0xBF = 0dB, 0x00 = mute)
#define ES8311_DAC_REG37      0x37

// ─────────────────────────────────────────────
//  I2C register write helper
// ─────────────────────────────────────────────
static bool es8311_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return (Wire.endTransmission() == 0);
}

// ─────────────────────────────────────────────
//  Codec init sequence
//
//  This is the minimal-but-working setup for 16kHz 16-bit mono
//  playback. Derived from the ES8311 datasheet's "Typical
//  application 1: I2S slave mode, MCLK from external" example
//  and adapted to ESP32-S3 master configuration. The codec
//  receives MCLK, BCK, LRCK, and DSDIN from the ESP32-S3 I2S
//  peripheral.
// ─────────────────────────────────────────────
static bool es8311_init() {
    // Reset
    if (!es8311_write(ES8311_RESET_REG00, 0x1F)) return false;
    delay(20);
    es8311_write(ES8311_RESET_REG00, 0x00);
    delay(10);

    // Clock manager: MCLK from external, 256*Fs (256 * 16kHz = 4.096MHz)
    es8311_write(ES8311_CLK_MANAGER1, 0x30);  // MCLK enable, slave mode
    es8311_write(ES8311_CLK_MANAGER2, 0x00);  // pre-divider = 1
    es8311_write(ES8311_CLK_MANAGER3, 0x10);  // FsDac = MCLK / 256
    es8311_write(ES8311_CLK_MANAGER4, 0x10);
    es8311_write(ES8311_CLK_MANAGER5, 0x00);
    es8311_write(ES8311_CLK_MANAGER6, 0x03);  // SCLK divider
    es8311_write(ES8311_CLK_MANAGER7, 0x00);
    es8311_write(ES8311_CLK_MANAGER8, 0xFF);

    // Serial data format — 16-bit I2S
    es8311_write(ES8311_SDP_IN,  0x0C);   // 16-bit input
    es8311_write(ES8311_SDP_OUT, 0x0C);   // 16-bit output

    // System power up
    es8311_write(ES8311_SYSTEM_REG0D, 0x01);
    es8311_write(ES8311_SYSTEM_REG0E, 0x02);
    es8311_write(ES8311_SYSTEM_REG12, 0x00);
    es8311_write(ES8311_SYSTEM_REG13, 0x10);
    es8311_write(ES8311_SYSTEM_REG14, 0x1A);

    // DAC volume — 0xBF = 0dB, comfortable max
    es8311_write(ES8311_DAC_VOL_REG32, 0xBF);
    es8311_write(ES8311_DAC_REG37, 0x08);

    Serial.println("[C28P] ES8311 codec initialized (16kHz/16-bit mono)");
    return true;
}

// ─────────────────────────────────────────────
//  I2S init
//
//  ESP32-S3 has two I2S peripherals; we use I2S_NUM_0.
//  Master mode, TX only, 16kHz, 16-bit, mono.
// ─────────────────────────────────────────────

#define C28P_AUDIO_SAMPLE_RATE  16000
#define C28P_AUDIO_BUFFER_LEN   512    // samples per buffer

static bool i2s_initialized = false;

static bool c28p_i2s_init() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = C28P_AUDIO_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = 0;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = C28P_AUDIO_BUFFER_LEN;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = PIN_I2S_MCLK;
    pins.bck_io_num   = PIN_I2S_SCLK;
    pins.ws_io_num    = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[C28P] i2s_driver_install failed: %d\n", err);
        return false;
    }
    err = i2s_set_pin(I2S_NUM_0, &pins);
    if (err != ESP_OK) {
        Serial.printf("[C28P] i2s_set_pin failed: %d\n", err);
        return false;
    }
    err = i2s_set_clk(I2S_NUM_0, C28P_AUDIO_SAMPLE_RATE,
                      I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    if (err != ESP_OK) {
        Serial.printf("[C28P] i2s_set_clk failed: %d\n", err);
        return false;
    }

    i2s_initialized = true;
    Serial.println("[C28P] I2S initialized (16kHz/16-bit mono, master TX)");
    return true;
}

// ─────────────────────────────────────────────
//  Public API — called from game_audio.cpp
// ─────────────────────────────────────────────

extern "C" {

// Called once at game audio start.
bool c28p_audio_begin() {
    if (i2s_initialized) return true;

    // Enable the FM8002E audio amplifier (IO1 active low)
    pinMode(PIN_AUDIO_EN, OUTPUT);
    digitalWrite(PIN_AUDIO_EN, LOW);
    delay(5);

    if (!es8311_init()) {
        Serial.println("[C28P] Audio begin failed — codec init error");
        return false;
    }
    if (!c28p_i2s_init()) {
        Serial.println("[C28P] Audio begin failed — I2S init error");
        return false;
    }
    return true;
}

// Stop any in-progress playback and disable the amplifier so the
// speaker doesn't draw idle current.
void c28p_audio_stop() {
    if (!i2s_initialized) return;
    i2s_zero_dma_buffer(I2S_NUM_0);
    digitalWrite(PIN_AUDIO_EN, HIGH);   // amp disabled (active low)
}

// Synthesize a square wave at `freq` Hz for `ms` milliseconds and
// stream it through I2S. Blocks until duration elapses. freq=0 is
// silence (rest).
void c28p_audio_tone(uint16_t freq, uint16_t ms) {
    if (!i2s_initialized) return;
    if (ms == 0) return;

    // Re-enable amp in case it was stopped between notes
    digitalWrite(PIN_AUDIO_EN, LOW);

    if (freq == 0) {
        // Rest — write zeros for the duration
        const int zero_samples = (C28P_AUDIO_SAMPLE_RATE * ms) / 1000;
        int16_t zero_buf[64] = {0};
        size_t written;
        int remaining = zero_samples;
        while (remaining > 0) {
            int chunk = (remaining > 64) ? 64 : remaining;
            i2s_write(I2S_NUM_0, zero_buf, chunk * sizeof(int16_t),
                      &written, portMAX_DELAY);
            remaining -= chunk;
        }
        return;
    }

    // Square wave: half-period of high, half of low. At freq Hz and
    // 16kHz sample rate, half-period = SR/(2*freq) samples.
    const int half_period = C28P_AUDIO_SAMPLE_RATE / (2 * freq);
    const int16_t amplitude = 8000;   // ~24% of full scale — comfortable

    const int total_samples = (C28P_AUDIO_SAMPLE_RATE * ms) / 1000;
    int16_t buf[64];
    int phase = 0;   // counts samples within current half-period
    bool high = true;

    int remaining = total_samples;
    while (remaining > 0) {
        int chunk = (remaining > 64) ? 64 : remaining;
        for (int i = 0; i < chunk; i++) {
            buf[i] = high ? amplitude : -amplitude;
            if (++phase >= half_period) {
                phase = 0;
                high = !high;
            }
        }
        size_t written;
        i2s_write(I2S_NUM_0, buf, chunk * sizeof(int16_t),
                  &written, portMAX_DELAY);
        remaining -= chunk;
    }
}

} // extern "C"

#endif // DEVICE_C28P