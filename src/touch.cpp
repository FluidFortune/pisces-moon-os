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

#include "touch.h"
#include <Wire.h>

// ============================================================
//  TOUCH INPUT
//
//  T-Deck Plus  : GT911 capacitive touch controller at the
//                 pins documented below.
//  T-LoraPager  : NO touch hardware. All functions are no-ops
//                 returning safe defaults so shared code that
//                 calls get_touch() doesn't fail or hang.
//
//  CRITICAL: the T-Deck GT911 RST pin (GPIO 21) is the SD card
//  CS pin on T-LoraPager. If touch init runs on T-LoraPager it
//  pulses GPIO 21 LOW during boot, corrupting the SD card mount.
//  Hence the strict #ifdef gate below.
// ============================================================

#ifdef DEVICE_TDECK_PLUS
// ── T-DECK PLUS — GT911 capacitive touch ─────────────────
#include <TAMC_GT911.h>

#define TOUCH_SDA 18
#define TOUCH_SCL 8
#define TOUCH_INT 16
#define TOUCH_RST 21

TAMC_GT911 tp = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, 320, 240);

bool init_touch() {
    pinMode(TOUCH_INT, OUTPUT);
    pinMode(TOUCH_RST, OUTPUT);

    digitalWrite(TOUCH_RST, LOW);
    digitalWrite(TOUCH_INT, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(10);
    pinMode(TOUCH_INT, INPUT);
    delay(50);

    tp.begin();
    tp.setRotation(ROTATION_RIGHT);

    return true;
}

TouchData update_touch() {
    tp.read();
    TouchData data;
    data.pressed = tp.isTouched;
    if (data.pressed && tp.touches > 0) {
        data.x = tp.points[0].x;
        data.y = tp.points[0].y;
    } else {
        data.x = 0;
        data.y = 0;
    }
    return data;
}

bool get_touch(int16_t *x, int16_t *y) {
    tp.read();
    if (tp.isTouched && tp.touches > 0) {
        *x = tp.points[0].x;
        *y = tp.points[0].y;
        return true;
    }
    return false;
}

#endif // DEVICE_TDECK_PLUS


#ifdef DEVICE_TLORAPAGER
// ── T-LORA PAGER — no touch hardware, return no-op stubs ─
//
// The launcher and apps call get_touch() in their event loop.
// We return false unconditionally so the keyboard + encoder
// path (handleTrackball) gets all the input attention.
//
// init_touch() is a no-op. Touch_INT/RST pin numbers from
// T-Deck would clash with SD_CS (GPIO 21) on this device, so
// we explicitly DO NOT touch any GPIOs here.

bool init_touch() {
    // No hardware. Don't touch GPIOs.
    return false;
}

TouchData update_touch() {
    TouchData data;
    data.pressed = false;
    data.x = 0;
    data.y = 0;
    return data;
}

bool get_touch(int16_t *x, int16_t *y) {
    // Never pressed. Don't write *x or *y.
    return false;
}

#endif // DEVICE_TLORAPAGER


#ifdef DEVICE_CARDPUTER_ADV
// -- CARDPUTER ADV - no touch hardware, return no-op stubs --
//
// Shared apps call the touch API opportunistically. The Cardputer
// path must not initialize T-Deck GT911 pins or Pager-only GPIOs.

bool init_touch() {
    return false;
}

TouchData update_touch() {
    TouchData data;
    data.pressed = false;
    data.x = 0;
    data.y = 0;
    return data;
}

bool get_touch(int16_t *x, int16_t *y) {
    (void)x;
    (void)y;
    return false;
}

#endif // DEVICE_CARDPUTER_ADV
