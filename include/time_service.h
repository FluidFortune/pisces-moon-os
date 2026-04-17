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

#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H
#include <Arduino.h>
#include "time.h"

inline String get_header_time() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return ""; // Return empty if no WiFi sync
    }
    char buf[10];
    strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    return String(buf);
}

#endif