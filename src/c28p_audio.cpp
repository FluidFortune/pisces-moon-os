// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_audio.cpp — ES8311 codec HAL with explicit mode machine
//
//  The C28P board pairs an Everest ES8311 codec (I2C @ 0x18) with an
//  FM8002E amplifier (gated by PIN_AUDIO_EN) and a small speaker. The
//  same codec has a MEMS mic on the ADC side. ADC and DAC share the
//  same I2S serial port — they can't both stream at once.
//
//  v1.3 refactor: this file now exposes c28p_audio_enter(MODE) /
//  c28p_audio_release() so callers can switch cleanly between game
//  audio (TX tone), MP3 playback (caller-owned I2S driver), and mic
//  recording (RX). v1.2.x's c28p_audio_begin / c28p_audio_stop /
//  c28p_audio_tone API is preserved as thin wrappers; nothing on
//  disk had to change at the call sites unless it wanted to.
//
//  Why this matters: the v1.2.x build kept a static i2s_initialized
//  flag inside this file. The mic probe (TOOLS → MIC TEST) calls
//  i2s_driver_uninstall() directly, but couldn't reset that flag.
//  After running the probe, game tones would silently fail because
//  c28p_audio_begin() saw the flag still set and returned early
//  without reinstalling the driver. The fix isn't a "remember to
//  reset the flag" — it's making the state public and explicit.
//
//  TRANSITIONS (state diagram):
//
//             enter(TONE)              enter(AUDIO)
//      ┌──────────────────┐      ┌─────────────────────┐
//      │                  ▼      │                     ▼
//   ┌──────┐         ┌──────────────┐         ┌──────────────────┐
//   │ IDLE │ ────►   │ PLAYBACK_    │         │ PLAYBACK_AUDIO   │
//   │      │         │   TONE       │         │ (caller owns I2S)│
//   └──────┘         └──────────────┘         └──────────────────┘
//      ▲                  │                          │
//      │                  └────────── release ───────┘
//      │
//      │ release / enter(RECORD)
//      │
//   ┌──────────┐
//   │ RECORD   │
//   │          │
//   └──────────┘
//
//  Internal state transitions all go through _teardown_current(),
//  which leaves the HAL in IDLE without releasing the static codec-
//  initialised flag. The codec stays alive across modes because its
//  I2C bring-up is expensive and producing/consuming side-effect-
//  free.
// ─────────────────────────────────────────────

#ifdef DEVICE_C28P

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <math.h>
#include "hal_pins.h"
#include "c28p_audio.h"

// ─────────────────────────────────────────────
//  ES8311 register map (subset used here)
// ─────────────────────────────────────────────

#define ES8311_I2C_ADDR       0x18

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
#define ES8311_SYSTEM_REG0B   0x0B
#define ES8311_SYSTEM_REG0C   0x0C
#define ES8311_SYSTEM_REG0D   0x0D
#define ES8311_SYSTEM_REG0E   0x0E
#define ES8311_SYSTEM_REG10   0x10
#define ES8311_SYSTEM_REG11   0x11
#define ES8311_SYSTEM_REG12   0x12
#define ES8311_SYSTEM_REG13   0x13
#define ES8311_SYSTEM_REG14   0x14
#define ES8311_ADC_REG15      0x15
#define ES8311_ADC_REG16      0x16
#define ES8311_ADC_REG17      0x17
#define ES8311_ADC_REG1B      0x1B
#define ES8311_ADC_REG1C      0x1C
#define ES8311_DAC_VOL_REG32  0x32
#define ES8311_DAC_REG37      0x37
#define ES8311_GPIO_REG44     0x44
#define ES8311_GP_REG45       0x45

// ─────────────────────────────────────────────
//  State (file-static)
//
//  _codec_initialized — has es8311_init() ever succeeded? We only do
//    the I2C bring-up once per boot; transitions between modes flip
//    the amp / I2S driver but don't re-pump the codec registers.
//
//  _current_mode      — the mode the HAL is currently in. Transitions
//    are: IDLE ↔ {PLAYBACK_TONE, PLAYBACK_AUDIO, RECORD}. Mode-to-mode
//    always tears down current via IDLE before entering target.
//
//  _hal_owns_driver   — true when the HAL itself installed the I2S
//    driver (PLAYBACK_TONE or RECORD). false in PLAYBACK_AUDIO since
//    the caller (e.g. the ESP32-audioI2S Audio library) installs and
//    uninstalls its own driver. Used to decide whether _teardown_
//    current() should call i2s_driver_uninstall().
// ─────────────────────────────────────────────

#define C28P_AUDIO_SAMPLE_RATE   16000
#define C28P_AUDIO_BUFFER_LEN    512
#define C28P_RX_DMA_BUF_COUNT    6
#define C28P_RX_DMA_BUF_LEN      256

// Tone amplitude (int16 sample peak). Was 8000, which made game audio
// uncomfortably loud through the SC8002B once the DAC path started
// working. 4000 is ~half and comfortable; WAV/MP3 playback is
// unaffected (it runs through the codec DAC at REG32 volume, not this
// constant). For the 2-voice mixer each voice uses half this so the
// summed peak still lands at C28P_TONE_AMP.
#define C28P_TONE_AMP            4000

