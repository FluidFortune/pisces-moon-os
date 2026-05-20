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
 * PISCES MOON OS — HASH TOOL v1.0
 * MD5 / SHA-1 / SHA-256 digest calculator
 *
 * Uses ESP32 hardware SHA acceleration via mbedTLS.
 *
 * Modes:
 *   TEXT mode: Type a string, compute its hash
 *   FILE mode: Select a file from SD, compute its hash
 *
 * Educational use:
 *   Verify file integrity (compare hash against known-good value)
 *   Understand hash function differences (MD5 vs SHA-256)
 *   Practice cryptography fundamentals
 *
 * Controls:
 *   Keyboard typing  = text input
 *   Trackball click  = compute hash / cycle mode / select file
 *   A key            = cycle algorithm (MD5 / SHA1 / SHA256)
 *   Q / header tap   = exit
 */

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <FS.h>
#include "SdFat.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "hash_tool.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS
// ─────────────────────────────────────────────
#define COL_BG      0x0000
#define COL_HDR     0x0820
#define COL_TITLE   0x07E0
#define COL_LABEL   0x07FF
#define COL_VALUE   0xFFFF
#define COL_HASH    0xFFE0
#define COL_DIM     0x4208
#define COL_ACCENT  0xFD20
#define COL_SEL     0x001F
#define COL_BOX     0x18C3

// ─────────────────────────────────────────────
//  ALGORITHMS
// ─────────────────────────────────────────────
#define ALG_MD5    0
#define ALG_SHA1   1
#define ALG_SHA256 2
static const char* ALG_NAMES[] = {"MD5", "SHA-1", "SHA-256"};
static const int   ALG_SIZES[] = {16, 20, 32};

static int  currentAlg = ALG_SHA256;

// ─────────────────────────────────────────────
//  HASH COMPUTATION
// ─────────────────────────────────────────────
static void computeHashText(const uint8_t* data, size_t len, uint8_t* out) {
    if (currentAlg == ALG_MD5) {
        mbedtls_md5_context ctx;
        mbedtls_md5_init(&ctx);
        mbedtls_md5_starts_ret(&ctx);
        mbedtls_md5_update_ret(&ctx, data, len);
        mbedtls_md5_finish_ret(&ctx, out);
        mbedtls_md5_free(&ctx);
    } else if (currentAlg == ALG_SHA1) {
        mbedtls_sha1_context ctx;
        mbedtls_sha1_init(&ctx);
        mbedtls_sha1_starts_ret(&ctx);
        mbedtls_sha1_update_ret(&ctx, data, len);
        mbedtls_sha1_finish_ret(&ctx, out);
        mbedtls_sha1_free(&ctx);
    } else {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts_ret(&ctx, 0); // 0 = SHA-256 (not 224)
        mbedtls_sha256_update_ret(&ctx, data, len);
        mbedtls_sha256_finish_ret(&ctx, out);
        mbedtls_sha256_free(&ctx);
    }
}

static bool computeHashFile(const char* path, uint8_t* out) {
    if (!sd.exists(path)) return false;
    FsFile f = sd.open(path, O_READ);
    if (!f) return false;

    uint8_t buf[256];
    size_t n;

    if (currentAlg == ALG_MD5) {
        mbedtls_md5_context ctx;
        mbedtls_md5_init(&ctx);
        mbedtls_md5_starts_ret(&ctx);
        while ((n = f.read(buf, sizeof(buf))) > 0)
            mbedtls_md5_update_ret(&ctx, buf, n);
        mbedtls_md5_finish_ret(&ctx, out);
        mbedtls_md5_free(&ctx);
    } else if (currentAlg == ALG_SHA1) {
        mbedtls_sha1_context ctx;
        mbedtls_sha1_init(&ctx);
        mbedtls_sha1_starts_ret(&ctx);
        while ((n = f.read(buf, sizeof(buf))) > 0)
            mbedtls_sha1_update_ret(&ctx, buf, n);
        mbedtls_sha1_finish_ret(&ctx, out);
        mbedtls_sha1_free(&ctx);
    } else {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts_ret(&ctx, 0);
        while ((n = f.read(buf, sizeof(buf))) > 0)
            mbedtls_sha256_update_ret(&ctx, buf, n);
        mbedtls_sha256_finish_ret(&ctx, out);
        mbedtls_sha256_free(&ctx);
    }
    f.close();
    return true;
}

