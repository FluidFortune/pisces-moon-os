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
 * 1. Copy this file is already named security_config.h (it ships disabled by default)
 * 2. To enable the Ghost Partition system, uncomment #define GHOST_PARTITION_ENABLED
 * 3. Set your three PINs below (any length, alphanumeric, keyboard-typeable)
 * 4. Format a 64GB MicroSD card with TWO FAT32 partitions, each <= 32GB
 *    - Partition 1: Public / Decoy data (medical, baseball, music, games)
 *    - Partition 2: Ghost / Tactical data (wardrive logs, Gemini vault, scan results)
 * 5. Add security_config.h to your .gitignore — DO NOT commit your PINs to GitHub
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

// ─────────────────────────────────────────────
//  MASTER SWITCH
//  Uncomment the line below to enable the Ghost
//  Partition system. Leave commented for standard
//  single-partition operation (default/community build).
// ─────────────────────────────────────────────
#define GHOST_PARTITION_ENABLED

// ─────────────────────────────────────────────
//  PIN CONFIGURATION
//  Only relevant when GHOST_PARTITION_ENABLED is defined.
//  Use any keyboard-typeable string. Longer = more secure.
//  The trackball CLICK key acts as ENTER during PIN entry.
//  Tip: avoid PINs that spell obvious words.
// ─────────────────────────────────────────────
#ifdef GHOST_PARTITION_ENABLED

    // Primary PIN: unlocks Tactical Mode + Ghost Partition
    #define PRIMARY_PIN     "0322"

    // Decoy PIN: unlocks Student Mode only (Partition 1, safe apps)
    // Use this if forced to unlock the device under duress
    #define DECOY_PIN       "3360"

    // Nuke PIN: deletes Ghost Partition index files then loads Student Mode
    // Data is irrecoverable after this — use with intent
    #define NUKE_PIN        "0911"

    // Maximum failed PIN attempts before lockout engages
    #define PIN_MAX_ATTEMPTS    3

    // Lockout duration in milliseconds (default: 60 seconds)
    // LoRa and background tasks continue running during lockout
    #define PIN_LOCKOUT_MS      60000UL

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
    // Change this to any keyboard-typeable character.
    // Default: backtick (`) — obscure position, not used in PINs
    // #define UNLOCK_KEY      '`'
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

    // Wardrive flat-file rotation targets (no index — must remove individually)
    // Matches the wardrive rolling log naming scheme
    #define WARDRIVE_LOG_COUNT  10
    static const char* WARDRIVE_LOGS[WARDRIVE_LOG_COUNT] = {
        "/wardrive/wardrive_01.csv", "/wardrive/wardrive_02.csv",
        "/wardrive/wardrive_03.csv", "/wardrive/wardrive_04.csv",
        "/wardrive/wardrive_05.csv", "/wardrive/wardrive_06.csv",
        "/wardrive/wardrive_07.csv", "/wardrive/wardrive_08.csv",
        "/wardrive/wardrive_09.csv", "/wardrive/wardrive_10.csv"
    };

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