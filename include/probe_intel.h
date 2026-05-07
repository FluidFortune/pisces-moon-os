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

#ifndef PROBE_INTEL_H
#define PROBE_INTEL_H

// ─────────────────────────────────────────────
//  PROBE INTEL — RF Device Intelligence App
//
//  Two modes, user-selectable on launch:
//
//  SCAN MODE (standalone)
//    Active WiFi scans every ~8s. Builds a list of nearby
//    networks with SSID, BSSID, RSSI, encryption, GPS coords.
//    Logs to /probe_intel_NNNN.csv on SD. No host required.
//    Best for: field work with the T-Deck Plus as a solo unit.
//
//  PROMISCUOUS MODE (edge node)
//    True 802.11 monitor mode — captures all management frames
//    passively (beacons, probe-req, probe-resp, deauth, auth, etc).
//    Displays live frame-type counters on screen.
//    If Bridge is also active, the wardrive Ghost Engine streams
//    {event:"pkt",...} JSON over Serial to the connected host.
//    If Bridge is NOT active, still works standalone showing stats.
//    Best for: T-Deck as an edge sensor feeding a laptop dashboard.
//
//  Mode selection is on the launch screen — trackball up/down,
//  trackball click or ENTER to confirm. Can switch mid-session
//  via [M] key from either mode screen.
//
//  SPI Bus Treaty:
//    Scan mode SD writes are wrapped in spi_mutex.
//    Promiscuous mode uses pm_promiscuous.h queue which is
//    already ISR-safe. No SD writes in promiscuous mode.
// ─────────────────────────────────────────────

void run_probe_intel();

#endif // PROBE_INTEL_H
