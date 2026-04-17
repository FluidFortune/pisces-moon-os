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

#ifndef BLE_GATT_EXPLORER_H
#define BLE_GATT_EXPLORER_H

/**
 * PISCES MOON OS — BLE GATT EXPLORER
 * Connect to a BLE device and enumerate its GATT service/characteristic tree.
 * Displays services, characteristics, properties, and readable values.
 * Logs full results to /cyber_logs/gatt_NNNNNNNNNN.json on Ghost Partition.
 *
 * USE ONLY ON DEVICES YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 *
 * NimBLE coexistence:
 *   Calls wardrive_ble_stop() before scanning, wardrive_ble_resume() on exit.
 */

void run_ble_gatt_explorer();

#endif
