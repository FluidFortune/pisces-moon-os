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

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

// Save credentials to the SD card Keyring
void save_wifi_config(const char* ssid, const char* password);

// Explicitly connect and save
bool connect_to_wifi(const char* ssid, const char* password);

// Read from SD and connect in the background (Runs on Boot)
void auto_connect_wifi();

// The new Keyring Lookup for fast reconnection
String get_known_password(String targetSSID);

#endif