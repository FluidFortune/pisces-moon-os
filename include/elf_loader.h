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
//  elf_loader.h — Pisces Moon OS ELF Engine
//  "ELF ON A SHELF" — Modular App Runtime v1.0
// ============================================================
//
//  SPLIT ARCHITECTURE — this header is safe to #include anywhere:
//    elf_loader.h   — structs, enums, extern declarations, API surface
//    elf_loader.cpp — variable definitions + all function bodies
//
//  NO variable definitions live in this header. Every object that
//  needs storage is declared extern here and defined once in
//  elf_loader.cpp. This eliminates the "Multiple Definition" linker
//  error that occurs when a header with definitions is included by
//  more than one translation unit (main.cpp, launcher.cpp, etc.).
//
//  ELF ABI Contract (all ELF modules must implement):
//    extern "C" int elf_main(void* ctx);
//      - ctx: cast to ElfContext* inside the module
//      - return 0 = clean exit, -1 = error
//
//  Required compile flags for ELF module sources:
//    -fPIC -mfix-esp32-psram-cache-issue --entry elf_main
//
//  SD Card Layout:
//    /apps/          — ELF modules + JSON sidecar manifests
//    /roms/nes/      — NES ROM files (.nes)
//    /roms/gb/       — Game Boy ROM files (.gb, .gbc)
//    /roms/atari/    — Atari 2600 ROM files (.a26, .bin)
//
//  Global variable convention (matches existing OS pattern):
//    extern SdFat sd;           — defined in main.cpp
//    extern Arduino_GFX* gfx;  — defined in main.cpp
//    These are injected into ElfContext by elf_build_context().
//
// ============================================================

#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <Arduino.h>
#include <FS.h>
#include "SdFat.h"
#include <Arduino_GFX_Library.h>

// ============================================================
//  Global OS objects — defined in main.cpp
//  Declaring them extern here means every .cpp that includes
//  this header can see them without a duplicate definition.
// ============================================================
extern SdFat sd;
extern Arduino_GFX* gfx;

// SPI Bus Treaty flags — defined in their respective .cpp files
extern volatile bool wifi_in_use;       // wardrive.cpp
extern volatile bool lora_voice_active; // lora_voice.cpp

// ============================================================
//  ElfContext — OS → ELF handshake packet
//  Passed to every loaded ELF via elf_main(ctx).
//  ELF module receives this as (void*) and casts to (ElfContext*).
// ============================================================
struct ElfContext {
    // Hardware access — populated by elf_build_context()
    Arduino_GFX* gfx;           // Display driver (always valid)
    SdFat*       sd;            // SD filesystem, public partition
    void*        gamepad;       // Cast to GamepadState* (gamepad.h)

    // Display geometry
    uint16_t screen_w;          // 320
    uint16_t screen_h;          // 240

    // SPI Bus Treaty flags — ELF modules must honor these
    volatile bool* wifi_in_use;    // Check before any WiFi API calls
    volatile bool* lora_active;    // Check before SD ops during LoRa voice

    // ROM path — emulator ELFs only; empty string for standalone apps
    char rom_path[128];         // Full SD path, e.g. "/roms/nes/smb.nes"
    char rom_ext[8];            // Lowercase extension: "nes","gb","gbc","a26","bin"

    // Platform version for ABI compatibility checks inside ELF modules
    uint8_t api_major;          // 1
    uint8_t api_minor;          // 0
    char    os_version[16];     // "0.9.6"

    // Reserved — zero-filled, available for ELF API v1.1+
    uint8_t _reserved[64];
};

