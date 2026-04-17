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
//  elf_loader.cpp — Pisces Moon OS ELF Engine Implementation
//  "ELF ON A SHELF" — v1.0
// ============================================================
//
//  This file owns:
//    - All variable definitions for _elf_internal namespace
//    - All function bodies declared in elf_loader.h
//
//  Include order follows The FS.h Mandate:
//    #include <FS.h> BEFORE #include "SdFat.h"
//
// ============================================================

#include <Arduino.h>
#include <FS.h>
#include "SdFat.h"
#include <Arduino_GFX_Library.h>
#include "elf_loader.h"
#include "gamepad.h"    // g_gamepad, GP_* masks, gamepad_poll()
#include "trackball.h"  // update_trackball() for error screen dismiss
#include "theme.h"      // C_BLACK, C_GREEN, C_WHITE, C_GREY, C_DARK, C_RED
#include "keyboard.h"   // get_keypress() for error screen dismiss

// ============================================================
//  Internal state — ONE definition, here and only here.
//  The header declares these extern; this file defines them.
// ============================================================
namespace _elf_internal {
    void*   psram_region      = nullptr;
    size_t  psram_region_size = 0;
    bool    elf_running       = false;
}

// ============================================================
//  elf_result_str()
// ============================================================
const char* elf_result_str(ElfLoadResult r) {
    switch (r) {
        case ELF_OK:           return "OK";
        case ELF_NOT_FOUND:    return "ELF file not found on SD";
        case ELF_NO_PSRAM:     return "Insufficient free PSRAM";
        case ELF_BAD_MAGIC:    return "Not a valid ELF binary";
        case ELF_NO_ENTRY:     return "elf_main() entry not resolvable";
        case ELF_ABI_MISMATCH: return "ELF API version not supported";
        case ELF_SD_ERROR:     return "SD card read error";
        case ELF_EXEC_ERROR:   return "ELF module returned error";
        default:               return "Unknown error";
    }
}

// ============================================================
//  elf_free_psram()
//  Zero and release the PSRAM allocation from the last ELF run.
//  Safe to call even if nothing is allocated (no-op).
// ============================================================
void elf_free_psram() {
    if (_elf_internal::psram_region) {
        // Zero the region before freeing — no stale game state or
        // sensitive data lingers in PSRAM between ELF sessions.
        memset(_elf_internal::psram_region, 0, _elf_internal::psram_region_size);
        free(_elf_internal::psram_region);
        _elf_internal::psram_region      = nullptr;
        _elf_internal::psram_region_size = 0;
    }
    _elf_internal::elf_running = false;
}

// ============================================================
//  elf_build_context()
//  Populate an ElfContext from current OS state.
//  The caller must pass their gamepad pointer separately if needed;
//  this function wires the OS globals (gfx, sd, treaty flags).
// ============================================================
void elf_build_context(ElfContext* ctx, const char* rom_path) {
    memset(ctx, 0, sizeof(ElfContext));

    // Wire OS hardware globals (defined in main.cpp)
    ctx->gfx      = gfx;
    ctx->sd       = &sd;
    ctx->gamepad  = &g_gamepad;  // global GamepadState from gamepad.h

    // Display geometry — ST7789 on T-Deck Plus
    ctx->screen_w = 320;
    ctx->screen_h = 240;

    // SPI Bus Treaty flags
    ctx->wifi_in_use  = &wifi_in_use;
    ctx->lora_active  = &lora_voice_active;

    // Platform version
    ctx->api_major = ELF_API_MAJOR_SUPPORTED;
    ctx->api_minor = 0;
    strncpy(ctx->os_version, PISCES_OS_VERSION, sizeof(ctx->os_version) - 1);

    // ROM path (emulator ELFs only)
    if (rom_path && rom_path[0]) {
        strncpy(ctx->rom_path, rom_path, sizeof(ctx->rom_path) - 1);
        // Extract extension into rom_ext
        const char* dot = strrchr(rom_path, '.');
        if (dot && *(dot + 1)) {
            char* dst = ctx->rom_ext;
            const char* src = dot + 1;
            int i = 0;
            while (i < 7 && *src) {
                *dst++ = tolower(*src++);
                i++;
            }
            *dst = '\0';
        }
    }
}

