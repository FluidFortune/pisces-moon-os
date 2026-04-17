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

/**
 * PISCES MOON OS — GHOST PARTITION SYSTEM
 * =========================================
 * Dual-partition SD card management, PIN router, and boot key detection.
 *
 * This module is a no-op when GHOST_PARTITION_ENABLED is not defined.
 * All functions exist but return safe defaults, so main.cpp can call
 * them unconditionally without #ifdef guards scattered everywhere.
 *
 * Architecture:
 *   sdPublic  — SdFat instance for Partition 1 (always mounted as 'sd')
 *   sdGhost   — SdFat instance for Partition 2 (Tactical Mode only)
 *
 * The global 'sd' object in main.cpp always points to Partition 1.
 * Apps that need Ghost Partition data call ghost_partition_get_sd()
 * to get the Partition 2 handle, and guard with ghost_partition_is_tactical().
 *
 * SPI Bus Safety:
 *   Both partitions share the same CS pin and SPI bus.
 *   SdFat 2.2.3 handles arbitration internally.
 *   No blocking operations during mount — SPI Bus Treaty preserved.
 */

#ifndef GHOST_PARTITION_H
#define GHOST_PARTITION_H

#include <Arduino.h>
#include <FS.h>
#include "SdFat.h"
#include "security_config.h"
#include "trackball.h"

// ─────────────────────────────────────────────
//  PUBLIC API
//  Call these from main.cpp — all are safe to
//  call regardless of GHOST_PARTITION_ENABLED.
// ─────────────────────────────────────────────

/**
 * ghost_partition_check_boot_keys()
 * Call early in setup(), after init_trackball() but before SD mount.
 * Reads the hardware key combo to determine if Ghost detection should run.
 * Safe no-op if GHOST_PARTITION_ENABLED is not defined.
 */
void ghost_partition_check_boot_keys();

/**
 * ghost_partition_mount_public(csPin, spi)
 * Mounts Partition 1. Call this where sd.begin() currently lives in main.cpp.
 * Returns true on success. Replaces the direct sd.begin() call.
 */
bool ghost_partition_mount_public(uint8_t csPin, SPIClass& spi);

/**
 * ghost_partition_run_pin_screen()
 * Shows the PIN entry screen after the splash, if Ghost Partition is enabled
 * and the boot key combo was detected. Sets the runtime mode accordingly.
 * Safe no-op (stays in NORMAL mode) if not enabled or key combo not held.
 */
void ghost_partition_run_pin_screen();

/**
 * ghost_partition_get_mode()
 * Returns the current runtime mode (NORMAL, STUDENT, or TACTICAL).
 * Use this to gate which apps appear in the launcher.
 */
PiscesMoonMode ghost_partition_get_mode();

/**
 * ghost_partition_is_tactical()
 * Convenience check — returns true only in TACTICAL mode.
 * Use this in wardrive, Gemini terminal, etc. to guard Ghost Partition writes.
 */
bool ghost_partition_is_tactical();

/**
 * ghost_partition_get_sd()
 * Returns a pointer to the Ghost Partition SdFat instance (Partition 2).
 * Returns nullptr if not in TACTICAL mode or Ghost Partition not enabled.
 * Always null-check the return value before use.
 */
SdFat* ghost_partition_get_sd();

/**
 * ghost_partition_unmount()
 * Safely unmounts Partition 2. Call before deep sleep or mode switching.
 */
void ghost_partition_unmount();

#endif // GHOST_PARTITION_H