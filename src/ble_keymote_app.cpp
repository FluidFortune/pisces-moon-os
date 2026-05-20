// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

// ============================================================
//  ble_keymote_app.cpp — Launcher entry point
//
//  Matches the run_*() pattern used by every Pisces Moon app.
//  The launcher calls run_ble_keymote() when the user taps
//  the KEYMOTE tile. Loop returns to launcher when the user
//  taps the header.
// ============================================================

#include <Arduino.h>
#include "ble_keymote.h"

void run_ble_keymote() {
    keymoteEnter();
    while (true) {
        if (keymoteLoopOnce()) break;
        delay(5);
        yield();
    }
    keymoteExit();
}