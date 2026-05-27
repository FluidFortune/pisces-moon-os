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
 * PISCES MOON OS — SECURITY CONFIGURATION
 * =========================================
 * Ghost Partition / Dual-PIN / Deniable Encryption system.
 *
 * SETUP INSTRUCTIONS:
 * 1. This file ships disabled by default for release/community builds.
 * 2. To enable the Ghost Partition system locally, create
 *    include/security_config_private.h and define:
 *      GHOST_PARTITION_ENABLED
 *      PRIMARY_PIN
 *      DECOY_PIN
 *      NUKE_PIN
 * 3. Never commit real PINs or private security config to GitHub.
 * 4. Format a 64GB MicroSD card with TWO FAT32 partitions, each <= 32GB
 *    - Partition 1: Public / Decoy data (medical, baseball, music, games)
 *    - Partition 2: Ghost / Tactical data (wardrive logs, Gemini vault, scan results)
 * 5. Keep security_config_private.h in .gitignore — DO NOT commit your PINs to GitHub
 *
 * IF GHOST PARTITION IS NOT ENABLED:
 * The device boots normally with no PIN screen. All data goes to Partition 1.
 * A standard single-partition SD card works perfectly. Nothing breaks.
 *
 * HARDWARE REQUIREMENT (Ghost Partition only):
 * At boot, hold TRK_CLICK (GPIO0) + TRK_LEFT (GPIO1) to signal that
 * Ghost Partition detection should run. Without this key combo, the device
 * boots directly to Student/Normal mode even on a dual-partition card.
 * This is your hardware "Dead Man's Switch" — physical MFA.
 *
 * SD CARD FORMATTING GUIDE:
 * macOS:   diskutil partitionDisk /dev/diskN MBR FAT32 "PUBLIC" 30G FAT32 "GHOST" R
 * Linux:   Use fdisk to create two primary FAT32 partitions, then mkfs.fat on each
 * Windows: Diskpart — Windows will only see Partition 1 natively (the forensic decoy)
 *
 * SPI BUS SAFETY:
 * Both partitions use the same SdFat SPI config and CS pin.
 * SdFat 2.2.3 handles partition switching internally — the LoRa module
 * is unaffected because no blocking operations occur during mount.
 *
 * "Security through architecture, not through complexity." — Pisces Moon OS
 */

#ifndef SECURITY_CONFIG_H
#define SECURITY_CONFIG_H

#if __has_include("security_config_private.h")
#include "security_config_private.h"
#endif

// ─────────────────────────────────────────────
//  MASTER SWITCH
//  Release/community builds leave Ghost Partition
//  disabled by default. Define this only in a local
//  private config or private build flags.
// ─────────────────────────────────────────────
// #define GHOST_PARTITION_ENABLED

// ─────────────────────────────────────────────
//  PIN CONFIGURATION
//  Only relevant when GHOST_PARTITION_ENABLED is defined.
//  Use any keyboard-typeable string. Longer = more secure.
//  The trackball CLICK key acts as ENTER during PIN entry.
//  Tip: avoid PINs that spell obvious words.
// ─────────────────────────────────────────────
#ifdef GHOST_PARTITION_ENABLED

    // Primary PIN: unlocks Tactical Mode + Ghost Partition
    // #define PRIMARY_PIN     "set-in-security_config_private.h"

    // Decoy PIN: unlocks Student Mode only (Partition 1, safe apps)
    // Use this if forced to unlock the device under duress
    // #define DECOY_PIN       "set-in-security_config_private.h"

    // Nuke PIN: deletes Ghost Partition index files then loads Student Mode
    // Data is irrecoverable after this — use with intent
    // #define NUKE_PIN        "set-in-security_config_private.h"

#if !defined(PRIMARY_PIN) || !defined(DECOY_PIN) || !defined(NUKE_PIN)
    #error "GHOST_PARTITION_ENABLED requires PRIMARY_PIN, DECOY_PIN, and NUKE_PIN in security_config_private.h or private build flags."
