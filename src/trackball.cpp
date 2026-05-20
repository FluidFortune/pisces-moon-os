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

#include "trackball.h"
#include <Arduino.h>

// ============================================================
//  TRACKBALL INPUT
//
//  T-Deck Plus  : real 5-pin trackball (up/down/left/right/click)
//                 on GPIOs defined in trackball.h. Edge-detect
//                 state machine + lockouts so one physical roll =
//                 exactly one event.
//
//  T-LoraPager  : NO physical trackball. We synthesize the same
//                 TrackballState from the rotary encoder only:
//                   - rotary encoder rotate = down/up
//                   - rotary encoder short press = click
//
//                 Keyboard input is intentionally NOT read here. The
//                 TCA8418 exposes a destructive FIFO: reading one event
//                 consumes it. If trackball polling also reads keyboard
//                 events, apps lose characters and exit keys before their
//                 own get_keypress() calls can see them.
//
//                 RESERVED FOR EMULATOR GAMES (future):
//                   - O = A button (jump/confirm in SMB)
//                   - K = B button (run/fireball in SMB)
//                   - C = SELECT button
//                   - V = START button
//                 (Games will read these directly, bypassing trackball synth)
//
//                 The launcher and apps see the same struct — no
//                 changes needed in handleTrackball().
//
//  Both paths expose the same interface.
// ============================================================


#ifdef DEVICE_TDECK_PLUS
// ============================================================
//  T-DECK PLUS — physical trackball
// ============================================================

// ─────────────────────────────────────────────
//  EDGE-DETECT STATE
//  Each pin has an "armed" flag. A movement event
//  only fires on the transition: HIGH → LOW.
//  The pin must return HIGH before it can fire again.
// ─────────────────────────────────────────────
static bool armed_up    = true;
static bool armed_down  = true;
static bool armed_left  = true;
static bool armed_right = true;
static bool armed_click = true;

static unsigned long last_move_time = 0;
const  unsigned long MOVE_LOCKOUT_MS = 250;

static unsigned long last_game_move_time = 0;
const  unsigned long GAME_LOCKOUT_MS = 80;

static unsigned long last_click_time = 0;
const  unsigned long CLICK_LOCKOUT_MS = 600;

void init_trackball() {
    pinMode(TRK_UP,    INPUT_PULLUP);
    pinMode(TRK_DOWN,  INPUT_PULLUP);
    pinMode(TRK_LEFT,  INPUT_PULLUP);
    pinMode(TRK_RIGHT, INPUT_PULLUP);
    pinMode(TRK_CLICK, INPUT_PULLUP);

    delay(100);
    armed_up    = (digitalRead(TRK_UP)    == HIGH);
    armed_down  = (digitalRead(TRK_DOWN)  == HIGH);
    armed_left  = (digitalRead(TRK_LEFT)  == HIGH);
    armed_right = (digitalRead(TRK_RIGHT) == HIGH);
    armed_click = (digitalRead(TRK_CLICK) == HIGH);

    last_move_time  = millis();
    last_click_time = millis();
}

TrackballState update_trackball() {
    TrackballState state = {0, 0, false};
    unsigned long now = millis();

    if (digitalRead(TRK_UP)    == HIGH) armed_up    = true;
    if (digitalRead(TRK_DOWN)  == HIGH) armed_down  = true;
    if (digitalRead(TRK_LEFT)  == HIGH) armed_left  = true;
    if (digitalRead(TRK_RIGHT) == HIGH) armed_right = true;
    if (digitalRead(TRK_CLICK) == HIGH) armed_click = true;

    if (now - last_move_time >= MOVE_LOCKOUT_MS) {
        if (armed_up && digitalRead(TRK_UP) == LOW) {
            state.y      = -1;
            armed_up     = false;
            last_move_time = now;
        } else if (armed_down && digitalRead(TRK_DOWN) == LOW) {
            state.y      = 1;
            armed_down   = false;
            last_move_time = now;
        } else if (armed_left && digitalRead(TRK_LEFT) == LOW) {
            state.x      = -1;
            armed_left   = false;
            last_move_time = now;
        } else if (armed_right && digitalRead(TRK_RIGHT) == LOW) {
            state.x      = 1;
            armed_right  = false;
            last_move_time = now;
        }
    }

    if (armed_click &&
        now - last_click_time >= CLICK_LOCKOUT_MS &&
        digitalRead(TRK_CLICK) == LOW) {
        state.clicked  = true;
        armed_click    = false;
        last_click_time = now;
    }
    return state;
}

TrackballState update_trackball_game() {
    TrackballState state = {0, 0, false};
    unsigned long now = millis();

    if (digitalRead(TRK_UP)    == HIGH) armed_up    = true;
    if (digitalRead(TRK_DOWN)  == HIGH) armed_down  = true;
    if (digitalRead(TRK_LEFT)  == HIGH) armed_left  = true;
    if (digitalRead(TRK_RIGHT) == HIGH) armed_right = true;
    if (digitalRead(TRK_CLICK) == HIGH) armed_click = true;

    if (now - last_game_move_time >= GAME_LOCKOUT_MS) {
        if (armed_up && digitalRead(TRK_UP) == LOW) {
            state.y = -1; armed_up = false; last_game_move_time = now;
        } else if (armed_down && digitalRead(TRK_DOWN) == LOW) {
            state.y = 1; armed_down = false; last_game_move_time = now;
        } else if (armed_left && digitalRead(TRK_LEFT) == LOW) {
            state.x = -1; armed_left = false; last_game_move_time = now;
        } else if (armed_right && digitalRead(TRK_RIGHT) == LOW) {
            state.x = 1; armed_right = false; last_game_move_time = now;
        }
    }

    if (armed_click && digitalRead(TRK_CLICK) == LOW) {
        state.clicked = true; armed_click = false;
    }
    return state;
}

