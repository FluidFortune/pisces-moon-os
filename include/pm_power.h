// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef PM_POWER_H
#define PM_POWER_H

// ============================================================
//  pm_power.h
//  Pisces Moon OS — software power management
//
//  Handles graceful shutdown to ESP32 deep sleep:
//    1. Signal Ghost Engine to stop
//    2. Halt wardrive session and flush its file
//    3. Unmount SD card cleanly
//    4. Cut all XL9555 peripheral power rails
//       (LoRa, GPS, NFC, haptic driver, audio amp)
//    5. Turn off display backlight
//    6. Put display panel in sleep mode
//    7. Configure wake-up pin(s)
//    8. Call esp_deep_sleep_start()
//
//  Wake-up triggers:
//    - Rotary encoder center button (BOARD_ENC_BTN, GPIO 7)
//    - BOOT button (GPIO 0)
//
//  Both wired as active-LOW so we use EXT1_WAKEUP_ANY_LOW
//  per LilyGo's reference firmware.
//
//  Current draw in deep sleep: ~860 µA (per LilyGo docs).
//  At 1500 mAh battery capacity, theoretical standby time is
//  about 72 days — enough that "leaving it off for a week"
//  works without thinking about it.
//
//  After wake-up, ESP32-S3 resets and re-runs setup(). The
//  full BIOS boot screen runs, SD remounts, and wardrive resumes
//  if active before sleep.
//
//  USAGE
//  -----
//  From any app or the launcher:
//
//      #include "pm_power.h"
//      pm_power_sleep();   // never returns
//
//  This is a one-shot call — execution never continues past
//  it. The device is in deep sleep until a wake button is
//  pressed.
//
//  Suggested binding: long-press the rotary center button
//  (~2-3 seconds) from the launcher. See launcher.cpp's input
//  handler.
// ============================================================

// Trigger a clean shutdown and enter deep sleep.
// Does not return. Wake-up via rotary button or BOOT button
// will trigger a fresh ESP32 boot through setup().
void pm_power_sleep();

#endif // PM_POWER_H
