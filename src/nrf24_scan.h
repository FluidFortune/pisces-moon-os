// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  nrf24_scan.h — 2.4 GHz channel-activity scanner (T-Embed nRF24)
//
//  A passive 2.4 GHz "poor-man's spectrum" view built on the nRF24's
//  RPD (received-power detector): for each of the 126 channels it drops
//  into RX, listens briefly, and reads whether any signal above about
//  -64 dBm was present. Sweeping all channels and plotting the hits
//  shows where the band is busy -- WiFi, Bluetooth, other 2.4 GHz gear.
//
//  Receive-only: it never transmits.
//
//  Based on LilyGO's factory nRF24 example (RadioLib nRF24), reworked
//  from their send/receive demo into a scanner and driven by the
//  T-Embed encoder. Runs on the shared SPI bus under the Bus Treaty.
// ─────────────────────────────────────────────

#pragma once

#ifdef DEVICE_TEMBED_CC1101

// Blocking app: owns the screen + encoder until the side key is pressed.
void nrf24_scan_run();

#endif // DEVICE_TEMBED_CC1101
