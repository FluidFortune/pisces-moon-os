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

#ifndef TRACKBALL_H
#define TRACKBALL_H

#include <Arduino.h>

// Silence any conflicting definitions from hal.h
#undef TRK_UP
#undef TRK_DOWN
#undef TRK_LEFT
#undef TRK_RIGHT
#undef TRK_CLICK

// ─────────────────────────────────────────────
//  T-DECK PLUS DEFINITIVE TRACKBALL PIN MAP
//  Source: LilyGO repository utilities.h
//
//  IMPORTANT: TRK_LEFT = GPIO1.
//  On the generic esp32-s3-devkitc-1 board target,
//  the Arduino framework may route Serial output to
//  GPIO1, which registers as a constant LEFT signal.
//  This is fixed in platformio.ini by adding:
//    -DARDUINO_USB_MODE=1
//    -DARDUINO_USB_CDC_ON_BOOT=1
//  which forces Serial to the native USB CDC interface.
// ─────────────────────────────────────────────
#define TRK_UP    3
#define TRK_DOWN  15   // Was incorrectly 14 — corrected to 15
#define TRK_LEFT  1    // GPIO1: ensure Serial is on USB CDC, not UART0
#define TRK_RIGHT 2
#define TRK_CLICK 0    // GPIO0: boot-strap pin, needs edge-detect debounce

struct TrackballState {
    int x;
    int y;
    bool clicked;
};

void init_trackball();
TrackballState update_trackball();
TrackballState update_trackball_game();  // 80ms lockout — use in game loops

#endif