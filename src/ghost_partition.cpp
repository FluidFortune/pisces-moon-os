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
 * ghost_partition.cpp
 *
 * Implementation of dual-partition SD management, boot key detection,
 * PIN router, and Nuke sequence.
 *
 * When GHOST_PARTITION_ENABLED is not defined, all functions are stubs
 * that return safe defaults. No code paths change in the rest of the OS.
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
extern SemaphoreHandle_t spi_mutex;
#include <SPI.h>
#include <FS.h>
#include "SdFat.h"
#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#else
#include <Arduino_GFX_Library.h>
#endif
#include "ghost_partition.h"
#include "theme.h"
#include "keyboard.h"
#include "pm_input.h"
#include "touch.h"

// ─────────────────────────────────────────────
//  EXTERNAL DEPENDENCIES
// ─────────────────────────────────────────────
#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
#else
extern Arduino_GFX *gfx;
#endif
extern SdFat sd;            // Main public SdFat instance (Partition 1)

// ─────────────────────────────────────────────
//  INTERNAL STATE
// ─────────────────────────────────────────────
static PiscesMoonMode  _currentMode       = PM_MODE_NORMAL;
static bool            _bootKeyDetected   = false;

#ifdef GHOST_PARTITION_ENABLED

static SdFat           _sdGhost;           // Partition 2 handle
static bool            _ghostMounted      = false;

// PIN lockout state — millis()-based, non-blocking
// Respects the SPI Bus Treaty: no delay() calls here
static unsigned long   _lockoutEnd        = 0;
static int             _attemptCount      = 0;

// ─────────────────────────────────────────────
//  INTERNAL: PIN VERIFICATION
//  Simple string compare. PINs are stored in
//  security_config.h which is .gitignored.
//  Not cryptographic — this is OPSEC through
//  obscurity + hardware key combo, not AES.
// ─────────────────────────────────────────────
static bool _verifyPin(const String& input, const char* target) {
    return input.equals(target);
}

// ─────────────────────────────────────────────
//  INTERNAL: MOUNT GHOST PARTITION (Partition 2)
//  Uses SdFat 2.2.3's explicit partition index.
//  Returns true on success, false if card has
//  only one partition or format is wrong.
// ─────────────────────────────────────────────
static bool _mountGhost(uint8_t csPin, SPIClass& spi) {
    if (_ghostMounted) return true;

    // SdFat 2.2.3: begin() with SdSpiConfig, then select partition via vol()->begin()
    SdSpiConfig cfg(csPin, SHARED_SPI, SD_SCK_MHZ(10), &spi);
    if (!_sdGhost.begin(cfg)) {
        Serial.println("[GHOST] Partition 2 card init failed.");
        _ghostMounted = false;
        return false;
    }
    // Select partition 2 explicitly on the already-initialized card
    if (!_sdGhost.vol()->begin(_sdGhost.card(), true, PARTITION_GHOST)) {
        Serial.println("[GHOST] Partition 2 volume select failed — single partition card?");
        _ghostMounted = false;
        return false;
    }
    _ghostMounted = true;
    Serial.println("[GHOST] Partition 2 mounted — Tactical Mode armed.");
    return true;
}

