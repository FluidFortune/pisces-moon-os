// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef HAL_INPUT_H
#define HAL_INPUT_H

// ============================================================
//  hal_input.h — Unified input abstraction
//
//  Library-free. Direct GPIO for trackball and encoder.
//  Direct I2C for TCA8418 keyboard controller.
//  No LilyGoLib. No third-party input library.
//
//  Provides a single pm_input_read() that returns a
//  pm_input_event_t regardless of physical input device.
//
//  T-Deck Plus:   trackball (5 GPIOs) → nav events
//  T-LoraPager:   rotary encoder (3 GPIOs) → nav events
//                 TCA8418 keyboard (I2C 0x34) → char events
//  Cardputer ADV: keyboard (TBD) → char events
//
//  Launcher and all apps use pm_input_event_t exclusively.
//  No app reads GPIO directly for navigation.
// ============================================================

#include <Arduino.h>
#include <Wire.h>
#include "hal_pins.h"

// ── Event types ───────────────────────────────────────────
typedef enum {
    PM_INPUT_NONE  = 0,
    PM_INPUT_UP,
    PM_INPUT_DOWN,
    PM_INPUT_LEFT,
    PM_INPUT_RIGHT,
    PM_INPUT_CLICK,    // trackball click / encoder center push
    PM_INPUT_BACK,     // dedicated back / ESC
    PM_INPUT_CHAR,     // keyboard character
} pm_input_type_t;

typedef struct {
    pm_input_type_t type;
    char            c;   // valid when type == PM_INPUT_CHAR
} pm_input_event_t;


// ── T-DECK PLUS — trackball (5 GPIOs) ────────────────────
#ifdef DEVICE_TDECK_PLUS

static inline void pm_input_init() {
    pinMode(TRK_UP,    INPUT_PULLUP);
    pinMode(TRK_DOWN,  INPUT_PULLUP);
    pinMode(TRK_LEFT,  INPUT_PULLUP);
    pinMode(TRK_RIGHT, INPUT_PULLUP);
    pinMode(TRK_CLICK, INPUT_PULLUP);
    Serial.println("[INPUT] Trackball ready");
}

static inline pm_input_event_t pm_input_read() {
    pm_input_event_t evt = { PM_INPUT_NONE, 0 };
    if (!digitalRead(TRK_UP))    { evt.type = PM_INPUT_UP;    return evt; }
    if (!digitalRead(TRK_DOWN))  { evt.type = PM_INPUT_DOWN;  return evt; }
    if (!digitalRead(TRK_LEFT))  { evt.type = PM_INPUT_LEFT;  return evt; }
    if (!digitalRead(TRK_RIGHT)) { evt.type = PM_INPUT_RIGHT; return evt; }
    if (!digitalRead(TRK_CLICK)) { evt.type = PM_INPUT_CLICK; return evt; }
    return evt;
}

static inline int pm_input_get_char(char *c) {
    // T-Deck keyboard handled in keyboard.cpp
    // Returns character if available, -1 if not
    (void)c;
    return -1;
}

#endif // DEVICE_TDECK_PLUS


// ── T-LORA PAGER — rotary encoder + TCA8418 keyboard ─────
// Rotary encoder: direct GPIO polling on A/B/BTN pins
// Keyboard: TCA8418 key event controller via I2C at 0x34
//
// TCA8418 key event registers:
//   0x02 = KEY_LCK_EC  — key event count
//   0x04 = KEY_EVENT_A — first event in FIFO
//          bit 7: 1=press, 0=release
//          bits 6-0: key code (1-80 for matrix keys)
//
// TCA8418 key codes for T-LoraPager QWERTY layout:
// LilyGo's keyboard uses a standard matrix layout.
// Key codes map directly to ASCII for most printable chars.
// Special keys (shift, alt, enter, backspace) need mapping.
//
// Encoder state machine: tracks A/B transitions to
// determine direction without a library.
#ifdef DEVICE_TLORAPAGER

