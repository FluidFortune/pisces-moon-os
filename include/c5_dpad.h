// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c5_dpad.h — Virtual D-pad for NM-CYD-C5 touch input
//
//  Mirror of c28p_dpad.h with one hardware difference:
//
//    C28P: FT6336G capacitive — supports TWO simultaneous points.
//          The C28P dpad uses both points to allow jump-while-running
//          combos (direction held by one finger, A tapped with the other).
//
//    C5:   XPT2046 resistive  — supports ONE point at a time.
//          The C5 dpad still handles all the same buttons (D-pad,
//          A, B, SELECT) but only one at a time. Games that depend
//          on simultaneous direction+action (Mario's jump-while-
//          running, Galaga's fire-while-moving) will degrade to
//          sequential input on the C5. Tetris, Snake, Pac-Man,
//          Breakout, and turn-based games are unaffected.
//
//  Layout shares the same numeric constants as the C28P (240×320
//  portrait, 200px game viewport on top, 120px control area on
//  the bottom) so games can render with the same dimensions on
//  both devices.
//
//  Usage:
//    c5_dpad_render();          // call once at game start
//    PMNesInput input;
//    c5_dpad_poll(&input);      // populate input from current touch
//    // ... game logic reads input.up/down/left/right/a/b ...
//
//  The render() function only draws once. After that, c5_dpad
//  redraws press/release feedback on individual buttons as needed.
//  No full-screen repaint per frame.
// ─────────────────────────────────────────────

#ifndef C5_DPAD_H
#define C5_DPAD_H

#ifdef DEVICE_C5

#include "game_input.h"   // PMNesInput

// Game viewport dimensions — games should draw within these.
// Identical numeric values to C28P_GAME_VIEW_* so any tetris.cpp
// (or other game) layout we write for the C28P transplants cleanly
// to the C5 by adding `|| defined(DEVICE_C5)` to the existing
// C28P branch.
#define C5_GAME_VIEW_W   240
#define C5_GAME_VIEW_H   200
#define C5_GAME_VIEW_X   0
#define C5_GAME_VIEW_Y   0

// D-pad area below the game viewport.
#define C5_DPAD_AREA_Y   200
#define C5_DPAD_AREA_H   120

// Render the static D-pad chrome (arrows, A, B, SELECT buttons,
// dividing line). Call once when entering a game. Subsequent calls
// are safe but redundant — selective per-button redraw is handled
// by poll().
void c5_dpad_render();

// Poll the XPT2046 and OR the directional / action fields into
// `input`. Existing fields (key, trackball, etc.) are untouched.
//
// Returns true if any button is currently pressed.
bool c5_dpad_poll(PMNesInput* input);

// Read the underlying touch state without filling a PMNesInput.
bool c5_dpad_touched();

#endif // DEVICE_C5

#endif // C5_DPAD_H
