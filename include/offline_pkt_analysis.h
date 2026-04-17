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

#ifndef OFFLINE_PKT_ANALYSIS_H
#define OFFLINE_PKT_ANALYSIS_H

/**
 * PISCES MOON OS — OFFLINE PACKET ANALYSIS
 * Rules-based post-session analysis of beacon spotter and packet sniffer logs.
 *
 * Reads JSON/CSV session files from /cyber_logs/ on SD card.
 * No real-time capture — works entirely on saved session data.
 * SPI Bus Treaty compliant: SD read only, no WiFi required.
 *
 * Detection rules:
 *   DEAUTH FLOOD    — same source sent >threshold deauths in session
 *   EVIL TWIN       — same SSID seen with different BSSID + incompatible channel
 *   ENCRYPTION DWGR — same SSID seen as WPA and OPEN in same session
 *   PROBE FINGERPRINT — device probing known-sensitive SSID patterns
 *   HIDDEN AP ACTIVITY — hidden SSID with high beacon rate (anomalous)
 *   CHANNEL ANOMALY — AP seen on non-standard channel for its band
 */

void run_offline_pkt_analysis();

#endif
