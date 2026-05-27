/**
 * PISCES MOON OS — OFFLINE HASH AUDITOR (Educational)
 * Demonstrates local dictionary attacks against SHA-256 hashes.
 * Uses mbedtls hardware acceleration on the ESP32-S3.
 * REQUIRES: A file named /dict.txt on the SD card.
 */

#include <Arduino.h>
#include <FS.h>
#include "SdFat.h"
#include <Arduino_GFX_Library.h>
#include "mbedtls/md.h"
#include "touch.h"
#include "trackball.h"

extern SdFat sd;
extern Arduino_GFX *gfx;

static String bytesToHex(const byte* bytes, size_t length) {
    String hexString = "";
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] < 0x10) hexString += "0";
        hexString += String(bytes[i], HEX);
    }
    return hexString;
}

static String generateSHA256(String payload) {
    byte shaResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;

    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(md_type), 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char *)payload.c_str(), payload.length());
    mbedtls_md_finish(&ctx, shaResult);
    mbedtls_md_free(&ctx);

    return bytesToHex(shaResult, 32);
}

void run_hash_auditor() {
    gfx->fillScreen(0x0000);
    gfx->fillRect(0, 0, 320, 24, 0x18C3);
    gfx->setCursor(10, 7);
    gfx->setTextColor(0x07E0);
    gfx->setTextSize(1);
    gfx->print("HASH AUDITOR | CLICK TO EXIT");

    String targetHash = "ef92b778bafe771e89245b89ecbc08a44a4e166c06659911881f383d4473e94f"; // "password123"
    
    gfx->setTextColor(0xFFFF);
    gfx->setCursor(10, 40);
    gfx->print("TARGET SHA-256:");
    gfx->setTextColor(0x07FF);
    gfx->setCursor(10, 55);
    gfx->print(targetHash.substring(0, 32));
    gfx->setCursor(10, 65);
    gfx->print(targetHash.substring(32));

    if (!sd.exists("/dict.txt")) {
        gfx->setTextColor(0xF800);
        gfx->setCursor(10, 100);
        gfx->print("ERROR: /dict.txt not found on SD!");
        delay(3000);
        return;
    }

    gfx->setTextColor(0xFFFF);
    gfx->setCursor(10, 100);
    gfx->print("Checking dictionary...");

    FsFile dictFile = sd.open("/dict.txt", O_READ);
    unsigned long startTime = millis();
    int attempts = 0;
    bool found = false;
    String crackedWord = "";

    gfx->fillRect(10, 120, 300, 60, 0x0000);

    while (dictFile.available()) {
        String word = dictFile.readStringUntil('\n');
        word.trim();
        
        if (word.length() == 0) continue;
        attempts++;

        if (attempts % 50 == 0) {
            gfx->fillRect(10, 120, 300, 20, 0x0000);
            gfx->setCursor(10, 120);
            gfx->setTextColor(0xFD20);
            gfx->printf("Trying: %s", word.c_str());
            yield();
        }

        if (generateSHA256(word).equalsIgnoreCase(targetHash)) {
            found = true;
            crackedWord = word;
            break;
        }

        TrackballState tb = update_trackball();
        if (tb.clicked) {
            dictFile.close();
            return;
        }
    }
    
    dictFile.close();
    unsigned long duration = millis() - startTime;
    float hashesPerSec = (attempts / (float)duration) * 1000.0;

    gfx->fillRect(10, 120, 300, 80, 0x0000);
    gfx->setCursor(10, 120);
    
    if (found) {
        gfx->setTextColor(0x07E0);
        gfx->setTextSize(2);
        gfx->print("MATCH FOUND!");
        gfx->setTextSize(1);
        gfx->setCursor(10, 145);
        gfx->setTextColor(0xFFFF);
        gfx->print("Password: ");
        gfx->setTextColor(0x07E0);
        gfx->print(crackedWord);
    } else {
        gfx->setTextColor(0xF800);
        gfx->setTextSize(2);
        gfx->print("NO MATCH");
    }

    gfx->setTextSize(1);
    gfx->setTextColor(0xC618);
    gfx->setCursor(10, 170);
    gfx->printf("Attempts: %d", attempts);
    gfx->setCursor(10, 185);
    gfx->printf("Time: %.2f sec", duration / 1000.0);
    gfx->setCursor(10, 200);
    gfx->printf("Speed: %.0f H/s", hashesPerSec);

    while (true) {
        TrackballState tb = update_trackball();
        if (tb.clicked) break;
        delay(50);
        yield();
    }
}