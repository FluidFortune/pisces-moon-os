// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef BLE_KEYMOTE_H
#define BLE_KEYMOTE_H

// ============================================================
//  ble_keymote.h — Pisces Moon as a BLE HID keyboard
//
//  Live BLE HID keyboard. Local hardware keyboard becomes a
//  wireless keyboard for any BLE-HID capable host.
//
//  Two modes:
//    LIVE   — every keypress sent immediately (default)
//    BUFFER — compose locally, send on Enter
//
//  Toggle with backtick. Exit by tapping the header.
// ============================================================

#include <stdint.h>

// Public state (read-only — for launcher footer indicator)
extern bool kmAdvertising;
extern bool kmConnected;

// Lifecycle — called by run_ble_keymote() wrapper
void keymoteEnter();
bool keymoteLoopOnce();  // returns true when user wants exit
void keymoteExit();

#endif