#endif // DEVICE_TDECK_PLUS


#ifdef DEVICE_TLORAPAGER
// ============================================================
//  T-LORA PAGER — synthesized trackball from encoder
// ============================================================

// Rotary encoder pins (declared in main.cpp's pinmap).
#define ENC_A_PIN   40
#define ENC_B_PIN   41
#define ENC_BTN_PIN 7

// Quadrature decode state for the rotary.
// Two GPIOs A and B form a 2-bit gray code:
//   00 -> 01 -> 11 -> 10 -> 00  (one detent CW)
//   00 -> 10 -> 11 -> 01 -> 00  (one detent CCW)
//
// We track the last gray-code and accumulate net direction;
// when a full detent transition completes we emit one event.
static uint8_t  enc_last_gray   = 0;
static int8_t   enc_accumulator = 0;
static bool     enc_btn_armed   = true;
static unsigned long enc_last_btn_time = 0;
const unsigned long ENC_BTN_LOCKOUT_MS = 400;

// Move lockout — prevents rapid-fire when key is held.
static unsigned long last_move_time = 0;
const  unsigned long MOVE_LOCKOUT_MS = 250;
static unsigned long last_game_move_time = 0;
const  unsigned long GAME_LOCKOUT_MS = 80;

void init_trackball() {
    pinMode(ENC_A_PIN,   INPUT_PULLUP);
    pinMode(ENC_B_PIN,   INPUT_PULLUP);
    pinMode(ENC_BTN_PIN, INPUT_PULLUP);

    delay(20);
    uint8_t a = digitalRead(ENC_A_PIN);
    uint8_t b = digitalRead(ENC_B_PIN);
    enc_last_gray = (a << 1) | b;
    enc_accumulator = 0;

    last_move_time = millis();
    enc_last_btn_time = millis();
    Serial.println("[TRACKBALL] T-LoraPager encoder ready");
}

// ── Encoder polling ───────────────────────────────────────
// Returns: 0 = no change, +1 = one detent CW, -1 = one detent CCW.
static int poll_encoder_step() {
    uint8_t a = digitalRead(ENC_A_PIN);
    uint8_t b = digitalRead(ENC_B_PIN);
    uint8_t gray = (a << 1) | b;
    if (gray == enc_last_gray) return 0;

    // Standard 4-state-per-detent transition table.
    //   prev | curr | dir
    //   00      01    +1
    //   01      11    +1
    //   11      10    +1
    //   10      00    +1
    // Reverse direction = -1.
    int8_t dir = 0;
    switch ((enc_last_gray << 2) | gray) {
        case 0b0001: case 0b0111: case 0b1110: case 0b1000: dir = +1; break;
        case 0b0010: case 0b1011: case 0b1101: case 0b0100: dir = -1; break;
        default: dir = 0; break;  // invalid transition (bounce)
    }
    enc_last_gray = gray;
    enc_accumulator += dir;

    // Emit one event per 4 quadrature steps (one full detent).
    if (enc_accumulator >= 4) {
        enc_accumulator -= 4;
        return +1;
    }
    if (enc_accumulator <= -4) {
        enc_accumulator += 4;
        return -1;
    }
    return 0;
}

// ── Encoder button (short press) ──────────────────────────
static bool poll_encoder_click() {
    unsigned long now = millis();
    if (digitalRead(ENC_BTN_PIN) == HIGH) {
        enc_btn_armed = true;
        return false;
    }
    if (enc_btn_armed && (now - enc_last_btn_time >= ENC_BTN_LOCKOUT_MS)) {
        enc_btn_armed = false;
        enc_last_btn_time = now;
        return true;
    }
    return false;
}

TrackballState update_trackball() {
    TrackballState state = {0, 0, false};
    unsigned long now = millis();

    // ── 1. Rotary encoder rotation → up/down ──────────────
    int step = poll_encoder_step();
    if (step != 0 && now - last_move_time >= MOVE_LOCKOUT_MS) {
        state.y = (step > 0) ? +1 : -1;
        last_move_time = now;
        return state;
    }

    // ── 2. Encoder button short press → click ─────────────
    if (poll_encoder_click()) {
        state.clicked = true;
        return state;
    }

    return state;
}

TrackballState update_trackball_game() {
    // Game-mode: tighter lockout for real-time response.
    TrackballState state = {0, 0, false};
    unsigned long now = millis();

    if (now - last_game_move_time >= GAME_LOCKOUT_MS) {
        int step = poll_encoder_step();
        if (step != 0) {
            state.y = (step > 0) ? +1 : -1;
            last_game_move_time = now;
            return state;
        }
    }

    if (poll_encoder_click()) {
        state.clicked = true;
    }
    return state;
}

#endif // DEVICE_TLORAPAGER


#ifdef DEVICE_CARDPUTER_ADV
// ============================================================
//  CARDPUTER ADV - no physical trackball
// ============================================================
//
// Keep these as no-op providers so shared apps that were written
// against the T-Deck/Pager navigation API can still link. Cardputer
// launcher/game navigation reads the keyboard directly; polling the
// keyboard here would consume characters before apps can see them.

void init_trackball() {
    Serial.println("[TRACKBALL] Cardputer ADV no trackball");
}

TrackballState update_trackball() {
    return TrackballState{0, 0, false};
}

TrackballState update_trackball_game() {
    return TrackballState{0, 0, false};
}

#endif // DEVICE_CARDPUTER_ADV
