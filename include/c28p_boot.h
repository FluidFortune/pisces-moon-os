// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_boot.h — First-boot bring-up for C28P
//
//  Three entry points main.cpp calls when DEVICE_C28P is defined:
//
//    c28p_setup()      — early hardware probe + I2C/SPI init.
//                        Run before pm_display_init().
//
//    c28p_splash()     — portrait-tuned Pisces Moon splash.
//                        Run after gfx is alive.
//
//    c28p_launcher()   — minimal 2x4 touch grid for first-boot
//                        validation. Returns never. App dispatch
//                        hooks in once boot is verified.
//
//  The implementations are entirely inside c28p_boot.cpp and
//  gated by #ifdef DEVICE_C28P, so this header is harmless on
//  other devices (the functions resolve to undefined symbols if
//  someone tries to call them — that's intentional, the build
//  fails loud rather than silently misbehaving).
// ─────────────────────────────────────────────

#ifndef C28P_BOOT_H
#define C28P_BOOT_H

#ifdef DEVICE_C28P

void c28p_setup();
void c28p_splash();
void c28p_launcher();

#endif // DEVICE_C28P

#endif // C28P_BOOT_H