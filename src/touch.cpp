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

#include "touch.h"
#include <Wire.h>
#include <TAMC_GT911.h>

#define TOUCH_SDA 18
#define TOUCH_SCL 8
#define TOUCH_INT 16
#define TOUCH_RST 21

TAMC_GT911 tp = TAMC_GT911(TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, 320, 240);

bool init_touch() {
    pinMode(TOUCH_INT, OUTPUT);
    pinMode(TOUCH_RST, OUTPUT);

    digitalWrite(TOUCH_RST, LOW);
    digitalWrite(TOUCH_INT, LOW);
    delay(10);
    digitalWrite(TOUCH_RST, HIGH);
    delay(10);
    pinMode(TOUCH_INT, INPUT); 
    delay(50);

    tp.begin();
    tp.setRotation(ROTATION_RIGHT); 
    
    return true; 
}

TouchData update_touch() {
    tp.read();
    TouchData data;
    data.pressed = tp.isTouched;
    if (data.pressed && tp.touches > 0) {
        data.x = tp.points[0].x;
        data.y = tp.points[0].y;
    } else {
        data.x = 0; 
        data.y = 0;
    }
    return data;
}

bool get_touch(int16_t *x, int16_t *y) {
    tp.read();
    if (tp.isTouched && tp.touches > 0) {
        *x = tp.points[0].x;
        *y = tp.points[0].y;
        return true;
    }
    return false;
}