// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  maxine_apps.h — Public entry points for Maxine kiosk apps
//
//  These are the Maxine (480x800 portrait) equivalents of the
//  C28P touch apps. Same underlying engines (NoSQL, weather
//  fetch, RSS parser, wardrive task, anomaly detector) — only
//  the render + touch UI is per-device.
//
//  Wiring: maxine_boot.cpp's launcher dispatches to these.
//  Touch input is read via maxine_touch_read() (defined in
//  maxine_boot.cpp).
// ─────────────────────────────────────────────

#ifndef MAXINE_APPS_H
#define MAXINE_APPS_H

#ifdef DEVICE_MAXINE

// ─── Info / system ───
void maxine_run_about();
void maxine_run_system();
void maxine_run_wifi_setup();

// ─── Reference reader (survival / medical / history) ───
// category is the NoSQL category name; display_name is the title.
void maxine_run_data_reader(const char* category, const char* display_name);

// ─── Network-using apps (lazy WiFi-on-demand) ───
void maxine_run_weather();
void maxine_run_rss();

// ─── Security tools ───
void maxine_run_wardrive();

#endif // DEVICE_MAXINE

#endif // MAXINE_APPS_H