// ─────────────────────────────────────────────
//  INTERNAL: MFA UNLOCK SCREEN
//  Shown after correct Primary PIN entry.
//  Prompts for hidden unlock key — no hint given.
//  Wrong key silently routes to Student Mode.
//  Observer cannot tell which key is correct.
// ─────────────────────────────────────────────
static void _drawMFAScreen() {
    gfx->fillScreen(C_BLACK);

    // Read actual display dimensions. T-Deck Plus = 320x240;
    // T-LoraPager = 480x222; Cardputer ADV = 240x135.
    const int W = gfx->width();
    const int H = gfx->height();
    const int CX = W / 2;

    // Header bar — full width, fixed 26px tall (matches _drawPinScreen)
    gfx->fillRect(0, 0, W, 26, 0x0841);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(10, 8);
    gfx->print("PISCES MOON OS  //  SECURE BOOT");

    // Lock icon — open (PIN accepted) — centered horizontally
    int lx = CX - 12;
    int ly = 60;
    gfx->fillRect(lx,     ly,      24, 18, 0x4208);
    gfx->drawRect(lx,     ly,      24, 18, C_GREEN);
    gfx->fillRect(lx + 8, ly + 6,  8,  8,  C_BLACK);
    // Shackle open — drawn offset to show unlocked state
    gfx->drawRect(lx + 4, ly - 14, 16, 14, C_GREEN);
    gfx->fillRect(lx + 6, ly - 12, 12, 8,  C_BLACK);

    // Title — centered horizontally
    gfx->setTextSize(2);
    gfx->setTextColor(C_CYAN);
    const char* prompt = "PRESS UNLOCK KEY";
    int promptW = (int)strlen(prompt) * 12;  // 12px per char at size 2
    gfx->setCursor(CX - promptW / 2, 95);
    gfx->print(prompt);

    // Dim instruction — no hint about which key
    gfx->setTextSize(1);
    gfx->setTextColor(0x4208);
    const char* hint = "PIN accepted. Key required.";
    int hintW = (int)strlen(hint) * 6;
    gfx->setCursor(CX - hintW / 2, 130);
    gfx->print(hint);

    // Footer — clamp Y so it stays visible on shorter Cardputer screen
    int footerY = (H > 220) ? 210 : H - 16;
    gfx->setTextColor(0x4208);
    gfx->setCursor(10, footerY);
    gfx->print("Wrong key loads safe mode.");
}

// ─────────────────────────────────────────────
//  INTERNAL: DRAW PIN SCREEN
// ─────────────────────────────────────────────
static void _drawPinScreen(const String& entered, const String& statusMsg, uint16_t statusColor) {
    gfx->fillScreen(C_BLACK);

    // Read actual display dimensions. T-Deck Plus = 320x240; T-LoraPager = 480x222.
    const int W = gfx->width();
    const int H = gfx->height();
    const int CX = W / 2;

    // Header bar — full width, fixed 26px tall
    gfx->fillRect(0, 0, W, 26, 0x0841);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(10, 8);
    gfx->print("PISCES MOON OS  //  SECURE BOOT");

    // Lock icon (simple pixel art padlock) — centered horizontally,
    // positioned just below the header bar.
    int lx = CX - 12;
    int ly = 50;
    gfx->fillRect(lx,      ly,      24, 18, 0x4208);   // Body
    gfx->drawRect(lx,      ly,      24, 18, C_GREEN);
    gfx->fillRect(lx + 8,  ly + 6,  8,  8,  C_BLACK);  // Keyhole
    gfx->drawRect(lx + 4,  ly - 10, 16, 14, C_GREEN);  // Shackle outer
    gfx->fillRect(lx + 6,  ly - 10, 12, 12, C_BLACK);  // Shackle inner

    // Title — "ENTER PIN" centered, below lock
    gfx->setTextSize(2);
    gfx->setTextColor(C_WHITE);
    const int titleW = 9 * 12;  // 9 chars * 12px (size-2 char width)
    gfx->setCursor(CX - titleW / 2, ly + 28);
    gfx->print("ENTER PIN");

    // PIN dots display — up to 8 dots, centered horizontally
    const int dotCount = 8;
    const int dotSpacing = 18;
    const int dotsRowY = ly + 65;
    int dotStartX = CX - ((dotCount * dotSpacing) / 2) + (dotSpacing / 2);
    for (int i = 0; i < dotCount; i++) {
        uint16_t dotColor = (i < (int)entered.length()) ? C_GREEN : 0x2104;
        gfx->fillCircle(dotStartX + (i * dotSpacing), dotsRowY, 6, dotColor);
    }

    // Input echo (masked) — shows length only, not characters
    gfx->setTextSize(1);
    gfx->setTextColor(0x4208);
    gfx->setCursor(10, dotsRowY + 18);
    gfx->print("Length: ");
    gfx->setTextColor(C_CYAN);
    gfx->print(entered.length());
    gfx->print(" chars");

    // Status message (errors, lockout countdown) — centered
    if (statusMsg.length() > 0) {
        gfx->setTextSize(1);
        gfx->setTextColor(statusColor);
        int sx = CX - (statusMsg.length() * 6) / 2;
        if (sx < 4) sx = 4;
        gfx->setCursor(sx, dotsRowY + 34);
        gfx->print(statusMsg);
    }

    // Instructions — anchored to bottom of screen with safe margin
    int instrY = H - 14;
    gfx->setTextColor(0x4208);
    gfx->setCursor(6, instrY);
    gfx->print("ENTER=confirm  BKSP=clear  Q=off");

    // Attempt counter (dim, bottom right) — only if attempts > 0
    if (_attemptCount > 0) {
        gfx->setTextColor(0xF800);
        int attemptW = 14 * 6;  // ~14 chars * 6px (size-1 char width)
        gfx->setCursor(W - attemptW - 4, instrY - 12);
        gfx->printf("Attempts: %d/%d", _attemptCount, PIN_MAX_ATTEMPTS);
    }
}

