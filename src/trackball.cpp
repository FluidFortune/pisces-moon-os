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

// ─────────────────────────────────────────────
//  EDGE-DETECT STATE
//  Each pin has an "armed" flag. A movement event
//  only fires on the transition: HIGH → LOW.
//  The pin must return HIGH before it can fire again.
//  This guarantees exactly one event per physical
//  roll click regardless of how long the loop takes
//  or how long the pin stays LOW.
// ─────────────────────────────────────────────
static bool armed_up    = true;
static bool armed_down  = true;
static bool armed_left  = true;
static bool armed_right = true;
static bool armed_click = true;

// Post-fire lockout: after a movement fires, ignore
// all pins for this many ms. Prevents a single roll
// from cascading across multiple loop iterations.
// Tune upward if still overshooting, downward if sluggish.
static unsigned long last_move_time = 0;
const  unsigned long MOVE_LOCKOUT_MS = 250;

// Game-mode lockout — lower threshold for real-time input.
// Games call update_trackball_game() instead of update_trackball().
static unsigned long last_game_move_time = 0;
const  unsigned long GAME_LOCKOUT_MS = 80;

// Click gets its own longer lockout — GPIO0 is noisy.
static unsigned long last_click_time = 0;
const  unsigned long CLICK_LOCKOUT_MS = 600;

void init_trackball() {
    pinMode(TRK_UP,    INPUT_PULLUP);
    pinMode(TRK_DOWN,  INPUT_PULLUP);
    pinMode(TRK_LEFT,  INPUT_PULLUP);
    pinMode(TRK_RIGHT, INPUT_PULLUP);
    pinMode(TRK_CLICK, INPUT_PULLUP);

    // Let GPIO0 settle before we start watching it.
    // Pre-arm all pins based on their current state.
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

    // ── Re-arm pins that have returned HIGH ──
    // Do this every call so we're always ready for the next edge.
    if (digitalRead(TRK_UP)    == HIGH) armed_up    = true;
    if (digitalRead(TRK_DOWN)  == HIGH) armed_down  = true;
    if (digitalRead(TRK_LEFT)  == HIGH) armed_left  = true;
    if (digitalRead(TRK_RIGHT) == HIGH) armed_right = true;
    if (digitalRead(TRK_CLICK) == HIGH) armed_click = true;

    // ── Directional pins — one event per edge, with post-fire lockout ──
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

    // ── Click — same edge-detect, separate lockout ──
    if (armed_click &&
        (now - last_click_time >= CLICK_LOCKOUT_MS) &&
        digitalRead(TRK_CLICK) == LOW) {

        state.clicked   = true;
        armed_click     = false;
        last_click_time = now;
        Serial.println("[TRACKBALL] Click");
    }

    return state;
}

// ─────────────────────────────────────────────
//  GAME-MODE TRACKBALL
//  Same logic as update_trackball() but with an
//  80ms lockout instead of 250ms. Use in game
//  loops where fast directional input matters.
//  Do NOT use in UI/launcher — too sensitive there.
// ─────────────────────────────────────────────
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
            state.y              = -1;
            armed_up             = false;
            last_game_move_time  = now;
        } else if (armed_down && digitalRead(TRK_DOWN) == LOW) {
            state.y              = 1;
            armed_down           = false;
            last_game_move_time  = now;
        } else if (armed_left && digitalRead(TRK_LEFT) == LOW) {
            state.x              = -1;
            armed_left           = false;
            last_game_move_time  = now;
        } else if (armed_right && digitalRead(TRK_RIGHT) == LOW) {
            state.x              = 1;
            armed_right          = false;
            last_game_move_time  = now;
        }
    }

    if (armed_click &&
        (now - last_click_time >= CLICK_LOCKOUT_MS) &&
        digitalRead(TRK_CLICK) == LOW) {
        state.clicked   = true;
        armed_click     = false;
        last_click_time = now;
    }

    return state;
}