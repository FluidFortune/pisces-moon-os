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

#include <Arduino_GFX_Library.h>
#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "touch.h" 
#include "theme.h"
#include "apps.h"
#include <XPowersLib.h> 

extern Arduino_GFX *gfx;
extern XPowersAXP2101 PMU; 
extern SdFat sd;
extern SemaphoreHandle_t spi_mutex;

void run_system() {
    gfx->fillScreen(C_BLACK);
    
    gfx->fillRect(0, 0, 320, 24, C_DARK);
    gfx->drawFastHLine(0, 24, 320, C_GREEN);
    gfx->setCursor(10, 7);
    gfx->setTextColor(C_GREEN);
    gfx->print("SYSTEM INFO | TAP HEADER TO EXIT");

    // Memory calculations
    uint32_t freeHeap = ESP.getFreeHeap() / 1024;
    uint32_t totalHeap = ESP.getHeapSize() / 1024;
    uint32_t freePSRAM = ESP.getFreePsram() / 1024;
    uint32_t totalPSRAM = ESP.getPsramSize() / 1024;
    uint32_t flashTotal = ESP.getFlashChipSize() / (1024 * 1024); // in MB
    
    // MicroSD size — guarded by spi_mutex.
    // Without the mutex, Core 0 (wardrive) may be mid-write when Core 1
    // calls sd.card()->sectorCount(), corrupting SdFat internal state and
    // triggering a tlsf_walk_pool WDT panic on Core 1.
    uint32_t sdTotalMB = 0;
    if (sd.fatType() != 0) {
        if (spi_mutex && xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            sdTotalMB = sd.card()->sectorCount() / 2048;
            xSemaphoreGive(spi_mutex);
        }
    }

    // Battery calculations — guarded against USB-only / no-battery state.
    // AXP2101 returns garbage voltages and negative percentages when running
    // purely on USB with no cell attached. Read voltage first; only call
    // getBatteryPercent() when a cell is confirmed present to avoid a crash.
    float vbat = 0.0f;
    int battPercent = 0;
    bool battPresent = false;
    if (PMU.isBatteryConnect()) {
        vbat = PMU.getBattVoltage() / 1000.0f;
        // Sanity-check: a connected LiPo should read between 3.0 V and 4.35 V.
        // Values outside this range mean the PMU is lying (common on USB-only).
        if (vbat >= 3.0f && vbat <= 4.35f) {
            battPresent = true;
            battPercent = PMU.getBatteryPercent();
            if (battPercent < 0)   battPercent = 0;
            if (battPercent > 100) battPercent = 100;
        }
    } else {
        // No battery detected — still read raw voltage for debug display
        vbat = PMU.getBattVoltage() / 1000.0f;
    }
    
    // The Mad Scientist Metric
    long splines = random(1, 1000000000);

    // Rendering SRAM
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 45); gfx->print("SRAM (Free):"); 
    gfx->setTextColor(C_GREEN); gfx->setCursor(140, 45); gfx->printf("%d / %d KB", freeHeap, totalHeap);

    // Rendering PSRAM
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 65); gfx->print("PSRAM (Free):"); 
    gfx->setTextColor(C_GREEN); gfx->setCursor(140, 65); 
    if(totalPSRAM > 0) gfx->printf("%d / %d KB", freePSRAM, totalPSRAM);
    else { gfx->setTextColor(C_RED); gfx->print("NOT DETECTED"); }

    // Rendering Flash
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 85); gfx->print("Flash Drive:"); 
    gfx->setTextColor(C_GREEN); gfx->setCursor(140, 85); gfx->printf("%d MB Total", flashTotal);

    // Rendering SD Card
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 105); gfx->print("MicroSD (Total):"); 
    gfx->setTextColor(C_GREEN); gfx->setCursor(140, 105); 
    if (sdTotalMB > 0) {
        gfx->printf("%d MB", sdTotalMB);
    } else {
        gfx->setTextColor(C_RED);
        gfx->print("NOT MOUNTED");
    }

    // Rendering Battery
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 125); gfx->print("Battery:"); 
    gfx->setCursor(140, 125);
    if (battPresent) {
        gfx->setTextColor(C_GREEN);
        gfx->printf("%.2fV (%d%%)", vbat, battPercent);
    } else {
        gfx->setTextColor(C_GREY);
        // Raw voltage exposed for AXP2101 debug when no cell is present
        gfx->printf("USB ONLY (%.2fV)", vbat);
    }

    // Rendering Splines
    gfx->setTextColor(C_WHITE);
    gfx->setCursor(10, 155); gfx->print("Splines Reticulated:"); 
    gfx->setTextColor(C_GREEN); gfx->setCursor(10, 175); gfx->printf("%ld", splines);

    // --- EXIT LOGIC: HEADER TAP ---
    while(true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty)) {
            if (ty < 30) {
                while(get_touch(&tx, &ty)) { delay(10); yield(); }
                break;
            }
        }
        yield();
        delay(15);
    }
}