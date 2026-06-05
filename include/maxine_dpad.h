// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  maxine_dpad.h — Virtual D-pad for Maxine touch input
//
//  Maxine (Sunton ESP32-8048S050C, 480x800 portrait) is the same
//  SOFTWARE class as the C28P: touch-only, no keyboard, no
//  trackball. This is the C28P virtual D-pad scaled up to the
//  larger panel. Identical behavior, larger geometry.
//
//  Layout (480x800 portrait):
//    Top 520px:    game viewport (game owns this region)
//    Bottom 280px: D-pad + action buttons
//      Left half:  ▲ ◄ ▼ ► D-pad cluster
//      Right half: [A] [B] action buttons
//
//  Usage from a game's main loop (identical to the C28P API):
//    maxine_dpad_render();        // call once at game start
//    PMNesInput input;
//    maxine_dpad_poll(&input);    // populate input from touches
//
//  As with the C28P dpad, render() draws once and poll() does
//  selective per-button redraw. No full-screen repaint per frame.
// ─────────────────────────────────────────────

#ifndef MAXINE_DPAD_H
#define MAXINE_DPAD_H

#ifdef DEVICE_MAXINE

#include "game_input.h"   // PMNesInput

// Game viewport dimensions — games should draw within these.
// Compile-time constants so games can use them in static const
// definitions (same contract as the C28P_GAME_VIEW_* macros).
#define MAXINE_GAME_VIEW_W   480
#define MAXINE_GAME_VIEW_H   520
#define MAXINE_GAME_VIEW_X   0
#define MAXINE_GAME_VIEW_Y   0

// D-pad area below the game viewport.
#define MAXINE_DPAD_AREA_Y   520
#define MAXINE_DPAD_AREA_H   280

// Render the static D-pad chrome (arrows, buttons, dividing line).
// Call once when entering a game.
void maxine_dpad_render();

// Poll the touch controller and OR the directional / action fields
// into `input`. Called alongside pm_read_nes_input().
// Returns true if any button is currently pressed.
bool maxine_dpad_poll(PMNesInput* input);

// Read raw touch state without filling a PMNesInput.
bool maxine_dpad_touched();

#endif // DEVICE_MAXINE

#endif // MAXINE_DPAD_H