// ============================================================
//  elf_load_manifest()
//  Parse the JSON sidecar for a given ELF filename.
//  Constructs "/apps/<base>.json" and extracts fields.
//  Returns true on success.
// ============================================================
bool elf_load_manifest(const char* elf_filename, ElfManifest* out) {
    memset(out, 0, sizeof(ElfManifest));
    out->valid    = false;
    out->psram_kb = 1024; // Conservative default if not in manifest

    // Build sidecar path
    char json_path[96];
    snprintf(json_path, sizeof(json_path), "/apps/%s", elf_filename);
    char* dot = strrchr(json_path, '.');
    if (dot) strcpy(dot, ".json");
    else {
        // No extension — append .json
        size_t len = strlen(json_path);
        if (len < sizeof(json_path) - 6)
            strcpy(json_path + len, ".json");
        else
            return false;
    }

    FsFile f = sd.open(json_path, O_RDONLY);
    if (!f) return false;

    char buf[512];
    size_t n = f.read(buf, sizeof(buf) - 1);
    f.close();
    if (n == 0) return false;
    buf[n] = '\0';

    // -----------------------------------------------------------
    //  Minimal JSON field extraction using strstr + pointer walk.
    //  ArduinoJson is available via platformio.ini but we avoid
    //  a DynamicJsonDocument allocation in this path — manifests
    //  are small and fixed-format, strstr is sufficient.
    // -----------------------------------------------------------
    auto extract_str = [&](const char* key, char* dest, size_t dlen) {
        dest[0] = '\0';
        char search[48];
        snprintf(search, sizeof(search), "\"%s\"", key);
        const char* found = strstr(buf, search);
        if (!found) return;
        const char* colon = strchr(found + strlen(search), ':');
        if (!colon) return;
        const char* q1 = strchr(colon, '"');
        if (!q1) return;
        q1++;
        const char* q2 = strchr(q1, '"');
        if (!q2) return;
        size_t len = (size_t)(q2 - q1);
        if (len >= dlen) len = dlen - 1;
        strncpy(dest, q1, len);
        dest[len] = '\0';
    };

    auto extract_int = [&](const char* key, int def) -> int {
        char search[48];
        snprintf(search, sizeof(search), "\"%s\"", key);
        const char* found = strstr(buf, search);
        if (!found) return def;
        const char* colon = strchr(found + strlen(search), ':');
        if (!colon) return def;
        // Skip whitespace
        colon++;
        while (*colon == ' ' || *colon == '\t') colon++;
        if (!isdigit(*colon) && *colon != '-') return def;
        return atoi(colon);
    };

    extract_str("name",     out->name,     sizeof(out->name));
    extract_str("version",  out->version,  sizeof(out->version));
    extract_str("author",   out->author,   sizeof(out->author));
    extract_str("category", out->category, sizeof(out->category));
    extract_str("elf",      out->elf_file, sizeof(out->elf_file));

    int psram = extract_int("psram_kb", 1024);
    out->psram_kb = (uint16_t)(psram > 0 ? psram : 1024);

    // API version array: "api": [1, 0]
    const char* av = strstr(buf, "\"api\"");
    if (av) {
        const char* br = strchr(av, '[');
        if (br) {
            br++;
            out->api_major = (uint8_t)atoi(br);
            const char* comma = strchr(br, ',');
            if (comma) out->api_minor = (uint8_t)atoi(comma + 1);
        }
    }

    // ROM extensions array: "rom_ext": ["nes", "gb"]
    const char* re = strstr(buf, "\"rom_ext\"");
    if (re) {
        const char* br = strchr(re, '[');
        if (br) {
            int idx = 0;
            const char* p = br + 1;
            while (idx < 4 && *p && *p != ']') {
                const char* q1 = strchr(p, '"');
                if (!q1) break;
                q1++;
                const char* q2 = strchr(q1, '"');
                if (!q2) break;
                size_t len = (size_t)(q2 - q1);
                if (len > 7) len = 7;
                strncpy(out->rom_exts[idx], q1, len);
                out->rom_exts[idx][len] = '\0';
                idx++;
                p = q2 + 1;
            }
        }
    }

    out->valid = (strlen(out->name) > 0 && strlen(out->elf_file) > 0);
    return out->valid;
}

