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

#ifndef WIFI_DUCKY_H
#define WIFI_DUCKY_H

/**
 * PISCES MOON OS — WIFI DUCKY v1.0
 * Network-based payload delivery and execution for authorized testing.
 *
 * Capabilities:
 *   HTTP POST   — deliver payload to a running HTTP listener on target
 *   HTTP GET    — trigger execution of a pre-staged script on target
 *   SSH EXEC    — execute a single command on target via SSH (requires LibSSH-ESP32)
 *   FILE DROP   — write file to a writable SMB/WebDAV share on target
 *   REVERSE CMD — act as a simple HTTP command server: target polls and executes
 *
 * Target configs stored in /payloads/wifi_targets.json
 * Payload files in /payloads/*.txt (same DuckyScript for SSH EXEC command sequences)
 *
 * USE ONLY ON SYSTEMS YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 * Requires WiFi connection — connect via COMMS > WIFI JOIN first.
 */

void run_wifi_ducky();

#endif
