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

#ifndef MESH_MESSENGER_H
#define MESH_MESSENGER_H

#include <Arduino.h>

// ─────────────────────────────────────────────
//  MESH MESSENGER
//  IRC-style multi-channel LoRa messenger using
//  Meshtastic-compatible packet format.
//
//  Interops with real Meshtastic nodes on the
//  default unencrypted channel (LongFast preset).
//
//  Channels map to Meshtastic channel indices 0-7.
//  Channel 0 = default "LongFast" broadcast channel.
// ─────────────────────────────────────────────

void run_mesh_messenger();

#endif