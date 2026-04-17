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

#ifndef HAL_H
#define HAL_H

// --- I2C & POWER MANAGEMENT ---
#define BOARD_SDA 18
#define BOARD_SCL 8
#define BOARD_POWERON 10
#define PMU_ADDRESS 0x40 

// --- THE UNIFIED SPI BUS ---
#define BOARD_TFT_MISO 38
#define BOARD_TFT_MOSI 41
#define BOARD_TFT_SCK  40

// --- CHIP SELECTS ---
#define BOARD_TFT_CS   12
#define BOARD_SD_CS    39
#define BOARD_LORA_CS  9  

// --- DISPLAY HARDWARE ---
#define BOARD_TFT_DC   11
#define BOARD_LCD_BL   42 

// --- TRACKBALL PHYSICAL PINS ---
#define TRK_UP    21
#define TRK_DOWN  1
#define TRK_LEFT  46
#define TRK_RIGHT 3
#define TRK_CLICK 0

// --- TOUCH CONTROLLER (GT911) ---
#define TOUCH_INT 16
#define TOUCH_RST 15
#define TOUCH_ADDR 0x5D // Sometimes 0x14 depending on boot strap

// --- ACCELEROMETER (QMA6100P) ---
#define IMU_ADDR 0x12

// --- CO-PROCESSOR ADDRESSES ---
#define KEYBOARD_ADDR  0x55 
#define TRACKBALL_ADDR 0x4A 

#endif