static bool              _codec_initialized = false;
static c28p_audio_mode_t _current_mode      = C28P_AUDIO_IDLE;
static bool              _hal_owns_driver   = false;

// Forward declarations — amp_disable() is defined below alongside
// amp_enable() and the codec mute helpers, but es8311_init() needs to
// call it to silence the amp before the first codec writes (see the
// boot-time crackling note inside es8311_init).
static void amp_disable();

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
//  Codec init (idempotent after first success)
//
//  Verified register values from Espressif's esp-bsp ES8311 driver.
//  Same sequence as v1.2.1 — only structural change is the call is
//  now guarded by _codec_initialized so we don't re-pump it on every
//  mode transition.
// ─────────────────────────────────────────────
static bool es8311_init() {
    if (_codec_initialized) return true;

    // Critical ordering: disable the amp BEFORE any codec writes. At
    // boot the SC8002B SHUTDOWN line is held HIGH by R28's pull-up
    // (per schematic), leaving the amp ENABLED by default. If we run
    // the reset/PLL sequence below with the amp on, every codec
    // transient gets amplified into audible crackling.
    amp_disable();

    // ── Reset ──────────────────────────────────────────────────────
    if (!es8311_write(ES8311_RESET_REG00, 0x1F)) return false;
    delay(20);
    es8311_write(ES8311_RESET_REG00, 0x00);
    delay(10);
    es8311_write(ES8311_RESET_REG00, 0x80);   // power-on
    delay(10);

    // ── Clock manager ──────────────────────────────────────────────
    // These match the Espressif ADF reference driver (es8311.c from
    // the esp-idf audio_hal — confirmed against maxgerhardt's
    // pio-esp-adf-example which is a faithful copy of the Espressif
    // reference init sequence for 16kHz operation).
    es8311_write(ES8311_CLK_MANAGER1, 0x3F);
    es8311_write(ES8311_CLK_MANAGER2, 0x00);
    es8311_write(ES8311_CLK_MANAGER3, 0x10);
    es8311_write(ES8311_CLK_MANAGER4, 0x10);
    es8311_write(ES8311_CLK_MANAGER5, 0x00);
    es8311_write(ES8311_CLK_MANAGER6, 0x03);
    es8311_write(ES8311_CLK_MANAGER7, 0x00);
    es8311_write(ES8311_CLK_MANAGER8, 0xFF);

    // ── Serial data port ───────────────────────────────────────────
    // 0x0C = 16-bit I2S, slave mode, standard I2S format.
    es8311_write(ES8311_SDP_IN,  0x0C);
    es8311_write(ES8311_SDP_OUT, 0x0C);

    // ── System / power ─────────────────────────────────────────────
    // Sequence reverted to exactly what the verified-working mic probe
    // wrote in the earlier session that captured RMS=474 from the mic.
    // Additions beyond this set (REG0B/0C state-machine reset, REG10/11
    // bias/VMID override, REG15 ALC, REG1B HPF) are removed because
    // they were added later as speculative "improvements" and coincide
    // with audio breaking. REG15 ALC in particular gates low-level
    // signal to zero and is a likely cause of the mic returning all
    // zeros despite REG14 + REG17 being correct.
    es8311_write(ES8311_SYSTEM_REG0D, 0x01);  // reference voltage / LDO
    es8311_write(ES8311_SYSTEM_REG0E, 0x02);  // enable analog PGA + ADC modulator
    es8311_write(ES8311_SYSTEM_REG12, 0x00);  // DAC powered
    es8311_write(ES8311_SYSTEM_REG13, 0x10);  // analog ADC bias
    es8311_write(ES8311_ADC_REG1C,    0x6A);  // ADC SDM config

    // ── DAC volume (operating level) ─────────────────────────────
    // REG32 = 0xBF is needed for PLAYBACK_TONE / PLAYBACK_AUDIO.
    // The probe didn't set it (RX-only) but it's needed once we drive
    // the DAC. codec_mute_dac() will override to 0x00 when needed.
    es8311_write(ES8311_DAC_VOL_REG32, 0xD8);

    // ── REG37 DAC fade/ramp ─────────────────────────────────────────
    // 0x08 = FADE_OFF (low-nibble 0x08 is the driver's resting value).
    // This register is NOT a mute gate — see codec_unmute_dac(). It
    // must stay 0x08 for the full lifetime; a nonzero high nibble
    // enables a volume ramp that swallows short game tones.
    es8311_write(ES8311_DAC_REG37, 0x08);

    // ── GPIO / routing ─────────────────────────────────────────────
    // REG44 = 0x08 normal GPIO mode (no DAC→ADC loopback).
    // REG45 = 0x00 normal (probe omitted but reset default is safe).
    es8311_write(ES8311_GPIO_REG44, 0x08);
    es8311_write(ES8311_GP_REG45,   0x00);

    // ── Microphone config (MUST come at end of init) ───────────────
    // Espressif BSP runs es8311_microphone_config() AFTER es8311_init().
    // Order matters: REG17 ADC gain set before REG14 mic-PGA enable
    // before REG16 digital gain. The verified-working probe followed
    // this order and captured RMS=474.
    es8311_write(ES8311_ADC_REG17,    0xC8);  // ADC gain (analog PGA path)
    es8311_write(ES8311_SYSTEM_REG14, 0x1A);  // enable analog mic + max PGA gain
    es8311_write(ES8311_ADC_REG16,    0x07);  // ADC PGA digital gain = +42dB (max)
                                              // demo leaves this at 0x00, but this
                                              // board's MEMS mic reads near-silent
                                              // (RMS~7) at 0dB; +42dB lifts to usable.

    _codec_initialized = true;
    Serial.println("[C28P-AUDIO] ES8311 codec initialized");
    return true;
}

