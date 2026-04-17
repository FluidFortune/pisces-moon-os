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

#include <Wire.h>
#include "keyboard.h"
#include "hal.h"

// Fallback definition in case hal.h version on disk is missing it
#ifndef KEYBOARD_ADDR
#define KEYBOARD_ADDR 0x55
#endif

void init_keyboard() {
    Wire.beginTransmission(KEYBOARD_ADDR);
    if (Wire.endTransmission() == 0) {
        Serial.println("KB: Online.");
    } else {
        Serial.println("KB: Sync Error.");
    }
}

char get_keypress() {
    Wire.requestFrom(KEYBOARD_ADDR, 1);
    if (Wire.available()) {
        uint8_t key = Wire.read();
        if (key == 0) return 0;
        
        // Standard ASCII Mapping
        if (key == 0x08) return 8;   // Backspace
        if (key == 0x0D) return 13;  // Enter
        if (key == 0x1B) return 27;  // ESC/SYM
        
        return (char)key;
    }
    return 0;
}