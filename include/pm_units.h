// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_units.h — Unit converter for C28P + Maxine
//
//  Measurement conversions only — no currency. Currency would
//  require live FX rates over the internet; measurements are
//  defined constants that don't change.
//
//  Categories:
//    LENGTH:      m, cm, mm, km, in, ft, yd, mi
//    WEIGHT:      kg, g, lb, oz
//    TEMPERATURE: C, F, K
//    VOLUME:      L, mL, gal (US), qt (US), pt (US), cup (US), fl oz (US)
// ─────────────────────────────────────────────

#pragma once

void pm_run_units();