// ============================================================
//  elf_scan_apps()
//  Walk /apps/ and build manifests for all .elf files.
//  Returns count of entries populated.
// ============================================================
int elf_scan_apps(ElfManifest* manifests, int max_count) {
    int found = 0;

    FsFile dir = sd.open("/apps");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[ELF] /apps/ directory not found on SD");
        return 0;
    }

    FsFile entry;
    char fname[64];
    while (found < max_count) {
        if (!entry.openNext(&dir, O_RDONLY)) break;
        if (!entry.isDirectory()) {
            entry.getName(fname, sizeof(fname));
            size_t len = strlen(fname);
            // Only process .elf files
            if (len > 4 && strcasecmp(fname + len - 4, ".elf") == 0) {
                ElfManifest& m = manifests[found];
                if (elf_load_manifest(fname, &m)) {
                    found++;
                } else {
                    // No sidecar — minimal entry from filename
                    memset(&m, 0, sizeof(ElfManifest));
                    size_t namelen = len - 4; // strip ".elf"
                    if (namelen > 47) namelen = 47;
                    strncpy(m.name, fname, namelen);
                    m.name[namelen] = '\0';
                    strncpy(m.elf_file, fname, sizeof(m.elf_file) - 1);
                    strcpy(m.category, "APPS");
                    m.psram_kb  = 1024;
                    m.api_major = 1;
                    m.api_minor = 0;
                    m.valid     = true;
                    found++;
                }
            }
        }
        entry.close();
    }
    dir.close();

    Serial.printf("[ELF] Scan complete: %d ELF modules found in /apps/\n", found);
    return found;
}

// ============================================================
//  elf_execute()
//  Core engine function.
//
//  Execution model for ESP32-S3:
//    ELF modules are compiled as position-independent flat binaries
//    (--entry elf_main, -fPIC) and linked so that elf_main() appears
//    at the start of the .text section (i.e., at offset 0 of the
//    binary image after the ELF header).
//
//    Full ELF relocation (section header parsing, dynamic symbol
//    resolution) requires the ESP-IDF elf_loader component and is
//    outside the scope of this Arduino-framework implementation.
//    The convention here is:
//      1. Validate ELF magic bytes
//      2. Load binary into PSRAM
//      3. The entry point is resolved from the ELF e_entry field
//         (offset 24 in a 32-bit ELF header, little-endian)
//      4. Cast to function pointer and call
//
//    This works correctly for modules compiled with:
//      xtensa-esp32s3-elf-gcc -fPIC -mfix-esp32-psram-cache-issue
//      -nostdlib -Wl,--entry,elf_main -Wl,-Ttext,0
//
// ============================================================
ElfLoadResult elf_execute(const char* elf_path, ElfContext* ctx) {
    // --- Open ELF file ---
    FsFile f = sd.open(elf_path, O_RDONLY);
    if (!f) {
        Serial.printf("[ELF] File not found: %s\n", elf_path);
        return ELF_NOT_FOUND;
    }

    size_t file_size = (size_t)f.size();
    if (file_size < 52) {
        // Smaller than the minimum 32-bit ELF header
        f.close();
        Serial.printf("[ELF] File too small (%u bytes): %s\n", file_size, elf_path);
        return ELF_BAD_MAGIC;
    }

    // --- Validate ELF magic ---
    uint8_t magic[4];
    f.read(magic, 4);
    if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        f.close();
        Serial.printf("[ELF] Bad magic bytes in: %s\n", elf_path);
        return ELF_BAD_MAGIC;
    }

    // --- Read e_entry from ELF header (offset 24, uint32_t, little-endian) ---
    // We store this as a PSRAM offset; actual jump address = psram_base + e_entry
    f.seek(24);
    uint32_t e_entry_raw = 0;
    f.read(&e_entry_raw, sizeof(uint32_t));
    f.seek(0); // Rewind for full read

    // --- PSRAM allocation ---
    // CONFIG_SPIRAM_USE_MALLOC=1 routes ps_malloc to PSRAM automatically.
    // Add 16 bytes for alignment padding.
    void* region = ps_malloc(file_size + 16);
    if (!region) {
        f.close();
        Serial.printf("[ELF] ps_malloc(%u) failed — not enough PSRAM\n", file_size + 16);
        return ELF_NO_PSRAM;
    }

    // Align execution address to 16-byte boundary for ESP32 instruction cache
    uintptr_t base = ((uintptr_t)region + 15) & ~15UL;

    // --- Load ELF into PSRAM ---
    size_t bytes_read = f.read((void*)base, file_size);
    f.close();

    if (bytes_read != file_size) {
        free(region);
        Serial.printf("[ELF] SD read incomplete: got %u of %u bytes\n", bytes_read, file_size);
        return ELF_SD_ERROR;
    }

    _elf_internal::psram_region      = region;
    _elf_internal::psram_region_size = file_size + 16;

    // --- Resolve entry point ---
    // e_entry_raw is the virtual address from the ELF header.
    // For flat PIE binaries linked with -Ttext=0, e_entry == offset into binary.
    // For standard ELF, we use it as an offset from our PSRAM base.
    typedef int (*ElfMainFn)(void*);
    ElfMainFn entry;

    if (e_entry_raw == 0 || e_entry_raw >= file_size) {
        // Fallback: assume elf_main is at offset 0 of the binary
        // (valid for --entry elf_main with -Ttext=0 linked modules)
        entry = (ElfMainFn)base;
        Serial.println("[ELF] e_entry is 0 or out of range — using base address");
    } else {
        entry = (ElfMainFn)(base + e_entry_raw);
    }

    Serial.printf("[ELF] Loaded %u bytes to PSRAM @ 0x%08X, entry @ 0x%08X\n",
                  file_size, (unsigned)base, (unsigned)entry);

    // --- Execute ---
    _elf_internal::elf_running = true;
    int result = entry((void*)ctx);
    _elf_internal::elf_running = false;

    Serial.printf("[ELF] Module exited with code %d\n", result);

    // --- Cleanup ---
    elf_free_psram();

    return (result == 0) ? ELF_OK : ELF_EXEC_ERROR;
}

