// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// rf_sniffer was removed in favor of folding LoRa observation
// directly into the WarDrive engine. LoRa node sightings now appear
// in /wardrive_NNNN.csv as Type=LORA rows alongside WIFI and BT-LE,
// produced by wardrive_task() itself — no separate app, no separate
// CYBER tile.
//
// This file is intentionally empty. The launcher entries and apps.h
// prototype were removed when the app was removed, so there should
// be no remaining references. If a future build pulls a stale
// reference from somewhere, it will fail to link, which is the
// desired signal.

#ifndef RF_SNIFFER_H
#define RF_SNIFFER_H
#endif