// ─────────────────────────────────────────────
//  Amplifier control (SC8002B on PIN_AUDIO_EN)
//
//  POLARITY:
//    IO1 HIGH → amp ENABLED (normal operation)
//    IO1 LOW  → amp SHUTDOWN (silent, low power)
//
//  CRITICAL NOTE — the LCDWiki text documentation for this board
//  says "IO1 low level enable, high level disable". That is WRONG
//  for the SC8002B (the chip actually on the board per the schematic
//  — LCDWiki's prose mentions FM8002E, but U6 in the schematic is
//  SC8002B). The SC8002B datasheet defines its SHUTDOWN pin as
//  active-low: LOW = shutdown, HIGH = normal operation. The standard
//  reference circuit puts a pull-up on SHUTDOWN so the amp comes up
//  enabled by default; driving IO1 low actively overrides that
//  pull-up and shuts the amp down.
//
//  Empirical confirmation: with IO1 driven LOW "to enable" (matching
//  LCDWiki text), audio on this board NEVER worked from first
//  bring-up. Codec init succeeded, I2S DMA installed, tone DMA writes
//  succeeded, [C28P-AUDIO] → PLAYBACK_TONE logged — but the speaker
//  stayed silent. Flipping the polarity here is what actually makes
//  sound come out.
//
//  Trust silicon over documentation. The schematic and the datasheet
//  agree; the LCDWiki English prose is mistranslated or copy-pasted
//  from a different chip.
// ───────────────────────────────────────────────
static void amp_enable() {
    pinMode(PIN_AUDIO_EN, OUTPUT);
    digitalWrite(PIN_AUDIO_EN, LOW);    // LOW = amp ON (active-low shutdown)
    // Confirmed by BruceDevices/firmware Issue #2117 for this exact board
    // (ES3C28P / ES3N28P): GPIO1 LOW enables the amplifier. LCDWiki docs
    // are correct on this point. The SC8002B datasheet active-low SHUTDOWN
    // pin is driven directly by GPIO1 with no inverter on this PCB.
}

static void amp_disable() {
    pinMode(PIN_AUDIO_EN, OUTPUT);
    digitalWrite(PIN_AUDIO_EN, HIGH);   // HIGH = amp shutdown
}

// ─────────────────────────────────────────────
//  Codec DAC mute (ES8311 register 0x32 / DAC_VOL)
//
//  Writing 0x00 puts the DAC at -96 dB (effective mute). Writing
//  0xBF restores the operating volume set during es8311_init().
//
//  Why this exists: class-D amps like the SC8002B have a wake-up
//  transient when SHUTDOWN transitions LOW→HIGH. The FM8002E
//  datasheet (sibling part) gives this as TD = 100ms typical. During
//  the wake-up window the amp output stage is ramping its bias; any
//  audio flowing through is distorted into audible pops. The mirror
//  happens on shutdown if real audio is still being fed in. The fix
//  is to mute the SOURCE during amp transitions so the speaker
//  hears silence while the amp is ramping. We mute the codec DAC
//  via I2C (~100us per write) rather than relying on the amp's own
//  pop-suppression circuit, which empirically isn't sufficient.
// ─────────────────────────────────────────────
static void codec_mute_dac() {
    // Digital-silence the DAC during amp transitions (pop suppression).
    // REG37 stays at its init value (0x08, FADE_OFF) — never touch it.
    es8311_write(ES8311_DAC_VOL_REG32, 0x00);
}

static void codec_unmute_dac() {
    // Restore operating volume. REG37 is LEFT at 0x08.
    //
    // REG37 is the DAC fade/ramp register (es8311_voice_fade in the
    // authoritative LCDWiki/Espressif es8311 driver for THIS board):
    // high nibble = fade ramp rate, low nibble keeps 0x08. It is NOT
    // an output-stage gate.
    //
    // Our previous code wrote 0x48 here, copied from a DIFFERENT ADF
    // driver where we misread it as "operating state". 0x48 actually
    // means FADE_32LRCK — a slow volume ramp that takes hundreds of ms
    // to reach full scale at 16kHz. Short game tones (68–136ms) ended
    // before the ramp finished, so Tetris was inaudible while longer
    // file playback (seconds) faded up fine. Gate audio with the amp
    // and REG32 only; leave REG37 alone.
    es8311_write(ES8311_DAC_VOL_REG32, 0xD8);  // operating volume (~85%)
}

