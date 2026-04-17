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

#ifndef DATA_READER_H
#define DATA_READER_H

#include <Arduino.h>

// Generic offline JSON reference reader.
// Pass the SD folder name and a display label.
//
// Examples:
//   run_data_reader("gemini",   "GEMINI LOG");
//   run_data_reader("medical",  "MEDICAL REF");
//   run_data_reader("survival", "SURVIVAL");
//   run_data_reader("baseball", "BASEBALL");
//   run_data_reader("history",  "AM. HISTORY");

void run_data_reader(const char* category, const char* display_name);

#endif