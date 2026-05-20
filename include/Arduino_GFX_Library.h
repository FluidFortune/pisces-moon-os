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

#pragma once

#ifdef DEVICE_TLORAPAGER

#include "pm_disp_tlorapager.h"

using Arduino_GFX = PMDispTLoRaPager;

#ifndef RGB565_BLACK
#define RGB565_BLACK 0x0000
#endif
#ifndef RGB565_WHITE
#define RGB565_WHITE 0xFFFF
#endif
#ifndef RGB565_RED
#define RGB565_RED   0xF800
#endif
#ifndef RGB565_GREEN
#define RGB565_GREEN 0x07E0
#endif
#ifndef RGB565_BLUE
#define RGB565_BLUE  0x001F
#endif

#ifndef BLACK
#define BLACK RGB565_BLACK
#endif
#ifndef WHITE
#define WHITE RGB565_WHITE
#endif
#ifndef RED
#define RED   RGB565_RED
#endif
#ifndef GREEN
#define GREEN RGB565_GREEN
#endif
#ifndef BLUE
#define BLUE  RGB565_BLUE
#endif

#else
#include_next <Arduino_GFX_Library.h>
#endif