// ─────────────────────────────────────────────
//  INTERNAL: MFA UNLOCK-KEY PROMPT
//  v1.2.1 — restored after v1.1 removal.
//
//  After a correct Primary PIN, this prompt appears asking the
//  user to "PRESS UNLOCK KEY". Only the configured UNLOCK_KEY
//  character returns true. ANY other key returns false silently,
//  with no on-screen indication of failure — the caller then
//  loads Student Mode as if a Decoy PIN had been entered.
//
//  Design rationale:
//    - The unlock key is the second factor in two-factor unlock.
//    - It does not appear on any device label or in any visible
//      hint. Operator must memorize it.
//    - Silent failure is intentional: an observer watching the
//      device cannot tell from the UI whether the user just
//      entered a Primary PIN + wrong key (Student Mode) or
//      a Decoy PIN (also Student Mode). Both paths produce the
//      identical visible outcome.
//    - Q (exit/abort) also returns false — same as wrong key.
// ─────────────────────────────────────────────
static bool _promptUnlockKey() {
    // Use the existing MFA screen renderer (now responsive across
    // T-Deck Plus, T-LoRa Pager, and Cardputer ADV).
    _drawMFAScreen();

    // Block until a single keypress arrives.
    // No backspace, no retry — one key, one decision.
    while (true) {
        char c = get_keypress();
        if (c == 0) {
            delay(15);
            yield();
            continue;
        }
        // Any non-zero key resolves the prompt.
        // Match against UNLOCK_KEY; anything else (including Q and Enter)
        // counts as a failed MFA and silently falls back to Student Mode.
        Serial.printf("[MFA-DIAG] received char=0x%02X expected=0x%02X match=%s\n",
                      (unsigned char)c, (unsigned char)UNLOCK_KEY,
                      (c == UNLOCK_KEY) ? "yes" : "no");
        return (c == UNLOCK_KEY);
    }
}

// ─────────────────────────────────────────────
//  INTERNAL: WIPE WARDRIVE LOGS ON A GIVEN PARTITION
//  v1.2.1 — dynamic scan replaces hardcoded list.
//  Wardrive writes /wardrive_NNNN.csv at the root of
//  whichever partition is active. Currently that's the
//  PUBLIC partition (sd), but the nuke wipes both so
//  the behavior is correct if/when wardrive is moved
//  to the Ghost partition in a future release.
//
//  Returns the number of files removed.
// ─────────────────────────────────────────────
static int _nukeWardriveLogsOn(SdFat& volume, const char* label) {
    int wiped = 0;
    char path[32];

    // Iterate the full 1..9999 session-number space.
    // Each existence check is a single metadata read — fast and
    // SPI-bus safe. Misses (file doesn't exist) cost essentially
    // nothing on a FAT32 volume.
    for (int n = 1; n <= WARDRIVE_LOG_MAX_SESSION; n++) {
        snprintf(path, sizeof(path), "%s%04d%s",
                 WARDRIVE_LOG_PREFIX, n, WARDRIVE_LOG_SUFFIX);
        if (volume.exists(path)) {
            if (volume.remove(path)) {
                wiped++;
            } else {
                Serial.printf("[GHOST] Failed to remove %s on %s\n",
                              path, label);
            }
        }
    }
    return wiped;
}

