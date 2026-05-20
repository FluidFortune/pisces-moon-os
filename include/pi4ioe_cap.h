// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#pragma once

// ============================================================
//  pi4ioe_cap.h
//
//  Driver for the PI4IOE5V6408 8-bit I2C I/O expander used on
//  M5Stack's Cap LoRa-1262 expansion module for Cardputer-Adv.
//
//  WHAT THIS DRIVER DOES
//  ─────────────────────
//  The Cap LoRa-1262 routes the SX1262's RF signal through an
//  on-Cap antenna switch. That switch is gated by pin P0 of a
//  PI4IOE5V6408 I/O expander at I2C address 0x43. Until P0 is
//  driven HIGH, the radio is electrically isolated from the
//  antenna — RadioLib's radio.begin() may succeed (the SX1262
//  itself responds to SPI), but TX/RX produce nothing on-air.
//
//  This driver writes three registers on the expander:
//    1. I/O Direction (0x03) — set P0 as output
//    2. Hi-Z (0x07)          — disable hi-Z so P0 can drive
//    3. Output State (0x05)  — set P0 HIGH (enable RF switch)
//
//  The Cap LoRa-868 variant does NOT have this expander. If
//  pi4ioe_cap_init() returns false because nothing answered at
//  0x43, that's normal on the 868 variant — code can proceed
//  without it.
//
//  CHIP REFERENCE
//  ──────────────
//  Diodes Inc. PI4IOE5V6408
//  https://www.diodes.com/assets/Datasheets/PI4IOE5V6408.pdf
//
//  Register map (confirmed from datasheet + Cascoda SDK):
//    0x01  Software reset / control
//    0x03  I/O Direction   (1 = output, 0 = input)
//    0x05  Output State    (1 = high,   0 = low)
//    0x07  Output Hi-Z     (1 = hi-Z,   0 = drive)
//    0x09  Input default state
//    0x0B  Pull-up/down enable
//    0x0D  Pull-up/down select
//    0x0F  Input status
//    0x11  Interrupt mask
//    0x13  Interrupt status
//
//  TREATY NOTES
//  ────────────
//  All transactions are I2C (Wire), not SPI. This driver does
//  NOT participate in the SPI Bus Treaty. It can be called any
//  time after Wire.begin() has run.
// ============================================================

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>

// Default I2C address of the PI4IOE5V6408 on Cap LoRa-1262.
// ADDR pin tied to GND → 0x43. (ADDR pin tied high → 0x44, used
// on some boards with two expanders; not relevant for the Cap.)
#ifndef PI4IOE_CAP_I2C_ADDR
#define PI4IOE_CAP_I2C_ADDR  0x43
#endif

// ─────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────

// Probe for a PI4IOE5V6408 at the configured I2C address.
// Returns true if a device acknowledged (Cap LoRa-1262 detected).
// Returns false if no device responded (Cap LoRa-868 attached,
// no Cap attached, or wiring fault).
//
// Wire must be initialized before calling this.
bool pi4ioe_cap_probe();

// Initialize the Cap LoRa-1262's RF antenna switch by:
//   1. Probing for the expander at PI4IOE_CAP_I2C_ADDR
//   2. Configuring P0 as a driven output
//   3. Setting P0 HIGH to enable the RF switch
//
// Returns:
//   true  — expander found and configured successfully
//   false — no expander present (likely Cap LoRa-868) OR an I2C
//           write failed. Mesh Messenger should be tolerant of
//           false: on a Cap LoRa-868, LoRa works without this.
//
// Logs a serial message in either case so the boot log shows
// which Cap variant is attached.
bool pi4ioe_cap_init();

// Write a single bit on the expander. Useful for runtime use
// (e.g., putting the radio in low-power state by clearing P0).
// Returns true on I2C ACK, false otherwise.
bool pi4ioe_cap_set_pin(uint8_t pin, bool high);

// Read the cached output register state. Does NOT do an I2C
// read — returns the last-written value. Used to track state
// for read-modify-write operations on multiple pins.
uint8_t pi4ioe_cap_get_output_shadow();

// True if pi4ioe_cap_init() previously succeeded.
// Mesh Messenger and other code can use this to know whether
// the Cap LoRa-1262 is present (vs the 868 variant or no Cap).
bool pi4ioe_cap_present();