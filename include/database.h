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

#ifndef DATABASE_H
#define DATABASE_H

#include <Arduino.h>

// Initializes the NoSQL Document Store
bool init_database();

// Appends messages to the isolated session file
bool save_chat_to_vault(const char* role, const char* message);

#endif