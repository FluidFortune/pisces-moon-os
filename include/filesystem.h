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

#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"

// The Bridge to the Matrix
extern SdFat sd;
extern Arduino_GFX *gfx;

// The new application entry point
void App_Filesystem_Explore();

#endif