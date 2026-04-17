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

#ifndef USB_DUCKY_H
#define USB_DUCKY_H

/**
 * PISCES MOON OS — USB DUCKY v1.0
 * Wired USB HID keyboard injection.
 *
 * REQUIRES SEPARATE BUILD: platformio_hid.ini
 *   build_flags += -DARDUINO_USB_MODE=0 (HID mode, disables CDC serial)
 *   build_flags += -DARDUINO_USB_HID_MODE=1
 *
 * In the standard CDC build (ARDUINO_USB_MODE=1), this app shows a
 * "WRONG BUILD" screen explaining how to flash the HID variant.
 *
 * In the HID build (ARDUINO_USB_MODE=0):
 *   - T-Deck enumerates as USB HID keyboard (no driver install on host)
 *   - Executes DuckyScript payloads from /payloads/ on SD card
 *   - GPIO1 (TRK_LEFT) may conflict without CDC — serial debug disabled
 *
 * Flash workflow for HID build:
 *   pio run -e esp32s3_hid --target upload
 *   (Use UART0 via separate USB-serial adapter if USB-CDC upload fails)
 *
 * USE ONLY ON SYSTEMS YOU OWN OR HAVE EXPLICIT AUTHORIZATION TO TEST.
 */

void run_usb_ducky();

#endif
