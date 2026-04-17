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

#ifndef LORA_VOICE_H
#define LORA_VOICE_H

/**
 * PISCES MOON OS — LORA VOICE v1.0
 * Push-to-talk voice communication over LoRa using Codec2
 *
 * First LoRa voice implementation for the T-Deck Plus.
 * Based on ESP32_Codec2 library (github.com/meshtastic/ESP32_Codec2)
 * and the LilyGO T3-S3 MVSR walkie-talkie sketch architecture.
 *
 * Requires lib_deps addition:
 *   meshtastic/ESP32_Codec2 @ ^1.0.0
 *
 * SPI Bus Treaty:
 *   LoRa TX/RX uses SPI bus shared with SD card.
 *   During active voice session, SD card writes are suspended.
 *   A new Treaty clause: lora_voice_active flag prevents wardrive
 *   SD logging while voice is transmitting.
 *
 * Hardware constraint:
 *   GPIO0 (TRK_CLICK) shares pin with ES7210 microphone.
 *   Cannot use trackball click as PTT — use SPACE bar.
 *
 * Codec2 mode: 3200 bps (best quality for LoRa bandwidth)
 * Audio: 8kHz 16-bit mono (Codec2 requirement)
 * LoRa: 915MHz, FSK modulation, 44-byte encoded frames
 */

#include <Arduino.h>

// Global flag for SPI Bus Treaty compliance
// Set true during voice TX/RX, wardrive task checks this
extern volatile bool lora_voice_active;

void run_lora_voice();

#endif