// ─────────────────────────────────────────────
//  INTERNAL: NUKE SEQUENCE
//  v1.2.1 — wipes Ghost Partition NoSQL indexes AND
//  scans BOTH partitions for wardrive CSV logs.
//
//  This is a LOGICAL wipe — files are removed from the
//  filesystem, making them invisible to the OS. Physical
//  NAND blocks containing the data persist until the SD
//  card's wear-leveling controller reuses them. For threat
//  models requiring guaranteed unrecoverability, physical
//  destruction of the SD card is required.
//
//  Fast metadata ops — no SPI bus stall.
//  Does NOT format the partition (that would lock the bus
//  and violate the SPI Bus Treaty).
// ─────────────────────────────────────────────
static void _executeNuke() {
    gfx->fillScreen(C_BLACK);
    gfx->setTextColor(0xF800);
    gfx->setTextSize(2);
    gfx->setCursor(40, 80);
    gfx->print("SECURE WIPE");
    gfx->setTextSize(1);
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 120);
    gfx->print("Removing tactical data...");

    int removed = 0;

    // ── 1. Ghost Partition NoSQL index files ──
    // Removing the index renders the entire category invisible
    // to the OS even though raw entries may persist as orphans.
    for (int i = 0; i < NUKE_TARGETS_COUNT; i++) {
        if (_sdGhost.exists(NUKE_INDEX_FILES[i])) {
            _sdGhost.remove(NUKE_INDEX_FILES[i]);
            removed++;
            gfx->print(".");
        }
    }

    // ── 2. Wardrive CSVs on the Ghost Partition ──
    // Future-proofing — if wardrive is moved to sdGhost in a
    // later release, this catches its logs there.
    removed += _nukeWardriveLogsOn(_sdGhost, "GHOST");
    gfx->print(".");

    // ── 3. Wardrive CSVs on the Public Partition ──
    // Current wardrive writes /wardrive_NNNN.csv to sd (Public).
    // This is the primary wipe target until wardrive is migrated.
    removed += _nukeWardriveLogsOn(sd, "PUBLIC");
    gfx->print(".");

    gfx->setCursor(10, 145);
    gfx->setTextColor(C_GREEN);
    gfx->printf("Done. %d items cleared.", removed);
    delay(800);

    // Unmount Ghost — it's now empty to the OS
    _sdGhost.end();
    _ghostMounted = false;

    Serial.printf("[GHOST] Nuke complete. %d items removed.\n", removed);
}

// ─────────────────────────────────────────────
//  INTERNAL: LOAD TACTICAL MODE
//  Mounts Ghost Partition, sets mode, starts
//  Ghost Engine (wardrive background task).
// ─────────────────────────────────────────────
static void _loadTacticalMode() {
    _currentMode = PM_MODE_TACTICAL;

    gfx->fillScreen(C_BLACK);
    gfx->setTextColor(C_GREEN);
    gfx->setTextSize(1);
    gfx->setCursor(10, 100);
    gfx->print("TACTICAL MODE — MOUNTING GHOST PARTITION");

    // Ghost Partition is already mounted at this point (detected at boot)
    // Just confirm and proceed
    gfx->setCursor(10, 120);
    gfx->setTextColor(C_CYAN);
    gfx->print("Ghost Partition: ACTIVE");
    gfx->setCursor(10, 135);
    gfx->print("Ghost Engine: ARMED");
    delay(600);

    Serial.println("[GHOST] Tactical Mode active. All systems go.");
}

// ─────────────────────────────────────────────
//  INTERNAL: LOAD STUDENT MODE
//  Partition 2 stays unmounted. Only safe apps
//  will appear in the launcher via mode check.
// ─────────────────────────────────────────────
static void _loadStudentMode() {
    _currentMode = PM_MODE_STUDENT;

    // Ensure Ghost is unmounted if it was briefly mounted for detection
    if (_ghostMounted) {
        _sdGhost.end();
        _ghostMounted = false;
    }

    Serial.println("[GHOST] Student Mode active. Ghost Partition unmounted.");
}

#endif // GHOST_PARTITION_ENABLED

// ═════════════════════════════════════════════
//  PUBLIC API IMPLEMENTATION
// ═════════════════════════════════════════════

// ─────────────────────────────────────────────
//  ghost_partition_check_boot_keys()
//  Call after init_trackball(), before SD mount.
// ─────────────────────────────────────────────
void ghost_partition_check_boot_keys() {
#ifdef GHOST_PARTITION_ENABLED
    // Key combo removed — PIN screen always appears when Ghost Partition is enabled.
    // PIN itself is the authentication layer. Physical MFA can be added later.
    _bootKeyDetected = true;
    Serial.println("[GHOST] Ghost Partition enabled — PIN screen will appear after splash.");
#endif
}

