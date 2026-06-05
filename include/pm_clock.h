// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_clock.h — Clock / Timer / Stopwatch app
//
//  Three tabs in one app:
//    CLOCK     — wall clock (NTP-synced on WiFi connect)
//    TIMER     — countdown timer, tap +/- to set, START/PAUSE/RESET
//    STOPWATCH — count up, START/PAUSE/LAP, no SD persistence
//
//  The C28P has no RTC chip, so the wall clock drifts on each
//  reboot. On WiFi connect we NTP-sync; otherwise the clock
//  shows a "no sync" indicator and runs off millis() since the
//  last sync. Stopwatch and timer use millis() directly — they
//  don't care about wall time.
// ─────────────────────────────────────────────

#pragma once

// Three entry points into the same multi-tab app. Each one sets
// the initial tab before entering the shared main loop, so a
// launcher item for TIMER or STOPWATCH lands the user directly
// on that tab instead of forcing them to swipe through the clock.
void pm_run_clock();       // CLOCK tab
void pm_run_timer();       // TIMER tab
void pm_run_stopwatch();   // STOPWATCH tab