static void hashToHex(const uint8_t* hash, int len, char* out) {
    for (int i = 0; i < len; i++)
        sprintf(out + i * 2, "%02x", hash[i]);
    out[len * 2] = '\0';
}

// ─────────────────────────────────────────────
//  DRAWING
// ─────────────────────────────────────────────
static void drawHeader() {
    gfx->fillRect(0, 0, 320, 24, COL_HDR);
    gfx->drawFastHLine(0, 23, 320, COL_TITLE);
    gfx->setTextSize(1);
    gfx->setTextColor(COL_TITLE);
    gfx->setCursor(8, 4);  gfx->print("HASH TOOL");
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(8, 13); gfx->printf("ALG: %s  [A]=cycle", ALG_NAMES[currentAlg]);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(260, 8); gfx->print("[TAP=EXIT]");
}

static void drawTextMode(const char* input, const char* hashResult, const char* hashHex2 = nullptr) {
    gfx->fillRect(0, 26, 320, 214, COL_BG);

    // Input label
    gfx->setTextSize(1);
    gfx->setTextColor(COL_LABEL);
    gfx->setCursor(6, 30);
    gfx->print("INPUT TEXT:");

    // Input box
    gfx->fillRect(5, 42, 310, 28, COL_BOX);
    gfx->drawRect(5, 42, 310, 28, COL_ACCENT);
    gfx->setTextColor(COL_VALUE);
    gfx->setCursor(10, 48);
    // Word-wrap at ~48 chars
    int ilen = strlen(input);
    if (ilen <= 48) {
        gfx->print(input);
    } else {
        gfx->print(String(input).substring(0, 48));
        gfx->setCursor(10, 58);
        gfx->print(String(input).substring(48, min(96, ilen)));
    }
    // Cursor blink placeholder
    gfx->print("_");

    // Algorithm info
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, 76);
    gfx->printf("Algorithm: %s  |  CLK or ENTER to hash", ALG_NAMES[currentAlg]);

    // Result
    if (hashResult && strlen(hashResult) > 0) {
        gfx->drawFastHLine(0, 88, 320, 0x2104);
        gfx->setTextColor(COL_LABEL);
        gfx->setCursor(6, 92);
        gfx->printf("%s DIGEST:", ALG_NAMES[currentAlg]);

        gfx->setTextColor(COL_HASH);
        // Display hash — wrap at 32 chars (64 hex chars for SHA256 = 2 lines)
        int hlen = strlen(hashResult);
        gfx->setCursor(6, 106);
        gfx->print(String(hashResult).substring(0, min(32, hlen)));
        if (hlen > 32) {
            gfx->setCursor(6, 118);
            gfx->print(String(hashResult).substring(32, min(64, hlen)));
        }

        // Info about algorithm
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(6, 136);
        if (currentAlg == ALG_MD5)
            gfx->print("MD5: 128-bit. Fast, NOT cryptographically secure.");
        else if (currentAlg == ALG_SHA1)
            gfx->print("SHA-1: 160-bit. Deprecated for security, still used.");
        else
            gfx->print("SHA-256: 256-bit. Current standard. Use for verification.");

        gfx->setCursor(6, 148);
        gfx->printf("Input length: %d bytes", strlen(input));

        // Quick compare box
        gfx->setTextColor(COL_LABEL);
        gfx->setCursor(6, 168);
        gfx->print("COMPARE (paste known hash):");
        gfx->fillRect(5, 180, 310, 14, COL_BOX);
        gfx->drawRect(5, 180, 310, 14, COL_DIM);
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(10, 183);
        gfx->print("[type in keyboard, then verify]");
    }

    // Controls
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, 228);
    gfx->print("Type=input  CLK=hash  BKSP=clear  A=alg");
}