// ============================================================
//  elf_execute_manifest()
//  Validates ABI + PSRAM headroom before calling elf_execute().
// ============================================================
ElfLoadResult elf_execute_manifest(const ElfManifest* manifest, ElfContext* ctx) {
    if (!manifest || !manifest->valid) return ELF_NOT_FOUND;

    // ABI version check
    if (manifest->api_major > ELF_API_MAJOR_SUPPORTED) {
        Serial.printf("[ELF] ABI mismatch: module needs v%d, OS supports v%d\n",
                      manifest->api_major, ELF_API_MAJOR_SUPPORTED);
        return ELF_ABI_MISMATCH;
    }

    // PSRAM headroom check
    if (manifest->psram_kb > 0) {
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t needed     = (size_t)manifest->psram_kb * 1024;
        if (free_psram < needed) {
            Serial.printf("[ELF] PSRAM low: need %uKB, have %uKB free\n",
                          manifest->psram_kb, (unsigned)(free_psram / 1024));
            return ELF_NO_PSRAM;
        }
    }

    char full_path[96];
    snprintf(full_path, sizeof(full_path), "/apps/%s", manifest->elf_file);
    return elf_execute(full_path, ctx);
}

// ============================================================
//  elf_launch_from_browser()
//  File Browser hook — called from filesystem.cpp when the
//  user taps a .elf file in the file browser.
//
//  Returns ELF_NOT_FOUND immediately if path doesn't end in
//  ".elf" — the file browser uses this to decide whether to
//  handle the file normally or hand it to the ELF engine.
//
//  Shows a loading screen before SD read and a detailed error
//  screen on failure. Waits for keypress before returning to
//  the browser on error so the user can actually read it.
// ============================================================
ElfLoadResult elf_launch_from_browser(const char* full_sd_path) {
    if (!full_sd_path) return ELF_NOT_FOUND;

    // Verify .elf extension (case-insensitive)
    size_t len = strlen(full_sd_path);
    if (len < 5 || strcasecmp(full_sd_path + len - 4, ".elf") != 0) {
        return ELF_NOT_FOUND; // not our business — browser handles it
    }

    // Extract bare filename for manifest lookup
    const char* slash = strrchr(full_sd_path, '/');
    const char* bare  = slash ? slash + 1 : full_sd_path;

    // Try to load manifest sidecar — graceful if missing
    ElfManifest manifest;
    bool has_manifest = elf_load_manifest(bare, &manifest);
    const char* display_name = (has_manifest && manifest.name[0]) ? manifest.name : bare;

    // --- Loading screen ---
    // Matches the OS header-bar convention: 24px header, content below
    gfx->fillScreen(C_BLACK);
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(6, 7);
    gfx->print("ELF ON A SHELF");

    // PSRAM available readout in header right side
    size_t free_psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    gfx->setTextColor(C_GREY);
    gfx->setCursor(200, 7);
    gfx->printf("%uKB PSRAM", (unsigned)free_psram_kb);

    // App name (large)
    gfx->setTextColor(C_WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(8, 38);
    gfx->print(display_name);

    // Metadata line
    gfx->setTextSize(1);
    gfx->setTextColor(C_GREY);
    gfx->setCursor(8, 68);
    if (has_manifest && manifest.version[0]) {
        gfx->printf("v%s  %s  [%s]",
                    manifest.version,
                    manifest.author[0] ? manifest.author : "?",
                    manifest.category[0] ? manifest.category : "APPS");
    } else {
        gfx->print("No manifest — running with defaults");
    }

    // Path
    gfx->setCursor(8, 84);
    gfx->setTextColor(C_DARK);
    gfx->print(full_sd_path);

    // Status
    gfx->setCursor(8, 106);
    gfx->setTextColor(C_GREEN);
    gfx->print("Reading from SD...");

    // Render before the blocking SD read
    delay(60);

    // --- Build context and execute ---
    ElfContext ctx;
    elf_build_context(&ctx, nullptr); // No ROM — standalone app

    Serial.printf("[ELF] Browser launch: %s (%s)\n", full_sd_path, display_name);
    ElfLoadResult result = elf_execute(full_sd_path, &ctx);

    // --- Error screen ---
    if (result != ELF_OK) {
        gfx->fillScreen(C_BLACK);
        gfx->fillRect(0, 0, 320, 24, C_DARK);
        gfx->setTextColor(C_RED);
        gfx->setTextSize(1);
        gfx->setCursor(6, 7);
        gfx->print("ELF LOAD ERROR");

        gfx->setTextColor(C_WHITE);
        gfx->setTextSize(2);
        gfx->setCursor(8, 38);
        gfx->print(display_name);

        gfx->setTextSize(1);
        gfx->setTextColor(C_RED);
        gfx->setCursor(8, 68);
        gfx->printf("  %s", elf_result_str(result));

        gfx->setTextColor(C_GREY);
        gfx->setCursor(8, 88);
        switch (result) {
            case ELF_NO_PSRAM:
                gfx->printf("Need %uKB PSRAM — only %uKB free.",
                            has_manifest ? manifest.psram_kb : 1024,
                            (unsigned)free_psram_kb);
                break;
            case ELF_BAD_MAGIC:
                gfx->print("Not a valid ESP32-S3 ELF binary.");
                gfx->setCursor(8, 104);
                gfx->print("Compile with Xtensa toolchain + -fPIC.");
                break;
            case ELF_ABI_MISMATCH:
                gfx->printf("Module needs ELF API v%d. OS has v%d.",
                            has_manifest ? manifest.api_major : 0,
                            ELF_API_MAJOR_SUPPORTED);
                break;
            case ELF_NOT_FOUND:
                gfx->print("File missing from SD card.");
                break;
            case ELF_SD_ERROR:
                gfx->print("SD read failed. Check card.");
                break;
            default:
                gfx->print("Check serial monitor for details.");
                break;
        }

        gfx->setCursor(8, 130);
        gfx->setTextColor(C_GREEN);
        gfx->print("Any key / A button to return...");

        // Wait up to 6 seconds for a keypress before auto-returning
        uint32_t wait_start = millis();
        while (millis() - wait_start < 6000) {
            char k = get_keypress();
            if (k) break;
            gamepad_poll();
            if (gamepad_pressed(GP_A | GP_B | GP_START)) break;
            TrackballState tb = update_trackball();
            if (tb.clicked) break;
            delay(50);
        }
    }

    return result;
}