// ── TCA8418 register definitions ─────────────────────────
#define TCA8418_ADDR        I2C_ADDR_KEYBOARD  // 0x34
#define TCA8418_REG_CFG     0x01
#define TCA8418_REG_INT     0x02
#define TCA8418_REG_EC      0x03  // event count
#define TCA8418_REG_EVT     0x04  // event FIFO
#define TCA8418_CFG_AI      0x80  // auto-increment
#define TCA8418_CFG_GPI_IEN 0x02  // GPI interrupt enable
#define TCA8418_CFG_KE_IEN  0x01  // key event interrupt enable
#define TCA8418_INT_K_INT   0x01  // key interrupt flag

// TCA8418 keycode → ASCII mapping for T-LoraPager layout
// Based on LilyGo keyboard matrix documentation
// Rows 0-7, Cols 0-9 — 80 possible key positions
// Only populated positions listed
static const char _tca8418_keymap[80] = {
    //  0     1     2     3     4     5     6     7     8     9
        0,    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  'o', // row 0
        'p',  'a',  's',  'd',  'f',  'g',  'h',  'j',  'k',  'l', // row 1
        0,    'z',  'x',  'c',  'v',  'b',  'n',  'm',  0,    0,   // row 2
        ' ',  0,    0,    '\n', '\b', 0,    0,    0,    0,    0,   // row 3
        0,    0,    0,    0,    0,    0,    0,    0,    0,    0,   // row 4-7
        0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
        0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
        0,    0,    0,    0,    0,    0,    0,    0,    0,    0,
};

// Encoder state — file-scope static, not global
static volatile int8_t  _enc_delta = 0;
static volatile bool    _enc_btn   = false;
static uint8_t          _enc_last  = 0;

// TCA8418 init
static inline bool _tca8418_init() {
    // Configure TCA8418: enable key event FIFO interrupts
    Wire.beginTransmission(TCA8418_ADDR);
    Wire.write(TCA8418_REG_CFG);
    Wire.write(TCA8418_CFG_KE_IEN);  // key event interrupt
    if (Wire.endTransmission() != 0) {
        Serial.println("[INPUT] TCA8418 not found at 0x34");
        return false;
    }
    Serial.println("[INPUT] TCA8418 keyboard ready");
    return true;
}

// TCA8418 read one key event from FIFO
// Returns ASCII char or 0 if none / key release
static inline char _tca8418_read_key() {
    // Check event count register
    Wire.beginTransmission(TCA8418_ADDR);
    Wire.write(TCA8418_REG_EC);
    Wire.endTransmission(false);
    Wire.requestFrom(TCA8418_ADDR, 1);
    if (!Wire.available()) return 0;
    uint8_t count = Wire.read() & 0x0F;
    if (count == 0) return 0;

    // Read event from FIFO
    Wire.beginTransmission(TCA8418_ADDR);
    Wire.write(TCA8418_REG_EVT);
    Wire.endTransmission(false);
    Wire.requestFrom(TCA8418_ADDR, 1);
    if (!Wire.available()) return 0;
    uint8_t evt = Wire.read();

    bool pressed = (evt & 0x80) != 0;
    uint8_t code = evt & 0x7F;

    if (!pressed) return 0;  // ignore release events
    if (code == 0 || code > 79) return 0;

    return _tca8418_keymap[code - 1];
}

// Encoder init — direct GPIO
static inline void _encoder_init() {
    pinMode(ENCODER_A,   INPUT_PULLUP);
    pinMode(ENCODER_B,   INPUT_PULLUP);
    pinMode(ENCODER_BTN, INPUT_PULLUP);
    _enc_last = (digitalRead(ENCODER_A) << 1) |
                 digitalRead(ENCODER_B);
    Serial.println("[INPUT] Rotary encoder ready");
}

// Encoder poll — call frequently from main loop
// Updates _enc_delta: +1 = clockwise (DOWN), -1 = CCW (UP)
static inline void _encoder_poll() {
    uint8_t cur = (digitalRead(ENCODER_A) << 1) |
                   digitalRead(ENCODER_B);
    if (cur == _enc_last) return;

    // Gray code transition table
    static const int8_t _table[16] = {
        0, -1,  1,  0,
        1,  0,  0, -1,
       -1,  0,  0,  1,
        0,  1, -1,  0
    };
    _enc_delta += _table[(_enc_last << 2) | cur];
    _enc_last = cur;
}

