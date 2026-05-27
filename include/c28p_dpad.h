// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_dpad.h — Virtual D-pad for C28P touch input
//
//  The C28P has no keyboard and no trackball. Games that the
//  other Pisces Moon devices control with arrows + A/B keys
//  need a touchscreen-native input surface.
//
//  Layout (240x320 portrait):
//    Top 200px:  game viewport (game owns this region)
//    Bottom 120px: D-pad + action buttons
//      Left half:  ▲ ◄ ▼ ► D-pad cluster
//      Right half: [A] [B] action buttons
//
//  Usage from a game's main loop:
//    c28p_dpad_render();          // call once at game start
//    PMNesInput input;
//    c28p_dpad_poll(&input);      // populate input from current touches
//    // ... game logic reads input.up/down/left/right/a/b ...
//
//  The render() function only draws once. After that, c28p_dpad
//  draws press/release feedback on individual buttons as needed.
//  No full-screen repaint per frame — important on a 240MHz
//  ESP32-S3 that has to keep up with audio + game logic.
//
//  Hit zones are slightly larger than the rendered visuals to
//  forgive fat-finger taps near edges (target ~12mm per button
//  with ~3mm padding). Trade-off: a touch directly between two
//  D-pad arrows goes to whichever zone has higher y/x dominance,
//  not to "both" — diagonals are not currently a thing.
// ─────────────────────────────────────────────

#ifndef C28P_DPAD_H
#define C28P_DPAD_H

#ifdef DEVICE_C28P

#include "game_input.h"   // PMNesInput

// Game viewport dimensions — games should draw within these.
// Defined as compile-time constants so games can use them in
// their own static const definitions.
#define C28P_GAME_VIEW_W   240
#define C28P_GAME_VIEW_H   200
#define C28P_GAME_VIEW_X   0
#define C28P_GAME_VIEW_Y   0

// D-pad area below the game viewport.
#define C28P_DPAD_AREA_Y   200
#define C28P_DPAD_AREA_H   120

// Render the static D-pad chrome (arrows, buttons, dividing line).
// Call once when entering a game. Subsequent calls are safe but
// redundant.
void c28p_dpad_render();

// Poll the touch controller and populate the directional / action
// fields of `input`. Existing fields (key, trackball, etc.) are
// left untouched — this is meant to be called alongside
// pm_read_nes_input() to OR in touch state.
//
// Returns true if any button is currently pressed.
bool c28p_dpad_poll(PMNesInput* input);

// Read the underlying touch state without filling a PMNesInput.
// Useful when you just want "is anything being touched" without
// allocating an input struct.
bool c28p_dpad_touched();

#endif // DEVICE_C28P

#endif // C28P_DPAD_H