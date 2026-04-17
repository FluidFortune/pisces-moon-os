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

// Shared variables — read by launcher status bar and other apps
extern int  networks_found;
extern int  bt_found;
extern int  esp_found;       // Espressif MAC hunter count
extern bool wardrive_active; // gemini_client.cpp sets this false during OAuth

// Core functions
void init_wardrive_core();
void run_wardrive();
void wardrive_task(void* pvParameters);

// Gamepad BLE handoff — call before/after gamepad pairing
// to give the gamepad client exclusive NimBLE stack access.
void wardrive_ble_stop();
void wardrive_ble_resume();

// Returns the current session's log filename (e.g. "/wardrive_0003.csv").
// Empty string until wardrive_task has initialized.
// Useful for displaying the active file in the UI or other apps.
const char* wardrive_get_log_filename();

#endif
