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

#ifndef TOUCH_H
#define TOUCH_H

#include <Arduino.h>

// This struct is the "container" for touch info
struct TouchData {
    int16_t x;
    int16_t y;
    bool pressed;
};

// Prototypes - these MUST match the .cpp exactly
bool init_touch();
TouchData update_touch();
bool get_touch(int16_t *x, int16_t *y);

#endif