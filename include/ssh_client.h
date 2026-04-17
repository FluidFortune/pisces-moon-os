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

#ifndef SSH_CLIENT_H
#define SSH_CLIENT_H

/**
 * PISCES MOON OS — SSH CLIENT v1.0
 * Minimal SSH terminal for homelab access
 *
 * Requires lib_deps addition:
 *   https://github.com/ewpa/LibSSH-ESP32
 *
 * Features:
 *   - Connect to saved SSH hosts (stored in /ssh_hosts.json on SD)
 *   - Password and keyboard-interactive authentication
 *   - 320x240 terminal display (38 cols × 9 rows at size 1)
 *   - Full keyboard input via T-Deck QWERTY
 *   - Scrollable output history
 *   - Save new hosts to keyring
 *
 * SPI Bus Treaty:
 *   WiFi TCP/IP does not touch SPI bus — fully compliant.
 *   SD card access (host config save/load) uses hit-and-run pattern.
 *
 * Screen note:
 *   320x240 with monospace size-1 text = ~53 chars × ~14 lines.
 *   Functional for most shell work. Not a replacement for a real terminal.
 */

void run_ssh_client();

#endif