#endif

    // Maximum failed PIN attempts before lockout engages
    // #define PIN_MAX_ATTEMPTS    3
#ifndef PIN_MAX_ATTEMPTS
    #define PIN_MAX_ATTEMPTS    3
#endif

    // Lockout duration in milliseconds (default: 60 seconds)
    // LoRa and background tasks continue running during lockout
    // #define PIN_LOCKOUT_MS      60000UL
#ifndef PIN_LOCKOUT_MS
    #define PIN_LOCKOUT_MS      60000UL
#endif

    // Boot key combo to enable Ghost Partition detection
    // Both must be held LOW at boot (active LOW, INPUT_PULLUP)
    // #define GHOST_KEY_1     TRK_CLICK   // GPIO0
    // #define GHOST_KEY_2     TRK_LEFT    // GPIO1

    // ── MFA UNLOCK KEY ──────────────────────────────────────────────
    // The hidden second factor after a correct Primary PIN entry.
    // After entering the Primary PIN, a "PRESS UNLOCK KEY" screen appears.
    // Only this key loads Tactical Mode. ANY other key silently loads
    // Student Mode instead — the user sees no indication of failure.
    // This key does not appear on any label on the device.
    // Override in security_config_private.h to change.
    // Default: backtick (`) — obscure position, not used in PINs
    #ifndef UNLOCK_KEY
        #define UNLOCK_KEY      '`'
    #endif
    // ────────────────────────────────────────────────────────────────

    // SD Card partition indices (1-based, per SdFat convention)
    #define PARTITION_PUBLIC    1   // Always mounted — decoy/normal data
    #define PARTITION_GHOST     2   // Mounted in Tactical Mode only

    // Ghost Partition directory structure
    // These paths are on Partition 2 (sdGhost)
    #define GHOST_WARDRIVE_DIR  "/wardrive"
    #define GHOST_VAULT_DIR     "/vault"
    #define GHOST_GEMINI_DIR    "/data/gemini"
    #define GHOST_SCANS_DIR     "/scans"
    #define GHOST_NOTES_DIR     "/notes"

    // Nuke targets — index files that render each Vault category invisible
    // Deleting these is a metadata-only operation: fast, SPI-bus safe
    #define NUKE_TARGETS_COUNT  5
    static const char* NUKE_INDEX_FILES[NUKE_TARGETS_COUNT] = {
        "/data/gemini/index.json",
        "/vault/index.json",
        "/scans/index.json",
        "/notes/index.json",
        "/wardrive/index.json"
    };

    // Wardrive flat-file rotation targets.
    //
    // v1.2.1 — Wardrive sessions are written as /wardrive_NNNN.csv at the
    // root of whichever partition wardrive is using (currently the PUBLIC
    // partition — see wardrive.cpp). Session numbers are 1-9999.
    //
    // Rather than hardcoding filenames, the nuke routine dynamically scans
    // for files matching this prefix and extension on BOTH partitions, so
    // it stays correct regardless of session count or which partition
    // wardrive is using.
    #define WARDRIVE_LOG_PREFIX  "/wardrive_"
    #define WARDRIVE_LOG_SUFFIX  ".csv"
    #define WARDRIVE_LOG_MAX_SESSION  9999

#endif // GHOST_PARTITION_ENABLED

// ─────────────────────────────────────────────
//  RUNTIME MODE FLAGS
//  These are set at boot by the PIN router.
//  Read-only after boot — do not modify directly.
//  Access via ghost_partition_get_mode() in ghost_partition.h
// ─────────────────────────────────────────────
typedef enum {
    PM_MODE_NORMAL   = 0,   // No Ghost Partition — standard single-partition boot
    PM_MODE_STUDENT  = 1,   // Decoy PIN entered — Partition 1 only, safe apps
    PM_MODE_TACTICAL = 2    // Primary PIN + key combo — both partitions mounted
} PiscesMoonMode;

#endif // SECURITY_CONFIG_H