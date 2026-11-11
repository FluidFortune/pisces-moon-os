// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  cc1101_analyzer.h — Sub-GHz CC1101 receive analyzer (T-Embed)
//
//  A RECEIVE-ONLY analyzer for the T-Embed CC1101's sub-GHz radio.
//  It confirms a transmitter is present, finds its band (315 / 390 /
//  433.92 / 868 MHz via the on-board antenna switch), shows signal
//  strength, captures raw OOK pulse bursts, and reports whether the
//  burst changes from press to press — i.e. whether the signal is a
//  fixed code or a rolling code.
//
//  It does not transmit and it does not clone or replay anything.
//  On a rolling-code device (e.g. Genie Intellicode) the "changes
//  each press" readout is the expected, correct result.
//
//  Runs on the T-Embed's shared SPI bus under the Bus Treaty.
// ─────────────────────────────────────────────

#pragma once

#ifdef DEVICE_TEMBED_CC1101

// Blocking app: owns the screen and encoder until the user backs out
// (side key), then returns to the caller (launcher).
void cc1101_analyzer_run();

// ── Survey hooks (receive-only RSSI sampling for Stationary Survey) ──
// The analyzer owns the one CC1101 driver object; other apps borrow it
// through these instead of building a second driver on the same chip.
// begin() puts the radio in plain RSSI receive; tune() moves to a band
// (and its antenna path); rssi() is one quick reading. end() idles it.
bool        cc1101_survey_begin();
int         cc1101_survey_bands();
const char* cc1101_survey_label(int band);
bool        cc1101_survey_tune(int band);
float       cc1101_survey_rssi();
void        cc1101_survey_end();

#endif // DEVICE_TEMBED_CC1101