// ============================================================
//  ElfManifest — parsed from /apps/<name>.json sidecar
//
//  Sidecar format:
//  {
//    "name":     "Nofrendo NES",
//    "version":  "1.0.0",
//    "author":   "Community",
//    "category": "GAMES",
//    "elf":      "nofrendo.elf",
//    "rom_ext":  ["nes"],
//    "psram_kb": 2048,
//    "api":      [1, 0]
//  }
//
//  ELFs without a sidecar are still loadable — the browser
//  creates a minimal manifest from the filename alone and
//  sets psram_kb = 1024 as a conservative default.
// ============================================================
struct ElfManifest {
    char     name[48];
    char     version[16];
    char     author[48];
    char     category[16];
    char     elf_file[64];      // filename only, relative to /apps/
    char     rom_exts[4][8];    // up to 4 ROM extensions this ELF handles
    uint16_t psram_kb;          // PSRAM required in KB (0 = skip check)
    uint8_t  api_major;
    uint8_t  api_minor;
    bool     valid;             // false if parse failed or file missing
};

// ============================================================
//  ElfLoadResult — return codes from elf_execute() and friends
// ============================================================
enum ElfLoadResult {
    ELF_OK            = 0,  // Clean exit
    ELF_NOT_FOUND     = 1,  // .elf file missing from SD
    ELF_NO_PSRAM      = 2,  // Insufficient free PSRAM
    ELF_BAD_MAGIC     = 3,  // First 4 bytes are not 0x7F 'E' 'L' 'F'
    ELF_NO_ENTRY      = 4,  // elf_main() entry point not resolvable
    ELF_ABI_MISMATCH  = 5,  // manifest api_major > ELF_API_MAJOR_SUPPORTED
    ELF_SD_ERROR      = 6,  // SD open or read failure
    ELF_EXEC_ERROR    = 7,  // elf_main() returned non-zero
};

// Maximum ELF API major version this OS release supports
#define ELF_API_MAJOR_SUPPORTED  1

// ============================================================
//  Internal engine state — DEFINED in elf_loader.cpp
//  extern here = "the storage is elsewhere, trust me linker"
// ============================================================
namespace _elf_internal {
    extern void*   psram_region;       // Current PSRAM allocation
    extern size_t  psram_region_size;  // Size of that allocation
    extern bool    elf_running;        // True while elf_main() is on the stack
}

// ============================================================
//  Public API — all bodies in elf_loader.cpp
// ============================================================

// Build a context struct from current OS state.
// Must be called before passing ctx to elf_execute().
// Pass nullptr for rom_path when launching non-emulator apps.
void elf_build_context(ElfContext* ctx, const char* rom_path);

// Load and execute an ELF by full SD path (e.g. "/apps/nofrendo.elf").
// Allocates PSRAM → validates ELF magic → reads binary →
// calls elf_main(ctx) → wipes and frees PSRAM on return.
ElfLoadResult elf_execute(const char* elf_path, ElfContext* ctx);

// Convenience wrapper: validates ABI version and PSRAM headroom
// from manifest before calling elf_execute().
ElfLoadResult elf_execute_manifest(const ElfManifest* manifest, ElfContext* ctx);

// Parse the JSON sidecar for a given ELF filename.
// elf_filename = bare filename, e.g. "nofrendo.elf"
// Constructs the path /apps/<base>.json and parses it.
// Returns true on success; out->valid will also be true.
bool elf_load_manifest(const char* elf_filename, ElfManifest* out);

// Scan /apps/ on SD, load manifests for all .elf files found.
// Returns count of entries populated in manifests[].
int elf_scan_apps(ElfManifest* manifests, int max_count);

// Free the PSRAM region from the last ELF execution.
// Called automatically by elf_execute() on all exit paths.
// Exposed here for error recovery (e.g. watchdog triggered mid-ELF).
void elf_free_psram();

// Return a human-readable string for an ElfLoadResult code.
// Returned string is a string literal — do not free().
const char* elf_result_str(ElfLoadResult r);

// ============================================================
//  File Browser Hook
//  Call this from filesystem.cpp when the user taps a .elf file.
//  Builds the context automatically and calls elf_execute().
//  Returns ELF_NOT_FOUND if the path doesn't end in ".elf".
// ============================================================
ElfLoadResult elf_launch_from_browser(const char* full_sd_path);

#endif // ELF_LOADER_H