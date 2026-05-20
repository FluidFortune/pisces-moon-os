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

#ifndef PM_INPUT_H
#define PM_INPUT_H

// ─────────────────────────────────────────────────────────
//  Universal key codes
//
//  These are the values get_keypress() can return for
//  non-printable keys across every supported device. Values
//  are placed in the 0x80+ range so they cannot collide with
//  printable ASCII (0x20–0x7E) or with the standard control
//  characters (BACKSPACE=8, TAB=9, LF=10, CR=13, ESC=27).
//
//  Devices that have dedicated keys (T-Deck Plus has actual
//  arrow keys on its keyboard) emit these directly. Devices
//  that use modifier layers (Cardputer ADV's Fn+;/,/./ for
//  arrows) emit these after translation in keyboard.cpp.
//
//  Apps should reference these constants rather than the
//  numeric values to stay portable across devices.
// ─────────────────────────────────────────────────────────

// Control characters — standard ASCII values
#define PM_KEY_BACKSPACE  8
#define PM_KEY_TAB        9
#define PM_KEY_ENTER      13
#define PM_KEY_ESC        27

// Arrow keys (0x80+ range to avoid printable-ASCII collisions)
#define PM_KEY_UP         0x81
#define PM_KEY_DOWN       0x82
#define PM_KEY_LEFT       0x83
#define PM_KEY_RIGHT      0x84

// Navigation cluster
#define PM_KEY_PGUP       0x85
#define PM_KEY_PGDN       0x86
#define PM_KEY_HOME       0x87
#define PM_KEY_END        0x88

// Forward delete (distinct from BACKSPACE)
#define PM_KEY_DEL        0x89

// Function-layer marker — Cardputer's Fn key when held alone
// (rarely emitted; mostly a modifier seen via fn_held in keysState)
#define PM_KEY_FN         0x8A

// ─────────────────────────────────────────────────────────
//  Exit-key family
//
//  Apps test for "user wants out" via pm_is_exit_key() rather
//  than hardcoding 'q' or ESC. The set of exit keys is
//  per-device because Pager has no touch and uses 'M' for
//  modal toggle, while T-Deck reserves header taps for exit.
// ─────────────────────────────────────────────────────────

static inline bool pm_is_exit_key(char k) {
#ifdef DEVICE_TLORAPAGER
    return k == 'm' || k == 'M' || k == 'q' || k == 'Q' || k == 27;
#else
    return k == 'q' || k == 'Q';
#endif
}

#ifdef DEVICE_TLORAPAGER
#define PM_EXIT_COPY       "Q TO EXIT"
#define PM_EXIT_SHORT_COPY "Q EXIT"
#define PM_BACK_COPY       "Q BACK"
#define PM_STOP_COPY       "Q TO STOP"
#define PM_ABORT_COPY      "Q TO ABORT"
#define PM_TEXT_COPY       "CLK TYPE / Q EXIT"
#define PM_TEXT_ACTIVE_COPY "CLK DONE"
#else
#define PM_EXIT_COPY       "TAP HEADER TO EXIT"
#define PM_EXIT_SHORT_COPY "TAP HEADER EXIT"
#define PM_BACK_COPY       "TAP HEADER: BACK"
#define PM_STOP_COPY       "TAP HEADER TO STOP"
#define PM_ABORT_COPY      "TAP HEADER TO ABORT"
#define PM_TEXT_COPY       "TYPE"
#define PM_TEXT_ACTIVE_COPY "TYPE"
#endif

#endif // PM_INPUT_H