// ─────────────────────────────────────────────
//  I2S install — TX-only (for PLAYBACK_TONE)
// ─────────────────────────────────────────────
static bool i2s_install_tx() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = C28P_AUDIO_SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    // STEREO (RIGHT_LEFT) to match the ESP32-audioI2S library config,
    // which is the PROVEN-WORKING DAC path on this board (recordings
    // play back fine through it). The ES8311 is a mono codec but it
    // expects a standard 2-slot stereo I2S frame and the DAC reads one
    // slot. A mono (ONLY_LEFT) frame structure does NOT drive the DAC
    // on this board — that was an earlier wrong guess (copied from the
    // RX/RECORD path, but RX mono extraction is not symmetric with TX
    // mono generation). We duplicate each sample into L+R below.
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = C28P_AUDIO_BUFFER_LEN;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[C28P-AUDIO] TX driver install failed: %d\n", err);
        return false;
    }

    // Set BCLK/LRCK/DOUT/MCLK AFTER i2s_set_clk so that any internal
    // clock-recalculation inside set_clk doesn't disturb the MCLK pin
    // routing we set here. (PLAYBACK_AUDIO adds MCLK the same way —
    // after all driver config — and that path produces audible output.)
    i2s_set_clk(I2S_NUM_0, C28P_AUDIO_SAMPLE_RATE,
                I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = PIN_I2S_MCLK;
    pins.bck_io_num   = PIN_I2S_SCLK;
    pins.ws_io_num    = PIN_I2S_LRCK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_NUM_0, &pins);

    return true;
}

// ─────────────────────────────────────────────
//  I2S install — RX-only (for RECORD)
//
//  Larger DMA buffer count than TX to absorb scheduling jitter:
//  the recording loop runs interleaved with display refresh and
//  touch polling, and missing a DMA buffer causes audible clicks.
// ─────────────────────────────────────────────
static bool i2s_install_rx() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = C28P_AUDIO_SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = C28P_RX_DMA_BUF_COUNT;
    cfg.dma_buf_len          = C28P_RX_DMA_BUF_LEN;
    // See note on use_apll in i2s_install_tx() — same reason here.
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = PIN_I2S_MCLK;
    pins.bck_io_num   = PIN_I2S_SCLK;
    pins.ws_io_num    = PIN_I2S_LRCK;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = PIN_I2S_DIN;

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        Serial.printf("[C28P-AUDIO] RX driver install failed: %d\n", err);
        return false;
    }
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_set_clk(I2S_NUM_0, C28P_AUDIO_SAMPLE_RATE,
                I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

    // Discard a few DMA windows of warmup garbage (codec PLL + mic bias settle).
    delay(120);
    uint8_t warmup[512];
    size_t  got = 0;
    for (int i = 0; i < 8; i++) {
        i2s_read(I2S_NUM_0, warmup, sizeof(warmup), &got, pdMS_TO_TICKS(50));
    }
    return true;
}

// ─────────────────────────────────────────────
//  Teardown — return the HAL to a clean IDLE
//
//  Only uninstall the I2S driver if we installed it. In
//  PLAYBACK_AUDIO mode the caller owns the driver; we just shut
//  off the amp and forget. The caller is expected to have called
//  c28p_audio_release_driver() if they were the one running.
// ─────────────────────────────────────────────
static void _teardown_current() {
    if (_current_mode == C28P_AUDIO_IDLE) {
        amp_disable();
        return;
    }

    // Coming out of a playback mode: mute the codec DAC FIRST so the
    // amp's shutdown transient happens against a silent signal. We
    // don't bother muting on RECORD teardown — amp was already off
    // in that mode (feedback risk during recording).
    if (_current_mode == C28P_AUDIO_PLAYBACK_TONE ||
        _current_mode == C28P_AUDIO_PLAYBACK_AUDIO) {
        codec_mute_dac();
        delay(5);   // let codec output settle to silence
    }

    if (_hal_owns_driver) {
        // Flush any pending TX data first to avoid a click on amp-off.
        if (_current_mode == C28P_AUDIO_PLAYBACK_TONE) {
            i2s_zero_dma_buffer(I2S_NUM_0);
        } else if (_current_mode == C28P_AUDIO_RECORD) {
            i2s_stop(I2S_NUM_0);
        }
        i2s_driver_uninstall(I2S_NUM_0);
        _hal_owns_driver = false;
    }
    amp_disable();
    _current_mode = C28P_AUDIO_IDLE;
}

