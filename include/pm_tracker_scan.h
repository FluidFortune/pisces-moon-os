// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_tracker_scan.h — Bluetooth tracker / AirTag detector
//
//  Stalking-aware BLE scanner. Filters all nearby BLE
//  advertisements for known tracker manufacturer IDs and
//  surfaces them with persistence/proximity data:
//
//    APPLE FIND MY  (AirTag, FindMy accessory, lost AirPods)
//      Company ID 0x004C, type byte 0x12 with length 0x19
//
//    TILE TRACKER   (Tile Pro, Mate, Slim, Sticker)
//      Company ID 0x00A8
//
//    SAMSUNG SMARTTAG  (Galaxy SmartTag/SmartTag+)
//      Company ID 0x0075
//
//  This is a deliberately user-facing tool: when Jen taps SCAN,
//  she wants to know "are there trackers near me?" — not the
//  full BLE scan firehose. We filter aggressively and present
//  only suspicious devices, with RSSI converted to a rough
//  distance band, and how long the tracker has been visible
//  during the scan session (a tracker seen for 30+ seconds
//  while you're moving is the actual concern).
//
//  Runs as a foreground app — does NOT spawn a background task.
//  The Ghost Engine wardrive task is independent; this scanner
//  uses the same NimBLE stack but in a UI-driven loop.
// ─────────────────────────────────────────────

#pragma once

void pm_run_tracker_scan();   // touch-kiosk entry point
