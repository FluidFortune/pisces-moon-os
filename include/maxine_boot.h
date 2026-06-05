// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  maxine_boot.h — First-boot bring-up for Maxine
//
//  Maxine (Sunton ESP32-8048S050C, 480x800 portrait, touch-only)
//  is the same software class as the C28P. These are the same
//  three entry points main.cpp calls for the C28P, renamed for
//  Maxine and implemented in maxine_boot.cpp scaled to 480x800.
//
//    maxine_setup()     — early hardware probe + GT911 touch init.
//                         Run after gfx is alive in main.cpp.
//    maxine_splash()    — portrait Pisces Moon splash (480x800).
//    maxine_launcher()  — touch category grid; never returns.
//
//  Implementations are gated by #ifdef DEVICE_MAXINE.
// ─────────────────────────────────────────────

#ifndef MAXINE_BOOT_H
#define MAXINE_BOOT_H

#ifdef DEVICE_MAXINE

void maxine_setup();
void maxine_splash();
void maxine_launcher();

// GT911 single-touch read, display coords (0..479, 0..799).
// Non-static so maxine_dpad.cpp can poll touch directly.
bool maxine_touch_read(int16_t* x, int16_t* y);

#endif // DEVICE_MAXINE

#endif // MAXINE_BOOT_H
