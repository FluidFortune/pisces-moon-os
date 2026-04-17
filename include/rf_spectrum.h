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

#ifndef RF_SPECTRUM_H
#define RF_SPECTRUM_H

/**
 * PISCES MOON OS — RF SPECTRUM VISUALIZER
 * SX1262 RSSI sweep across configurable frequency range.
 * Displays real-time scrolling waterfall + peak-hold bar chart.
 *
 * SPI Bus Treaty: wardrive SD logging pauses during spectrum sweep
 * (lora_voice_active flag is reused — prevents SD contention during
 * continuous SPI radio operations).
 */

void run_rf_spectrum();

#endif
