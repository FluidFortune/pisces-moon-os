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
//
// ─────────────────────────────────────────────────────────────────
//  wifi_mode.h — WiFi mode-locking (v1.2)
//
//  ARCHITECTURAL CONTEXT — Cardputer ADV
//
//  The M5Stack Cardputer ADV has 320KB of internal SRAM and no PSRAM.
//  WiFi STA mode costs ~52KB at init. NimBLE costs ~48KB at init.
//  Active app heap, file manager buffers, Gemini HTTPS context, etc.
//  collectively consume the rest.
//
//  This makes "WiFi-scanning apps" (wardrive, packet sniffer, beacon
//  spotter, probe intel, promiscuous, WPA handshake) and "WiFi-client
//  apps" (Gemini, Bridge HTTP, file manager web UI, WiFi connect, OTA)
//  mutually exclusive: you cannot run one mode's stack while the
//  other is loaded.
//
//  v1.2 enforces this constraint architecturally via g_wifi_mode.
//  The two modes are CLIENT (associated to AP) and SCANNER (passive
//  scan + monitor). Each app calls request_wifi_mode() before doing
//  anything WiFi-related. If the requested mode conflicts with the
//  current mode, the user is prompted to switch — and accepting the
//  prompt tears down the conflicting subsystem cleanly to recover
//  the memory.
//
//  T-Deck Plus and T-LoRa Pager have 8MB PSRAM and don't need this
//  constraint; g_wifi_mode tracking still happens but request_wifi_mode()
//  is permissive (always returns success without prompts).
// ─────────────────────────────────────────────────────────────────

#ifndef WIFI_MODE_H
#define WIFI_MODE_H

#include <Arduino.h>

typedef enum {
    WIFI_MODE_PM_OFF     = 0,  // Driver not initialized, no cost
    WIFI_MODE_PM_CLIENT  = 1,  // STA associated to AP — Gemini, Bridge, etc.
    WIFI_MODE_PM_SCANNER = 2,  // Scan-only — wardrive, packet, beacon, etc.
} pm_wifi_mode_t;

extern pm_wifi_mode_t g_wifi_mode;

// Friendly app name → string, used for the modal prompt.
// Pass the app name when requesting (e.g. "Gemini Chat", "Wardrive").
//
// Returns:
//   true  — the requested mode is now active, app may proceed.
//   false — the user declined the switch, or teardown failed. App
//           must abort its launch and return to launcher.
//
// Behavior:
//   - If current mode == OFF: initialize requested mode, return true.
//   - If current mode == requested: no-op, return true.
//   - If current mode != requested:
//       - Show modal "Switch WiFi mode? Current mode will be torn down."
//       - If user confirms: teardown current, init new, return true.
//       - If user declines: return false.
//   - On non-Cardputer devices: always permissive, returns true with
//     no prompts (PSRAM provides headroom for both modes concurrently).
bool request_wifi_mode(pm_wifi_mode_t mode, const char* app_name);

// Lower-level helpers — exposed for diagnostic apps only. Most apps
// should use request_wifi_mode() above.

// Tear down whatever WiFi state is currently active. After return,
// g_wifi_mode == WIFI_MODE_PM_OFF and WiFi driver is fully released.
// Calls wardrive_teardown() if currently in SCANNER mode.
// Calls WiFi.disconnect(true) + WiFi.mode(WIFI_OFF) if in CLIENT mode.
bool teardown_wifi_mode();

// Initialize WiFi in CLIENT mode and attempt to connect using saved
// credentials from SD (wifi_manager.cpp). Returns true if WiFi.begin()
// was called successfully — the actual association may still be in
// progress; caller should poll WiFi.status() to confirm.
//
// Precondition: g_wifi_mode == WIFI_MODE_PM_OFF.
// Postcondition on success: g_wifi_mode == WIFI_MODE_PM_CLIENT.
bool init_wifi_client();

// Initialize WiFi in SCANNER mode and spawn the wardrive task.
//
// Precondition: g_wifi_mode == WIFI_MODE_PM_OFF.
// Postcondition on success: g_wifi_mode == WIFI_MODE_PM_SCANNER.
bool init_wifi_scanner();

// String for debug logging — "OFF" / "CLIENT" / "SCANNER".
const char* pm_wifi_mode_str(pm_wifi_mode_t mode);

#endif // WIFI_MODE_H