// ── DRV2605 haptic — direct I2C ──────────────────────────
// Simple GO command for tactile click on key press.
// DRV2605 at 0x5A. Effect 1 = strong click.
static inline void _haptic_click() {
    Wire.beginTransmission(I2C_ADDR_HAPTIC);
    Wire.write(0x01);  // MODE register
    Wire.write(0x00);  // internal trigger
    Wire.endTransmission();
    Wire.beginTransmission(I2C_ADDR_HAPTIC);
    Wire.write(0x04);  // waveform sequencer
    Wire.write(14);    // effect 14: strong click
    Wire.endTransmission();
    Wire.beginTransmission(I2C_ADDR_HAPTIC);
    Wire.write(0x0C);  // GO register
    Wire.write(0x01);  // fire
    Wire.endTransmission();
}

// DRV2605 init — mode register setup
static inline bool _haptic_init() {
    // Wake from standby
    Wire.beginTransmission(I2C_ADDR_HAPTIC);
    Wire.write(0x01);  // MODE
    Wire.write(0x00);  // internal trigger, out of standby
    if (Wire.endTransmission() != 0) {
        Serial.println("[INPUT] DRV2605 not found at 0x5A");
        return false;
    }
    // Set ERM open loop mode
    Wire.beginTransmission(I2C_ADDR_HAPTIC);
    Wire.write(0x1A);  // feedback control
    Wire.write(0x36);  // ERM mode
    Wire.endTransmission();
    // Library selection: LRA = 0x20, ERM = register default
    Wire.beginTransmission(I2C_ADDR_HAPTIC);
    Wire.write(0x1D);  // control3
    Wire.write(0xA0);  // ERM open loop
    Wire.endTransmission();
    Serial.println("[INPUT] DRV2605 haptic ready");
    return true;
}

// ── Public API ────────────────────────────────────────────
static inline void pm_input_init() {
    _encoder_init();
    _tca8418_init();
    _haptic_init();
}

static inline pm_input_event_t pm_input_read() {
    pm_input_event_t evt = { PM_INPUT_NONE, 0 };

    // Poll encoder state
    _encoder_poll();

    // ── Encoder center button ─────────────────────────────
    if (!digitalRead(ENCODER_BTN)) {
        // Debounce: wait for release
        uint32_t t = millis();
        while (!digitalRead(ENCODER_BTN) &&
               millis() - t < 500) delay(1);
        evt.type = PM_INPUT_CLICK;
        _haptic_click();
        return evt;
    }

    // ── Encoder rotation ──────────────────────────────────
    if (_enc_delta >= 4) {
        _enc_delta -= 4;
        evt.type = PM_INPUT_DOWN;  // CW = scroll down
        return evt;
    }
    if (_enc_delta <= -4) {
        _enc_delta += 4;
        evt.type = PM_INPUT_UP;    // CCW = scroll up
        return evt;
    }

    // ── Keyboard character ────────────────────────────────
    char c = _tca8418_read_key();
    if (c) {
        evt.type = PM_INPUT_CHAR;
        evt.c    = c;
        if (c != '\n' && c != '\b') _haptic_click();
        return evt;
    }

    return evt;
}

static inline int pm_input_get_char(char *c) {
    char k = _tca8418_read_key();
    if (k) { *c = k; return 1; }
    return -1;
}

// Encoder sensitivity — 4 detents per event feels natural
// Adjust if the physical encoder has different detent count
#define ENCODER_DETENTS_PER_EVENT  4

#endif // DEVICE_TLORAPAGER


// ── CARDPUTER ADV ─────────────────────────────────────────
#ifdef DEVICE_CARDPUTER_ADV

static inline void pm_input_init() {
    // TODO: M5Stack Cardputer keyboard init
    Serial.println("[INPUT] Cardputer ADV input TBD");
}

static inline pm_input_event_t pm_input_read() {
    pm_input_event_t evt = { PM_INPUT_NONE, 0 };
    return evt;
}

static inline int pm_input_get_char(char *c) {
    (void)c;
    return -1;
}

#endif // DEVICE_CARDPUTER_ADV

#endif // HAL_INPUT_H
