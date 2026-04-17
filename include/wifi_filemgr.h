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

#ifndef WIFI_FILEMGR_H
#define WIFI_FILEMGR_H

/**
 * PISCES MOON OS — WIFI FILE MANAGER
 * =====================================
 * HTTP file server over WiFi — browse, download, upload, and delete
 * files on the MicroSD card from any browser on the same network.
 *
 * Access: http://<device-ip>/ after launching from SYSTEM > SD SHARE.
 * No special software required on Mac, Windows, or Linux.
 *
 * Primary use case: deploy ELF files to /apps/ without removing the
 * MicroSD card. Drag .elf onto the browser upload UI, done.
 *
 * SPI Bus Treaty: all SD reads/writes take spi_mutex.
 * WiFi Treaty: sets wifi_in_use for duration of session.
 *              Wardrive WiFi scanning pauses automatically.
 */

void run_wifi_filemgr();

#endif // WIFI_FILEMGR_H