// ─────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────
extern "C" {

bool c28p_audio_enter(c28p_audio_mode_t mode) {
    if (mode == _current_mode) {
        return true;   // already there — no-op
    }

    // Make sure the codec is alive (one-time init).
    if (!es8311_init()) {
        Serial.println("[C28P-AUDIO] Codec init failed — staying IDLE");
        _teardown_current();
        return false;
    }

    // Drop to IDLE before installing new state, regardless of the
    // direction. _teardown_current() is a no-op if we're already idle.
    _teardown_current();

    switch (mode) {
        case C28P_AUDIO_IDLE:
            return true;

        case C28P_AUDIO_PLAYBACK_TONE: {
            // Codec stays muted until the amp has woken up. This
            // prevents the turn-on transient from being amplified
            // into an audible pop.
            codec_mute_dac();

            if (!i2s_install_tx()) return false;
            _hal_owns_driver = true;

            // I2S DMA is now installed and pumping zeros (tx_desc_auto_
            // clear=true). Give the codec ~10ms to lock onto BCLK and
            // reach steady-state DC bias before un-shutdowning the amp.
            delay(10);
            amp_enable();

            // Amp wake-up settle. The FM8002E datasheet (a sibling
            // class-D amp) lists TD = 100ms typical for full wake-up
            // from shutdown. We give it 60ms which is enough in
            // practice for SC8002B; any audio data fed during this
            // window goes into a still-ramping output stage and
            // sounds distorted/pops. Codec is muted during this whole
            // window so the speaker hears silence regardless.
            delay(60);
            codec_unmute_dac();

            _current_mode = C28P_AUDIO_PLAYBACK_TONE;
            Serial.println("[C28P-AUDIO] → PLAYBACK_TONE");
            return true;
        }

        case C28P_AUDIO_PLAYBACK_AUDIO: {
            // Caller installs their own I2S driver. We set up codec +
            // amp and step out of the way. They must call
            // c28p_audio_release_driver() before c28p_audio_release()
            // so we don't try to uninstall a driver they own.
            //
            // Same mute-during-wake-up dance as TONE. The Audio
            // library will start decoding immediately after we return
            // — the codec mute keeps the wake-up transient inaudible
            // and we unmute right before returning so the first audio
            // frame from the library plays through a stable amp.
            codec_mute_dac();
            delay(10);
            amp_enable();
            delay(60);
            codec_unmute_dac();

            _hal_owns_driver = false;
            _current_mode = C28P_AUDIO_PLAYBACK_AUDIO;
            Serial.println("[C28P-AUDIO] → PLAYBACK_AUDIO (caller owns I2S)");
            return true;
        }

        case C28P_AUDIO_RECORD: {
            // Amp stays disabled during record — speaker driving the
            // mic preamp is a feedback loop.
            amp_disable();

            // Mute the codec DAC during recording. The ES8311's DAC
            // stays powered up by default (SYSTEM_REG12 = 0x00) and
            // its volume is at operating level (DAC_VOL_REG32 = 0xBF)
            // from init. With no TX data being driven (we're in I2S
            // RX-only mode), the DAC outputs its idle noise floor
            // straight into the amp's input pins. The amp is in
            // shutdown, but class-D amps have finite shutdown
            // isolation (typ. 60-80 dB) — enough idle DAC noise leaks
            // through to be audible as continuous crackling during
            // recording. Muting the DAC at the codec level removes
            // the source signal regardless of amp leakage.
            codec_mute_dac();

            if (!i2s_install_rx()) {
                _current_mode = C28P_AUDIO_IDLE;
                return false;
            }
            _hal_owns_driver = true;
            _current_mode = C28P_AUDIO_RECORD;
            Serial.println("[C28P-AUDIO] → RECORD");
            return true;
        }
    }

    // Unknown mode value — keep us in a safe state.
    _teardown_current();
    return false;
}

c28p_audio_mode_t c28p_audio_current_mode(void) {
    return _current_mode;
}

void c28p_audio_release(void) {
    _teardown_current();
}

void c28p_audio_release_driver(void) {
    // Caller is telling us they removed their own I2S driver — we
    // don't own it, just forget the mode. _teardown_current() will
    // disable the amp without touching the driver.
    _hal_owns_driver = false;
    _teardown_current();
}

// ── Backwards-compat wrappers ──
//
// c28p_audio_begin() historically did "init codec + install TX
// driver + enable amp". That's now C28P_AUDIO_PLAYBACK_TONE. If
// we're already past IDLE (e.g. PLAYBACK_AUDIO or RECORD) we
// preserve that — entering TONE would knock the other mode out.

bool c28p_audio_begin(void) {
    // v1.3.1: always enter PLAYBACK_TONE here, not just from IDLE.
    //
    // The original wrapper short-circuited if already in any non-IDLE
    // mode (the intent was "don't disturb an active session"). In
    // practice this caused silent audio failures: if the HAL was in
    // PLAYBACK_AUDIO (a prior MP3 session that didn't tear down) or
    // RECORD (mic probe leftover), the wrapper returned true without
    // entering PLAYBACK_TONE — and c28p_audio_tone() then bailed
    // because _current_mode wasn't PLAYBACK_TONE.
    //
    // c28p_audio_enter() handles "same mode = no-op" at the top, so
    // it's safe to call unconditionally. The result is robust to
    // whatever state the HAL was previously left in: tetris (and any
    // other game) gets audio every time, regardless of what ran before.
    //
    // DO NOT re-add the IDLE guard here. If audio worked once and then
    // stopped after running media/mic, the IDLE guard is back.
    return c28p_audio_enter(C28P_AUDIO_PLAYBACK_TONE);
}

void c28p_audio_stop(void) {
    // Old semantics were "stop playback but don't tear down" — i.e.
    // zero DMA and disable amp. We approximate by going to IDLE,
    // which the next c28p_audio_begin() will undo. This is the right
    // behavior for game-audio teardown.
    c28p_audio_release();
}

void c28p_audio_tone(uint16_t freq, uint16_t ms) {
    static uint32_t _tone_call_count = 0;
    static c28p_audio_mode_t _last_logged_mode = (c28p_audio_mode_t)0xFF;
    if (_tone_call_count < 3 || _current_mode != _last_logged_mode) {
        Serial.printf("[C28P-AUDIO] tone(freq=%u ms=%u) mode=%d call=%lu\n",
                      freq, ms, (int)_current_mode, (unsigned long)_tone_call_count);
        _last_logged_mode = _current_mode;
    }
    _tone_call_count++;

    if (_current_mode != C28P_AUDIO_PLAYBACK_TONE) return;
    if (ms == 0) return;

    // Amp is already enabled by c28p_audio_enter(PLAYBACK_TONE). Do NOT
    // re-assert it here. Calling amp_enable() on every note was a v1.3
    // belt-and-suspenders measure that turned out to introduce its own
    // problems: pinMode(OUTPUT) on a pin that's already an output is a
    // wasted system call, and on some ESP-IDF versions the brief
    // reconfigure window can cause a glitch audible as a per-note tick.
    // The amp stays on for the whole PLAYBACK_TONE session and goes
    // down only when c28p_audio_release() or c28p_audio_stop() runs.

    if (freq == 0) {
        const int zero_frames = (C28P_AUDIO_SAMPLE_RATE * ms) / 1000;
        int16_t zero_buf[128] = {0};   // 64 stereo frames (L+R)
        size_t written;
        int remaining = zero_frames;
        while (remaining > 0) {
            int chunk = (remaining > 64) ? 64 : remaining;
            const size_t expected = chunk * 2 * sizeof(int16_t);  // stereo
            esp_err_t err = i2s_write(I2S_NUM_0, zero_buf, expected,
                                      &written, portMAX_DELAY);
            if (err != ESP_OK || written != expected) {
                Serial.printf("[C28P-AUDIO] zero write err=%d bytes=%u/%u\n",
                              err, (unsigned)written, (unsigned)expected);
                break;
            }
            remaining -= chunk;
        }
        return;
    }

    const int half_period = C28P_AUDIO_SAMPLE_RATE / (2 * freq);
    const int16_t amplitude = C28P_TONE_AMP;
    const int total_samples = (C28P_AUDIO_SAMPLE_RATE * ms) / 1000;
    int16_t buf[128];   // 64 stereo frames (L+R duplicated)
    int phase = 0;
    bool high = true;
    int remaining = total_samples;
    while (remaining > 0) {
        int chunk = (remaining > 64) ? 64 : remaining;
        for (int i = 0; i < chunk; i++) {
            int16_t sample = high ? amplitude : -amplitude;
            buf[i * 2]     = sample;   // L
            buf[i * 2 + 1] = sample;   // R
            if (++phase >= half_period) {
                phase = 0;
                high = !high;
            }
        }
        size_t written;
        const size_t expected = chunk * 2 * sizeof(int16_t);  // stereo
        esp_err_t err = i2s_write(I2S_NUM_0, buf, expected,
                                  &written, portMAX_DELAY);
        if (err != ESP_OK || written != expected) {
            Serial.printf("[C28P-AUDIO] tone write err=%d bytes=%u/%u\n",
                          err, (unsigned)written, (unsigned)expected);
            break;
        }
        remaining -= chunk;
    }
}

// ─────────────────────────────────────────────
//  c28p_audio_tone2 — two-voice (polyphonic) square-wave mixer
//
//  Plays two independent square waves summed into one signal, so a
//  game can have a melody voice and a bass voice at once. Each voice
//  contributes ±(C28P_TONE_AMP/2); when both voices are high the sum
//  reaches ±C28P_TONE_AMP (no clipping at int16). A voice with freq 0
//  is silent, in which case the other voice plays alone at half
//  amplitude (naturally softer when the bass drops out — sounds fine).
//
//  Same DMA/stereo conventions as c28p_audio_tone(): samples are
//  duplicated into L+R, written in 64-frame chunks.
// ─────────────────────────────────────────────
void c28p_audio_tone2(uint16_t f1, uint16_t f2, uint16_t ms) {
    if (_current_mode != C28P_AUDIO_PLAYBACK_TONE) return;
    if (ms == 0) return;

    const int16_t VOICE = C28P_TONE_AMP / 2;
    const int total_samples = (C28P_AUDIO_SAMPLE_RATE * ms) / 1000;

    // Per-voice half-period (in samples). 0 freq → voice disabled.
    const int hp1 = (f1 > 0) ? (int)(C28P_AUDIO_SAMPLE_RATE / (2 * f1)) : 0;
    const int hp2 = (f2 > 0) ? (int)(C28P_AUDIO_SAMPLE_RATE / (2 * f2)) : 0;

    int   ph1 = 0, ph2 = 0;
    bool  hi1 = true, hi2 = true;
    int16_t buf[128];   // 64 stereo frames (L+R duplicated)
    int remaining = total_samples;

    while (remaining > 0) {
        int chunk = (remaining > 64) ? 64 : remaining;
        for (int i = 0; i < chunk; i++) {
            int32_t s = 0;
            if (hp1 > 0) {
                s += hi1 ? VOICE : -VOICE;
                if (++ph1 >= hp1) { ph1 = 0; hi1 = !hi1; }
            }
            if (hp2 > 0) {
                s += hi2 ? VOICE : -VOICE;
                if (++ph2 >= hp2) { ph2 = 0; hi2 = !hi2; }
            }
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            buf[i * 2]     = (int16_t)s;   // L
            buf[i * 2 + 1] = (int16_t)s;   // R
        }
        size_t written;
        const size_t expected = chunk * 2 * sizeof(int16_t);  // stereo
        esp_err_t err = i2s_write(I2S_NUM_0, buf, expected,
                                  &written, portMAX_DELAY);
        if (err != ESP_OK || written != expected) {
            Serial.printf("[C28P-AUDIO] tone2 write err=%d bytes=%u/%u\n",
                          err, (unsigned)written, (unsigned)expected);
            break;
        }
        remaining -= chunk;
    }
}

}  // extern "C"