// ─────────────────────────────────────────────
//  ghost_partition_mount_public()
//  Drop-in replacement for sd.begin() in main.cpp
// ─────────────────────────────────────────────
bool ghost_partition_mount_public(uint8_t csPin, SPIClass& spi) {
    // Always mount Partition 1 as the main 'sd' object
    SdSpiConfig cfg(csPin, SHARED_SPI, SD_SCK_MHZ(10), &spi);

#ifdef GHOST_PARTITION_ENABLED
    // Mount with SdSpiConfig — standard single begin() call
    // SPI Bus Treaty — hold the shared mutex during SD card
    // initialization so display/LoRa/NFC don't collide.
    if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
    bool ok = sd.begin(cfg);
    if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
    if (!ok) {
        Serial.println("[SD] Public partition (P1) mount failed.");
        return false;
    }
    // Select partition 1 explicitly
    if (!sd.vol()->begin(sd.card(), true, PARTITION_PUBLIC)) {
        Serial.println("[SD] Public partition (P1) volume select failed.");
        return false;
    }
    Serial.println("[SD] Public partition (P1) mounted.");

    // Always attempt Ghost Partition mount when enabled
    Serial.println("[GHOST] Attempting Ghost Partition detection...");
    if (!_mountGhost(csPin, spi)) {
        // No Partition 2 — fall back silently to Normal Mode
        _bootKeyDetected = false;
        Serial.println("[GHOST] No Ghost Partition found — falling back to Normal Mode.");
    }
    return true;

#else
    // Standard single-partition mount — identical to original main.cpp behavior
    if (spi_mutex) xSemaphoreTakeRecursive(spi_mutex, portMAX_DELAY);
    bool ok2 = sd.begin(cfg);
    if (spi_mutex) xSemaphoreGiveRecursive(spi_mutex);
    return ok2;
#endif
}

