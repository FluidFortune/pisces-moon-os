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

#ifndef BLE_DUCKY_H
#define BLE_DUCKY_H

/**
 * PISCES MOON OS — BLE DUCKY v1.0
 * Wireless HID keyboard injection over Bluetooth LE.
 * Advertises as a Bluetooth keyboard, pairs with any BLE-capable host,
 * then injects keystrokes from DuckyScript-format payload files on SD card.
 *
 * Payload files: /payloads/*.txt — DuckyScript syntax
 * Ghost Partition: /payloads/ on Ghost Partition hidden in Student Mode
 *
 * USE ONLY ON SYSTEMS YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 *
 * NimBLE coexistence:
 *   Calls wardrive_ble_stop() before advertising.
 *   wardrive_ble_resume() called on exit.
 *   T-Deck appears as "PM-Keyboard" to the target host.
 */

void run_ble_ducky();

#endif
