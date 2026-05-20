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

#ifndef WARDRIVE_H
#define WARDRIVE_H

#include <Arduino.h>

// ─────────────────────────────────────────────
//  WARDRIVE MODE
//
//  SCAN mode    — active WiFi probe/response cycles + BLE scanning.
//                 Good for wardrive mapping. Used in standalone T-Deck.
//                 Writes GPS-tagged CSV to SD card.
//
//  PROMISCUOUS  — true 802.11 monitor mode. Captures all management
//                 frames (beacons, probe-req, deauth, etc) at 10-100/sec.
//                 Good for security research / edge node use.
//                 No SD writes — streams to bridge host.
//
//  Modes are mutually exclusive. wardrive_set_mode() handles the
//  transition cleanly (stops the radio, switches state, restarts).
// ─────────────────────────────────────────────
typedef enum {
    WARDRIVE_MODE_SCAN        = 0,  // default — active scan cycles
    WARDRIVE_MODE_PROMISCUOUS = 1,  // monitor mode — all management frames
} wardrive_mode_t;

// Shared variables — read by launcher status bar and other apps
extern int  networks_found;         // count from last WiFi scan
extern volatile int  bt_found;      // count from current BLE window
extern int  esp_found;              // Espressif MAC hunter count
extern bool wardrive_active;
extern wardrive_mode_t wardrive_mode;           // current mode
extern volatile bool wardrive_promiscuous_active; // true when promisc running

// Session totals — accumulate across all scans, never reset until reboot.
extern int      networks_total;
extern int      ble_total;
extern uint32_t last_scan_ms;

// Traffic flags
extern volatile bool wifi_in_use;
extern volatile bool sd_in_use;

// Core functions
void init_wardrive_core();
void run_wardrive();
void wardrive_task(void* pvParameters);

// Clean shutdown of the wardrive task and all its resources.
// Called from wifi_mode.cpp when switching from SCANNER to CLIENT mode.
// Blocks until the task confirms exit. Returns false on timeout.
// After successful teardown, init_wardrive_core() can be called again
// to respawn the task.
bool wardrive_teardown(uint32_t timeout_ms = 3000);

// Mode selection — call from app task (Core 1), not from wardrive task.
// If wardrive is currently running, it will pick up the new mode on
// its next cycle. Safe to call while wardrive_active is true.
void wardrive_set_mode(wardrive_mode_t mode);

// Gamepad BLE handoff
void wardrive_ble_stop();
void wardrive_ble_resume();

// Session log filename
const char* wardrive_get_log_filename();

// ─── v1.1 — Bridge streaming ───────────────────────────────────────
extern volatile bool wardrive_bridge_streaming;
extern volatile bool wardrive_raw_log;  // When true: emit wifi_seen for EVERY observation
                                        // (not just new/updated). Enables PMStats.timeline()
                                        // and PMStats.persistence() in The Clinician.

#endif