// ─────────────────────────────────────────────
//  ghost_partition_run_pin_screen()
//  Call after showRainbowSplash() in main.cpp.
//  No-op if Ghost Partition not enabled or
//  boot key combo was not detected.
// ─────────────────────────────────────────────
void ghost_partition_run_pin_screen() {
#ifdef GHOST_PARTITION_ENABLED
    // Always show PIN screen when Ghost Partition is enabled.
    // If Ghost Partition card not found, fall back to Normal Mode.
    if (!_ghostMounted) {
        _currentMode = PM_MODE_NORMAL;
        Serial.println("[GHOST] No Ghost Partition — booting Normal Mode.");
        return;
    }

    // ── Hard screen clear before PIN screen draws ──
    // The splash screen leaves residual content on the display.
    // fillScreen alone is insufficient — the display needs a brief
    // settle period after the splash animation before the PIN screen
    // renders cleanly. Without this, splash content bleeds through
    // until the first keypress triggers a full redraw.
#ifdef DEVICE_TDECK_PLUS
    gfx->setRotation(1);       // T-Deck's known landscape render state.
#endif
    gfx->fillScreen(0x0000);
    gfx->setCursor(0, 0);       // Reset cursor to origin
    gfx->setTextSize(1);        // Reset text size
    delay(150);                 // Allow display to settle
    gfx->fillScreen(0x0000);   // Second clear for belt-and-suspenders

    // ── PIN Entry Loop ──
    String entered = "";
    String statusMsg = "";
    uint16_t statusColor = C_WHITE;

    _drawPinScreen(entered, statusMsg, statusColor);

    while (true) {
        // ── Lockout Check (non-blocking, millis-based) ──
        if (_attemptCount >= PIN_MAX_ATTEMPTS && millis() < _lockoutEnd) {
            unsigned long remaining = (_lockoutEnd - millis()) / 1000;
            statusMsg = "Locked. Wait " + String(remaining) + "s";
            statusColor = 0xF800; // Red
            _drawPinScreen(entered, statusMsg, statusColor);
            delay(500);
            continue;
        }
        if (_attemptCount >= PIN_MAX_ATTEMPTS && millis() >= _lockoutEnd) {
            // Lockout expired — reset
            _attemptCount = 0;
            statusMsg = "Enter PIN";
            statusColor = C_WHITE;
            _drawPinScreen(entered, statusMsg, statusColor);
        }

        // ── Key Input ──
        char c = get_keypress();

        if (c == 0) {
            delay(15);
            yield();
            continue;
        }

        // DIAGNOSTIC — remove once PIN entry is confirmed working
        Serial.printf("[PIN-DIAG] received char=0x%02X (%d) '%c'  entered='%s' len=%d\n",
                      (unsigned char)c, (int)(unsigned char)c,
                      (c >= 32 && c <= 126) ? c : '?',
                      entered.c_str(), entered.length());

        if (c == 13 || c == 10) {
            Serial.printf("[PIN-DIAG] ENTER pressed. entered.length()=%d, entered='%s'\n",
                          entered.length(), entered.c_str());
            // ENTER — evaluate PIN
            if (entered.length() == 0) continue;

            if (_verifyPin(entered, PRIMARY_PIN)) {
                // ── PRIMARY PIN: MFA + Tactical Mode ──
                // v1.2.1 — MFA unlock-key restored.
                // Correct PIN brings up the "PRESS UNLOCK KEY" prompt.
                // The correct UNLOCK_KEY loads Tactical Mode.
                // Any other key silently falls back to Student Mode —
                // visually indistinguishable from a Decoy PIN entry.
                _attemptCount = 0;
                if (_promptUnlockKey()) {
                    _loadTacticalMode();
                } else {
                    _loadStudentMode();
                }
                return;

            } else if (_verifyPin(entered, DECOY_PIN)) {
                // ── DECOY PIN: Student Mode ──
                _attemptCount = 0;
                _loadStudentMode();
                return;

            } else if (_verifyPin(entered, NUKE_PIN)) {
                // ── NUKE PIN: Wipe Ghost, then Student Mode ──
                _attemptCount = 0;
                _executeNuke();
                _loadStudentMode();
                return;

            } else {
                // ── WRONG PIN ──
                _attemptCount++;
                entered = "";
                if (_attemptCount >= PIN_MAX_ATTEMPTS) {
                    _lockoutEnd = millis() + PIN_LOCKOUT_MS;
                    statusMsg = "Brute force detected. Cooling down...";
                    statusColor = 0xF800;
                    Serial.println("[GHOST] PIN lockout engaged.");
                } else {
                    statusMsg = "Incorrect. " + String(PIN_MAX_ATTEMPTS - _attemptCount) + " attempt(s) left.";
                    statusColor = 0xFD20; // Orange
                }
                _drawPinScreen(entered, statusMsg, statusColor);
            }

        } else if (c == 8 || c == 127) {
            // BACKSPACE
            if (entered.length() > 0) {
                entered.remove(entered.length() - 1);
                _drawPinScreen(entered, statusMsg, statusColor);
            }

        } else if (pm_is_exit_key(c)) {
            // Q = Power off / abort (enters Student mode as safe fallback)
            _loadStudentMode();
            return;

        } else if (c >= 32 && c <= 126) {
            // Printable character — add to PIN buffer (max 32 chars)
            if (entered.length() < 32) {
                entered += c;
                _drawPinScreen(entered, statusMsg, statusColor);
            }
        }

        delay(15);
        yield();
    }
#endif // GHOST_PARTITION_ENABLED
    // If not enabled: _currentMode stays PM_MODE_NORMAL, function returns immediately
}

// ─────────────────────────────────────────────
//  ghost_partition_get_mode()
// ─────────────────────────────────────────────
PiscesMoonMode ghost_partition_get_mode() {
    return _currentMode;
}

// ─────────────────────────────────────────────
//  ghost_partition_is_tactical()
// ─────────────────────────────────────────────
bool ghost_partition_is_tactical() {
    return (_currentMode == PM_MODE_TACTICAL);
}

// ─────────────────────────────────────────────
//  ghost_partition_get_sd()
//  Returns nullptr if not in Tactical Mode.
//  Always null-check before use.
// ─────────────────────────────────────────────
SdFat* ghost_partition_get_sd() {
#ifdef GHOST_PARTITION_ENABLED
    if (_currentMode == PM_MODE_TACTICAL && _ghostMounted) {
        return &_sdGhost;
    }
#endif
    return nullptr;
}

// ─────────────────────────────────────────────
//  ghost_partition_unmount()
// ─────────────────────────────────────────────
void ghost_partition_unmount() {
#ifdef GHOST_PARTITION_ENABLED
    if (_ghostMounted) {
        _sdGhost.end();
        _ghostMounted = false;
        Serial.println("[GHOST] Ghost Partition unmounted.");
    }
#endif
}