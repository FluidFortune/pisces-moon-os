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

// ============================================================
//  retro_elf_pack.h — Pisces Moon OS Retro ELF Pack
//  "ELF ON A SHELF" — ROM Browser + Emulator Launcher v1.0
// ============================================================
//
//  SPLIT ARCHITECTURE:
//    retro_elf_pack.h   — structs, constants, extern declarations
//    retro_elf_pack.cpp — all function bodies (run_retro_pack, etc.)
//
//  NO function bodies live in this header. Previous versions had
//  run_retro_pack() defined here, which caused duplicate-definition
//  linker errors when included by multiple translation units.
//
//  SD Card layout required:
//    /roms/nes/     ← .nes files
//    /roms/gb/      ← .gb .gbc files
//    /roms/atari/   ← .a26 .bin files
//    /apps/         ← nofrendo.elf, gnuboy.elf, stella.elf + .json sidecars
//
//  Extension → Emulator routing:
//    .nes            → /apps/nofrendo.elf
//    .gb / .gbc      → /apps/gnuboy.elf
//    .a26 / .bin     → /apps/stella.elf
//
// ============================================================

#ifndef RETRO_ELF_PACK_H
#define RETRO_ELF_PACK_H

#include <Arduino.h>

// ─────────────────────────────────────────────
//  SYSTEM TYPES
// ─────────────────────────────────────────────
enum RetroSystem {
    SYS_NES   = 0,
    SYS_GB    = 1,
    SYS_ATARI = 2,
    SYS_COUNT = 3
};

// ─────────────────────────────────────────────
//  ROM ENTRY
//  One entry per ROM file found on SD card.
// ─────────────────────────────────────────────
#define ROM_NAME_MAX  48
#define ROM_PATH_MAX  96
#define ROM_LIST_MAX  128   // Max ROMs displayed (memory budget)

struct RomEntry {
    char         name[ROM_NAME_MAX];   // Filename without extension
    char         path[ROM_PATH_MAX];   // Full SD path, e.g. /roms/nes/game.nes
    RetroSystem  system;               // SYS_NES / SYS_GB / SYS_ATARI
};

// ─────────────────────────────────────────────
//  SYSTEM METADATA TABLE
//  Indexed by RetroSystem enum value.
// ─────────────────────────────────────────────
struct RetroSystemInfo {
    const char* dir;         // SD scan directory, e.g. "/roms/nes"
    const char* elf;         // Emulator ELF path, e.g. "/apps/nofrendo.elf"
    const char* badge;       // Short label shown in browser, e.g. "NES"
    const char* exts[3];     // Recognised extensions (null-terminated list)
    uint16_t    color;       // RGB565 badge color
    uint16_t    psram_kb;    // PSRAM footprint estimate for PSRAM check
};

// Defined in retro_elf_pack.cpp — extern here for any code that needs it
extern const RetroSystemInfo RETRO_SYSTEMS[SYS_COUNT];

// ─────────────────────────────────────────────
//  PUBLIC API — bodies in retro_elf_pack.cpp
// ─────────────────────────────────────────────

// Main entry point — called from launcher via APP_RETRO
void run_retro_pack();

#endif // RETRO_ELF_PACK_H