// ═════════════════════════════════════════════
//  NON-BLOCKING MUSIC ENGINE
//
//  FreeRTOS task pinned to Core 0 owns I2S TX while the game runs
//  on Core 1. The task continuously generates polyphonic square-wave
//  samples from a song descriptor, writes them to I2S DMA (which
//  paces the loop at the codec's 16kHz sample rate), and self-
//  advances through the song. The game thread changes songs / queues
//  FX via setters that update a critical-section-protected shared
//  state struct — no blocking, no synchronization on the game side
//  beyond a brief lock.
//
//  Voice budget: each voice contributes ±(C28P_TONE_AMP/3) so three
//  voices (melody + bass + FX overlay) sum to ±C28P_TONE_AMP without
//  clipping. Game audio still sounds the same loudness as before
//  because the amp's input gain is unchanged — only the digital mix
//  was rebalanced.
// ═════════════════════════════════════════════

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t  _music_task_handle  = NULL;
static volatile bool _music_task_running = false;
static portMUX_TYPE  _engine_mux         = portMUX_INITIALIZER_UNLOCKED;

static struct {
    const c28p_note_t* song;            // NULL = silence
    uint16_t           song_len;
    bool               pending_change;
    uint16_t           fx_freq;
    uint16_t           fx_ms;
    bool               pending_fx;
} _shared = {0};

