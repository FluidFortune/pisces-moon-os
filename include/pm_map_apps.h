// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com

#ifndef PM_MAP_APPS_H
#define PM_MAP_APPS_H

// ─────────────────────────────────────────────────────────────
//  Map-flavored apps built on pm_map_engine. All render per-device
//  (Cardputer ADV, T-Deck Plus, T-LoRa Pager, C28P, Maxine) by
//  drawing through the engine, which sizes itself from gfx at
//  runtime. Input is unified across touch / trackball / keyboard.
//
//  Shared controls (every map app):
//    +/-            zoom in / out      (also trackball-click cycles)
//    arrows/WASD    pan                (also trackball roll, touch drag)
//    g / center-tap recenter on GPS + follow mode
//    q / back       exit
// ─────────────────────────────────────────────────────────────

// MAP — offline moving map + GPS breadcrumb. The anchor app: pan/zoom
// a raster basemap from /maps/, drop a trail of where you've been,
// tap to read out a coordinate. Works with no SD (graticule mode).
void run_map();

// FLIGHT TRACKER — live aircraft on the moving map. Pulls the OpenSky
// Network free API over WiFi for a bounding box around the viewport,
// plots each aircraft as a heading arrow with callsign + altitude.
// Requires WiFi; degrades to "no link" banner offline. (Direct 1090
// MHz ADS-B reception is the hardware-companion path, future.)
void run_flight_tracker();

// MESH MAP — live Meshtastic/LoRa node positions on the moving map.
// Reuses the passive SX1262 RX path: every heard packet's origin is
// placed at our GPS fix (where we heard it) as a node dot, with
// RSSI-driven color and a range ring. The spatial, in-field view of
// the same data WarDrive logs to CSV. SX1262 devices only.
void run_mesh_map();

#endif // PM_MAP_APPS_H
