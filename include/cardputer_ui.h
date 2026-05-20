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

#ifndef CARDPUTER_UI_H
#define CARDPUTER_UI_H

#include <Arduino.h>
#include "theme.h"

static constexpr int CP_DISP_W = 240;
static constexpr int CP_DISP_H = 135;
static constexpr int CP_HDR_H  = 14;
static constexpr int CP_FTR_H  = 14;
static constexpr int CP_FTR_Y  = CP_DISP_H - CP_FTR_H;

static constexpr uint16_t CP_HDR_BG = C_DARK;
static constexpr uint16_t CP_HDR_FG = C_GREEN;
static constexpr uint16_t CP_BODY_BG = C_BLACK;
static constexpr uint16_t CP_FTR_BG = C_BLACK;
static constexpr uint16_t CP_FTR_FG = C_GREY;

#endif