static void _music_task(void* arg) {
    (void)arg;
    const int CHUNK = 64;        // 64 stereo frames per DMA write
    int16_t buf[CHUNK * 2];

    // Engine-local state (owned by task, no locking needed)
    const c28p_note_t* song = NULL;
    uint16_t song_len = 0;
    uint16_t note_idx = 0;
    uint32_t samples_into_note = 0;
    uint32_t total_samples_in_note = 0;
    uint32_t sound_samples = 0;  // 85% of total — rest is articulation gap
    int  hp_mel = 0, hp_bass = 0;
    int  phase_mel = 0, phase_bass = 0;
    bool hi_mel = true, hi_bass = true;
    bool note_loaded = false;

    // FX overlay state
    int  fx_hp = 0;
    int  fx_phase = 0;
    bool fx_hi = true;
    uint32_t fx_remaining = 0;

    // Monophonic melody — the bass voice in each PMVoice/c28p_note_t
    // is INTENTIONALLY IGNORED here. We tried two-voice polyphony
    // (melody + bass) through the C28P's small speaker and it sounded
    // awful: two square waves summed create sum/difference frequencies
    // that the cheap speaker emphasizes as beating and intermodulation
    // distortion. The score wants polyphony but the hardware can't
    // render it cleanly. Single-voice melody at full duration is the
    // closest we can get to the intended music on this speaker.
    //
    // Two voices remain in the mix budget: melody + one FX overlay,
    // so each gets half of C28P_TONE_AMP (no clipping when both are
    // active).
    const int16_t VOICE = C28P_TONE_AMP / 2;

    while (_music_task_running) {
        // ── Pick up changes from game thread (brief lock) ──
        const c28p_note_t* new_song = NULL;
        uint16_t new_song_len = 0;
        bool song_changed = false;
        uint16_t new_fx_freq = 0;
        uint16_t new_fx_ms = 0;
        bool fx_queued = false;

        portENTER_CRITICAL(&_engine_mux);
        if (_shared.pending_change) {
            new_song = _shared.song;
            new_song_len = _shared.song_len;
            _shared.pending_change = false;
            song_changed = true;
        }
        if (_shared.pending_fx) {
            new_fx_freq = _shared.fx_freq;
            new_fx_ms = _shared.fx_ms;
            _shared.pending_fx = false;
            fx_queued = true;
        }
        portEXIT_CRITICAL(&_engine_mux);

        if (song_changed) {
            song = new_song;
            song_len = new_song_len;
            note_idx = 0;
            samples_into_note = 0;
            note_loaded = false;
        }
        if (fx_queued) {
            if (new_fx_freq > 0 && new_fx_ms > 0) {
                fx_hp = C28P_AUDIO_SAMPLE_RATE / (2 * new_fx_freq);
                fx_phase = 0;
                fx_hi = true;
                fx_remaining = (C28P_AUDIO_SAMPLE_RATE * new_fx_ms) / 1000;
            } else {
                fx_remaining = 0;   // cancel
            }
        }

        // ── Load next note if needed ──
        if (!note_loaded && song != NULL && song_len > 0) {
            const c28p_note_t* n = &song[note_idx];
            total_samples_in_note = (C28P_AUDIO_SAMPLE_RATE * n->ms) / 1000;
            if (total_samples_in_note < 1) total_samples_in_note = 1;
            sound_samples = (total_samples_in_note * 85) / 100;
            hp_mel  = (n->mel  > 0) ? (C28P_AUDIO_SAMPLE_RATE / (2 * n->mel))  : 0;
            hp_bass = (n->bass > 0) ? (C28P_AUDIO_SAMPLE_RATE / (2 * n->bass)) : 0;
            phase_mel = phase_bass = 0;
            hi_mel = hi_bass = true;
            samples_into_note = 0;
            note_loaded = true;
        }

        // ── Synthesize one DMA chunk ──
        for (int i = 0; i < CHUNK; i++) {
            int32_t s = 0;
            const bool in_sound_window = note_loaded &&
                                          (samples_into_note < sound_samples);

            if (in_sound_window && hp_mel > 0) {
                s += hi_mel ? VOICE : -VOICE;
                if (++phase_mel >= hp_mel) { phase_mel = 0; hi_mel = !hi_mel; }
            }
            // BASS VOICE INTENTIONALLY DROPPED — see note above the
            // VOICE amplitude definition. The phase trackers below
            // are kept silent (no sample contribution) to preserve
            // the data layout for future polyphony work.
            (void)hp_bass; (void)phase_bass; (void)hi_bass;
            if (fx_remaining > 0 && fx_hp > 0) {
                s += fx_hi ? VOICE : -VOICE;
                if (++fx_phase >= fx_hp) { fx_phase = 0; fx_hi = !fx_hi; }
                fx_remaining--;
            }

            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            buf[i * 2]     = (int16_t)s;
            buf[i * 2 + 1] = (int16_t)s;

            if (note_loaded) {
                samples_into_note++;
                if (samples_into_note >= total_samples_in_note) {
                    note_idx = (note_idx + 1) % song_len;
                    note_loaded = false;
                }
            }
        }

        // ── Push to I2S — blocks until DMA buffer drains, pacing the task ──
        size_t written;
        esp_err_t err = i2s_write(I2S_NUM_0, buf, sizeof(buf),
                                  &written, portMAX_DELAY);
        if (err != ESP_OK) {
            Serial.printf("[C28P-AUDIO] engine i2s_write err=%d — exiting\n", err);
            break;
        }
    }

    _music_task_handle = NULL;
    vTaskDelete(NULL);
}

