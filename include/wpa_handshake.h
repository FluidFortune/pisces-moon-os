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

#ifndef WPA_HANDSHAKE_H
#define WPA_HANDSHAKE_H

/**
 * PISCES MOON OS — WPA HANDSHAKE CAPTURE
 * Passive promiscuous mode capture of WPA/WPA2 EAPOL 4-way handshakes.
 * Saves captures in .hccapx format for offline analysis with Hashcat.
 *
 * PASSIVE ONLY — does not transmit any frames.
 * USE ONLY ON NETWORKS YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 *
 * Output: /cyber_logs/handshake_NNNN.hccapx
 */

void run_wpa_handshake();

#endif
