// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
// REWRITTEN for width-aware rendering (480x240 T-LoRa Pager / 320x240 T-Deck Plus)

#ifdef DEVICE_TLORAPAGER
#include "pm_disp_tlorapager.h"
#include <Wire.h>
#else
#include <Arduino_GFX_Library.h>
#endif
#include "keyboard.h"
#include "pm_input.h"
#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "touch.h"
#include "theme.h"
#include "apps.h"
#include <XPowersLib.h>

#ifdef DEVICE_TLORAPAGER
extern PMDispTLoRaPager *gfx;
static constexpr int DISP_W = 480;
static constexpr int DISP_H = 222;
#elif defined(DEVICE_CARDPUTER_ADV)
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 240;
static constexpr int DISP_H = 135;
#else
extern Arduino_GFX *gfx;
static constexpr int DISP_W = 320;
static constexpr int DISP_H = 240;
#endif
extern XPowersAXP2101 PMU;
extern SdFat sd;
extern SemaphoreHandle_t spi_mutex;

static int estimateBatteryPercent(float volts) {
    if (volts >= 4.20f) return 100;
    if (volts <= 3.30f) return 0;
    return (int)((volts - 3.30f) * 100.0f / 0.90f);
}

#ifdef DEVICE_TLORAPAGER
static bool readBQ27220Word(uint8_t reg, uint16_t &value) {
    constexpr uint8_t BQ27220_ADDR = 0x55;
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(BQ27220_ADDR, (uint8_t)2) != 2) return false;
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    value = (uint16_t)lo | ((uint16_t)hi << 8);
    return true;
}
#endif

void run_system() {
    gfx->fillScreen(C_BLACK);

    gfx->fillRect(0, 0, DISP_W, 24, C_DARK);
    gfx->drawFastHLine(0, 24, DISP_W, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->print("SYSTEM INFO | " PM_EXIT_COPY);

    uint32_t freeHeap   = ESP.getFreeHeap()  / 1024;
    uint32_t totalHeap  = ESP.getHeapSize()  / 1024;
    uint32_t freePSRAM  = ESP.getFreePsram() / 1024;
    uint32_t totalPSRAM = ESP.getPsramSize() / 1024;
    uint32_t flashTotal = ESP.getFlashChipSize() / (1024 * 1024);

    uint32_t sdTotalMB = 0;
    if (sd.fatType() != 0) {
        if (spi_mutex && xSemaphoreTakeRecursive(spi_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            sdTotalMB = sd.card()->sectorCount() / 2048;
            xSemaphoreGiveRecursive(spi_mutex);
        }
    }

    float vbat = 0.0f;
    int   battPercent = 0;
    bool  battPresent = false;
#ifdef DEVICE_TLORAPAGER
    uint16_t soc = 0;
    uint16_t mv = 0;
    bool haveSoc = readBQ27220Word(0x2C, soc);
    bool haveMv = readBQ27220Word(0x08, mv);
    if (haveMv && mv >= 3300 && mv <= 4350) {
        vbat = mv / 1000.0f;
        battPresent = true;
        battPercent = (haveSoc && soc <= 100) ? soc : estimateBatteryPercent(vbat);
    }
#else
    if (PMU.isBatteryConnect()) {
        vbat = PMU.getBattVoltage() / 1000.0f;
        if (vbat >= 3.0f && vbat <= 4.35f) {
            battPresent = true;
            battPercent = PMU.getBatteryPercent();
            if (battPercent < 0)   battPercent = 0;
            if (battPercent > 100) battPercent = 100;
        }
    } else {
        vbat = PMU.getBattVoltage() / 1000.0f;
        if (vbat >= 3.30f && vbat <= 4.35f) {
            battPresent = true;
            battPercent = estimateBatteryPercent(vbat);
        }
    }
#endif

    long splines = random(1, 1000000000);

    // Value column at consistent x for both devices; on T-LoRa Pager the
    // 480 width gives us much more breathing room for the value display.
    const int labelX = 10;
    const int valueX = 150;

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(labelX, 45);  gfx->print("SRAM (Free):");
    gfx->setTextColor(C_GREEN);  gfx->setCursor(valueX, 45);
    gfx->printf("%d / %d KB", freeHeap, totalHeap);

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(labelX, 65);  gfx->print("PSRAM (Free):");
    gfx->setTextColor(C_GREEN);  gfx->setCursor(valueX, 65);
    if (totalPSRAM > 0) gfx->printf("%d / %d KB", freePSRAM, totalPSRAM);
    else { gfx->setTextColor(C_RED); gfx->print("NOT DETECTED"); }

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(labelX, 85);  gfx->print("Flash Drive:");
    gfx->setTextColor(C_GREEN);  gfx->setCursor(valueX, 85);
    gfx->printf("%d MB Total", flashTotal);

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(labelX, 105); gfx->print("MicroSD (Total):");
    gfx->setTextColor(C_GREEN);  gfx->setCursor(valueX, 105);
    if (sdTotalMB > 0) gfx->printf("%d MB", sdTotalMB);
    else { gfx->setTextColor(C_RED); gfx->print("NOT MOUNTED"); }

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(labelX, 125); gfx->print("Battery:");
    gfx->setCursor(valueX, 125);
    if (battPresent) {
        gfx->setTextColor(C_GREEN);
        gfx->printf("%.2fV (%d%%)", vbat, battPercent);
    } else {
        gfx->setTextColor(C_GREY);
        gfx->printf("USB ONLY (%.2fV)", vbat);
    }

    gfx->setTextColor(C_WHITE);
    gfx->setCursor(labelX, 155); gfx->print("Splines Reticulated:");
    gfx->setTextColor(C_GREEN);  gfx->setCursor(labelX, 175);
    gfx->printf("%ld", splines);

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while (get_touch(&tx, &ty)) { delay(10); yield(); }
                break;
            }
        }
        if (pm_is_exit_key(get_keypress())) break;
        yield();
        delay(15);
    }
}
