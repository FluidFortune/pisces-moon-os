// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_calculator.h — Touch calculator for C28P + Maxine
//
//  Four-function calculator with percent and sign toggle. No
//  parenthesis / scientific functions in this version — kiosk
//  use case is "quick math at the desk", not engineering.
//
//  The keyboard-equipped devices (T-Deck Plus, Pager, Cardputer)
//  use src/calculator.cpp instead; this file is touch-only.
// ─────────────────────────────────────────────

#pragma once

void pm_run_calculator();