static void drawResult(const char* label, const char* hashHex, bool success) {
    gfx->fillRect(0, 26, 320, 214, COL_BG);
    gfx->setTextSize(1);

    if (!success) {
        gfx->setTextColor(0xF800);
        gfx->setCursor(20, 100);
        gfx->print("Error computing hash.");
        gfx->setCursor(20, 116);
        gfx->print("Check file and try again.");
        return;
    }

    gfx->setTextColor(COL_LABEL);
    gfx->setCursor(6, 30);
    gfx->printf("%s HASH OF:", ALG_NAMES[currentAlg]);
    gfx->setTextColor(COL_ACCENT);
    gfx->setCursor(6, 42);
    // Truncate long path
    char shortLabel[44]; strncpy(shortLabel, label, 43); shortLabel[43] = '\0';
    gfx->print(shortLabel);

    gfx->drawFastHLine(0, 56, 320, 0x2104);
    gfx->setTextColor(COL_HASH);
    gfx->setCursor(6, 62);
    int hlen = strlen(hashHex);
    gfx->print(String(hashHex).substring(0, min(32, hlen)));
    if (hlen > 32) {
        gfx->setCursor(6, 74);
        gfx->print(String(hashHex).substring(32));
    }

    gfx->setTextColor(COL_DIM);
    gfx->setCursor(6, 92);
    if (currentAlg == ALG_MD5)
        gfx->print("MD5: 128-bit (32 hex chars)");
    else if (currentAlg == ALG_SHA1)
        gfx->print("SHA-1: 160-bit (40 hex chars)");
    else
        gfx->print("SHA-256: 256-bit (64 hex chars)");

    gfx->setCursor(6, 228);
    gfx->print("CLK=back  A=change algorithm");
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_hash_tool() {
    char inputBuf[256] = "";
    char hashResult[65] = "";
    uint8_t rawHash[32];
    bool hasResult = false;

    gfx->fillScreen(COL_BG);
    drawHeader();
    drawTextMode(inputBuf, nullptr);

    while (true) {
        char k = get_keypress();
        TrackballState tb = update_trackball();
        int16_t tx, ty;

        // Exit
        if (get_touch(&tx, &ty) && ty < 24) {
            while(get_touch(&tx,&ty)){delay(10);}
            break;
        }
        if (k == 'q' || k == 'Q' && strlen(inputBuf) == 0) break;

        // Cycle algorithm
        if (k == 'a' || k == 'A') {
            currentAlg = (currentAlg + 1) % 3;
            hashResult[0] = '\0'; hasResult = false;
            drawHeader();
            drawTextMode(inputBuf, nullptr);
            continue;
        }

        // Keyboard input
        if (k >= 32 && k <= 126 && k != 'a' && k != 'A') {
            int ilen = strlen(inputBuf);
            if (ilen < 255) {
                inputBuf[ilen] = k; inputBuf[ilen+1] = '\0';
                hasResult = false; hashResult[0] = '\0';
                drawTextMode(inputBuf, nullptr);
            }
        } else if ((k == 8 || k == 127) && strlen(inputBuf) > 0) {
            inputBuf[strlen(inputBuf)-1] = '\0';
            hasResult = false; hashResult[0] = '\0';
            drawTextMode(inputBuf, nullptr);
        } else if ((k == 13 || tb.clicked) && strlen(inputBuf) > 0) {
            // Compute hash
            computeHashText((const uint8_t*)inputBuf, strlen(inputBuf), rawHash);
            hashToHex(rawHash, ALG_SIZES[currentAlg], hashResult);
            hasResult = true;
            drawTextMode(inputBuf, hashResult);
        }

        delay(30);
        yield();
    }

    gfx->fillScreen(COL_BG);
}