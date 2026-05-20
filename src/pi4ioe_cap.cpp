// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

// ============================================================
//  pi4ioe_cap.cpp
//
//  Implementation of the PI4IOE5V6408 driver. See pi4ioe_cap.h
//  for design notes and references.
// ============================================================

#include "pi4ioe_cap.h"

// ─────────────────────────────────────────────
//  Register addresses (from datasheet section 7.x)
// ─────────────────────────────────────────────
static constexpr uint8_t REG_RESET       = 0x01; // bit 0 = sw reset
static constexpr uint8_t REG_IO_DIR      = 0x03; // 1 = output, 0 = input
static constexpr uint8_t REG_OUT_STATE   = 0x05; // 1 = high,   0 = low
static constexpr uint8_t REG_OUT_HIGHZ   = 0x07; // 1 = hi-Z,   0 = drive
static constexpr uint8_t REG_INP_DEFAULT = 0x09;
static constexpr uint8_t REG_PUP_ENABLE  = 0x0B;
static constexpr uint8_t REG_PUP_SELECT  = 0x0D;
static constexpr uint8_t REG_INP_STATUS  = 0x0F;
static constexpr uint8_t REG_IRQ_MASK    = 0x11;
static constexpr uint8_t REG_IRQ_STATUS  = 0x13;

// ─────────────────────────────────────────────
//  Module state
//
//  We maintain a shadow copy of three registers so we can do
//  read-modify-write bit operations without an actual I2C read
//  for every change. Datasheet confirms that registers reflect
//  written values, not "actual pin state," so a shadow is safe.
//
//  All shadows initialize to power-on defaults per datasheet:
//    direction = 0x00 (all inputs)
//    output    = 0x00 (all low)
//    hi-Z      = 0xFF (all hi-Z) — this is why writing direction
//                                  to 1 alone isn't enough; we
//                                  must also clear hi-Z.
// ─────────────────────────────────────────────
static bool    s_present       = false;
static uint8_t s_shadow_dir    = 0x00;
static uint8_t s_shadow_output = 0x00;
static uint8_t s_shadow_highz  = 0xFF;

// ─────────────────────────────────────────────
//  Low-level I2C register write.
//  Returns true on ACK, false on NACK or bus error.
// ─────────────────────────────────────────────
static bool write_reg(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(PI4IOE_CAP_I2C_ADDR);
    Wire.write(reg);
    Wire.write(value);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Serial.printf("[PI4IOE] write_reg(0x%02X, 0x%02X) failed: I2C err=%u\n",
                      reg, value, err);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────
//  Low-level I2C register read (single byte).
//  Returns true on success; *value populated.
// ─────────────────────────────────────────────
static bool read_reg(uint8_t reg, uint8_t* value) {
    Wire.beginTransmission(PI4IOE_CAP_I2C_ADDR);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false); // repeated start
    if (err != 0) return false;

    if (Wire.requestFrom((int)PI4IOE_CAP_I2C_ADDR, (int)1) != 1) {
        return false;
    }
    *value = Wire.read();
    return true;
}

// ─────────────────────────────────────────────
//  Probe: do a zero-length write to 0x43 and check ACK.
//  This is the standard "is anyone home?" I2C scan pattern.
// ─────────────────────────────────────────────
bool pi4ioe_cap_probe() {
    Wire.beginTransmission(PI4IOE_CAP_I2C_ADDR);
    uint8_t err = Wire.endTransmission();
    return (err == 0);
}

// ─────────────────────────────────────────────
//  Init: detect the chip, then enable P0 as a driven high
//  output. This is the Cap LoRa-1262 RF switch enable sequence
//  matching M5Stack's reference code:
//     ioe.setDirection(0, true);      → P0 = output
//     ioe.setHighImpedance(0, false); → P0 not hi-Z
//     ioe.digitalWrite(0, true);      → P0 = HIGH
// ─────────────────────────────────────────────
bool pi4ioe_cap_init() {
    s_present = false;

    if (!pi4ioe_cap_probe()) {
        Serial.println("[PI4IOE] No device at 0x43 — assuming Cap LoRa-868 or no Cap");
        return false;
    }

    Serial.println("[PI4IOE] Detected PI4IOE5V6408 at 0x43 (Cap LoRa-1262)");

    // 1. Set P0 as output (bit 0 of REG_IO_DIR = 1)
    s_shadow_dir |= (1 << 0);
    if (!write_reg(REG_IO_DIR, s_shadow_dir)) {
        Serial.println("[PI4IOE] FAILED: write direction");
        return false;
    }

    // 2. Take P0 out of high-impedance (bit 0 of REG_OUT_HIGHZ = 0)
    //    Without this, the output FETs don't actually drive even
    //    though the pin is "configured" as output. This is the
    //    step every from-scratch implementation seems to miss.
    s_shadow_highz &= ~(1 << 0);
    if (!write_reg(REG_OUT_HIGHZ, s_shadow_highz)) {
        Serial.println("[PI4IOE] FAILED: write hi-Z");
        return false;
    }

    // 3. Drive P0 HIGH (bit 0 of REG_OUT_STATE = 1)
    //    This is what M5Stack's docs describe as "enable the RF
    //    antenna switch." Until this, the SX1262 RF path is open.
    s_shadow_output |= (1 << 0);
    if (!write_reg(REG_OUT_STATE, s_shadow_output)) {
        Serial.println("[PI4IOE] FAILED: write output state");
        return false;
    }

    // Optional verification: read back what we wrote and confirm.
    // Helpful during bring-up; can be removed later if needed.
    uint8_t verify = 0;
    if (read_reg(REG_OUT_STATE, &verify)) {
        Serial.printf("[PI4IOE] OUT_STATE readback: 0x%02X (expected 0x%02X)\n",
                      verify, s_shadow_output);
    }

    Serial.println("[PI4IOE] Cap LoRa-1262 RF switch enabled (P0=HIGH)");
    s_present = true;
    return true;
}

// ─────────────────────────────────────────────
//  Runtime bit-set on the output register.
//  Uses the shadow to preserve other bits.
// ─────────────────────────────────────────────
bool pi4ioe_cap_set_pin(uint8_t pin, bool high) {
    if (pin > 7) return false;
    if (!s_present) return false;

    uint8_t bit = (1 << pin);
    uint8_t new_output = s_shadow_output;
    if (high) {
        new_output |= bit;
    } else {
        new_output &= ~bit;
    }
    if (new_output == s_shadow_output) return true;   // no change

    if (!write_reg(REG_OUT_STATE, new_output)) {
        return false;
    }
    s_shadow_output = new_output;
    return true;
}

// ─────────────────────────────────────────────
//  Accessors
// ─────────────────────────────────────────────
uint8_t pi4ioe_cap_get_output_shadow() {
    return s_shadow_output;
}

bool pi4ioe_cap_present() {
    return s_present;
}