extern "C" {

bool c28p_audio_music_start(void) {
    if (_music_task_running) return true;

    if (!c28p_audio_enter(C28P_AUDIO_PLAYBACK_TONE)) {
        Serial.println("[C28P-AUDIO] Music engine: PLAYBACK_TONE entry failed");
        return false;
    }

    portENTER_CRITICAL(&_engine_mux);
    _shared.song = NULL;
    _shared.song_len = 0;
    _shared.pending_change = false;
    _shared.fx_freq = 0;
    _shared.fx_ms = 0;
    _shared.pending_fx = false;
    portEXIT_CRITICAL(&_engine_mux);

    _music_task_running = true;
    BaseType_t r = xTaskCreatePinnedToCore(
        _music_task, "pm_music_engine", 4096, NULL, 5,
        &_music_task_handle, 0);

    if (r != pdPASS) {
        _music_task_running = false;
        Serial.println("[C28P-AUDIO] Music engine task spawn failed");
        return false;
    }
    Serial.println("[C28P-AUDIO] Music engine started on Core 0");
    return true;
}

void c28p_audio_music_stop(void) {
    if (!_music_task_running) return;
    _music_task_running = false;
    // Wait briefly for task to self-clean (it exits after the current
    // i2s_write, which takes ~4ms at the chosen chunk size).
    for (int i = 0; i < 50 && _music_task_handle != NULL; i++) {
        delay(10);
    }
    c28p_audio_release();
    Serial.println("[C28P-AUDIO] Music engine stopped");
}

void c28p_audio_music_set_song(const c28p_note_t* notes, uint16_t count) {
    portENTER_CRITICAL(&_engine_mux);
    _shared.song = notes;
    _shared.song_len = count;
    _shared.pending_change = true;
    portEXIT_CRITICAL(&_engine_mux);
}

void c28p_audio_music_fx(uint16_t freq, uint16_t ms) {
    portENTER_CRITICAL(&_engine_mux);
    _shared.fx_freq = freq;
    _shared.fx_ms = ms;
    _shared.pending_fx = true;
    portEXIT_CRITICAL(&_engine_mux);
}

bool c28p_audio_music_is_running(void) {
    return _music_task_running;
}

}  // extern "C" (music engine)

#endif  // DEVICE_C28P

