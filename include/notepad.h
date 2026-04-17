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

#ifndef NOTEPAD_H
#define NOTEPAD_H

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"

// The 'extern' keyword tells the app the hardware exists in main.cpp
extern SdFat sd;
extern Arduino_GFX *gfx;

// Declare our functions
void run_notepad();
void App_Notepad_Save(String filename, String content);

#endif