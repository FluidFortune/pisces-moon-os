// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  tembed_input.h — T-Embed CC1101 encoder + buttons
//
//  The T-Embed has no touch and no keyboard: all navigation is the
//  rotary encoder (scroll) plus two buttons (the encoder push and the
//  side user key). These helpers decode the quadrature encoder and
//  debounce the buttons with direct GPIO reads, so apps don't each
//  reimplement it. Implemented in tembed_boot.cpp.
// ─────────────────────────────────────────────

#pragma once
#include <stdint.h>

#ifdef DEVICE_TEMBED_CC1101

// Call once after the board is powered (tembed_boot does this).
void tembed_input_init();

// Signed detents accumulated since the last call (+clockwise). Poll it
// in your loop; it never blocks.
int  tembed_encoder_delta();

// Rising-edge, debounced. Each press returns true exactly once.
bool tembed_btn_select();   // encoder push (GPIO0)
bool tembed_btn_back();     // side user key (GPIO6)

// Raw "is it held right now" state, for autofire and long-press. An app
// that reads these instead of the edge functions must wait for release
// and call tembed_input_flush() before returning, or the parent screen
// will see the tail of that press as a fresh edge.
bool tembed_btn_select_down();
bool tembed_btn_back_down();

// Discard pending button edges and encoder motion. Call on app entry and
// after any raw-state handling.
void tembed_input_flush();

#endif // DEVICE_TEMBED_CC1101
