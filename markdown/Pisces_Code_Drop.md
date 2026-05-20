Apps.h source:

#ifndef APPS_H
#define APPS_H

#include <Arduino.h>

// --- OS KERNEL UI ---
void run_launcher();
void show_splash_screen();

// --- THE APP SUITE ---
void run_snake();        // snake.cpp
void run_filesystem();   // filesystem.cpp
void run_notepad();      // notepad.cpp (JOURNAL)
void run_calculator();   // calculator.cpp (MATH)
void run_clock();        // clock.cpp
void run_wifi_app();     // wifi_app.cpp (WIFI)
void run_calendar();     // calendar.cpp
void run_system();       // system_app.cpp
void run_about();        // about_app.cpp
void run_gps();          // gps_app.cpp
void run_etch();         // etch.cpp (ETCHASKETCH)
void run_bluetooth_app(); // bluetooth_app.cpp

// --- UTILITIES ---
String get_text_input(int x, int y); 

#endif

main.cpp source:

#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include "SdFat.h"
#include <Wire.h>
#include <WiFi.h>
#include <XPowersLib.h> 
#include <TinyGPSPlus.h>
#include "hal.h"
#include "keyboard.h"
#include "trackball.h"
#include "touch.h"
#include "apps.h"
#include "theme.h"
#include "wardrive.h"

// --- T-DECK PLUS HARD PIN OVERRIDES ---
#undef BOARD_SDA
#undef BOARD_SCL
#define BOARD_SDA 18
#define BOARD_SCL 17
#define PIN_POWER_ON 10
#define LORA_CS 9  
#define SD_CS 39
#define TFT_CS 12

SdFat sd;
Arduino_DataBus *bus;
Arduino_GFX *gfx;
XPowersAXP2101 PMU; 
TinyGPSPlus gps; 

bool pmu_online = false;
bool touch_online = false;

// --- 90s TERMINAL TYPEWRITER ---
void term_print(const char* msg, uint16_t color = C_WHITE, int wait = 100) {
    gfx->setTextColor(color);
    for(int i=0; msg[i] != '\0'; i++) {
        gfx->print(msg[i]);
        delay(10); 
    }
    delay(wait);
}

// --- THE SPLASH SCREEN (Linking Fix) ---
void show_splash_screen() {
    gfx->fillScreen(C_BLACK);
    gfx->drawRect(5, 5, 310, 230, C_WHITE);
    gfx->drawRect(7, 7, 306, 226, 0x001F); 
    gfx->setTextSize(4); 
    uint16_t c[] = {0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x001F, 0xF81F};
    String p = "PISCES"; String m = "MOON OS";
    for(int i=0; i<p.length(); i++) { gfx->setTextColor(c[i%6]); gfx->setCursor(88+(i*24), 60); gfx->print(p[i]); }
    for(int i=0; i<m.length(); i++) { gfx->setTextColor(c[(i+2)%6]); gfx->setCursor(76+(i*24), 110); gfx->print(m[i]); }
    gfx->setTextSize(1); gfx->setTextColor(C_WHITE);
    gfx->setCursor(65, 180); gfx->print("Loading Pisces Moon kernel...");
}

void setup() {
    // 1. PIN PROTECTION
    pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
    pinMode(SD_CS, OUTPUT); digitalWrite(SD_CS, HIGH);
    pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
    pinMode(PIN_POWER_ON, OUTPUT); digitalWrite(PIN_POWER_ON, HIGH);

    Serial.begin(115200);

    // 2. BIOS DISPLAY INIT
    SPI.begin(BOARD_TFT_SCK, BOARD_TFT_MISO, BOARD_TFT_MOSI);
    bus = new Arduino_HWSPI(BOARD_TFT_DC, TFT_CS, BOARD_TFT_SCK, BOARD_TFT_MOSI, BOARD_TFT_MISO, &SPI, true);
    gfx = new Arduino_ST7789(bus, -1, 1, true, 240, 320);
    gfx->begin();
    gfx->fillScreen(C_BLACK);
    pinMode(BOARD_LCD_BL, OUTPUT); digitalWrite(BOARD_LCD_BL, HIGH);

    // --- BIOS START ---
    gfx->setCursor(0, 0); gfx->setTextSize(1);
    term_print("PISCES MOON V1.0.4 BIOS v4.51PG\n", 0xAD55);
    term_print("Copyright (C) 1994-2026, PM-OS Industries\n\n");
    term_print("Main Processor : ESP32-S3 Dual-Core\n");
    term_print("Memory Test    : 320KB SRAM OK\n");
    term_print("HIMEM.SYS is testing extended memory... done.\n\n");

    // 3. HARDWARE HANDSHAKES
    term_print("Initializing I2C Bus... ", C_WHITE);
    Wire.begin(BOARD_SDA, BOARD_SCL, 100000);
    term_print("OK\n", C_GREEN);

    term_print("Purgalating Purgulators (PMU)... ", C_WHITE);
    pmu_online = PMU.init(Wire, BOARD_SDA, BOARD_SCL, PMU_ADDRESS);
    if (pmu_online) {
        PMU.enableBattVoltageMeasure();
        PMU.setALDO2Voltage(3300); PMU.enableALDO2(); // GPS
        PMU.setALDO3Voltage(3300); PMU.enableALDO3(); // Touch
        PMU.setBLDO1Voltage(3300); PMU.enableBLDO1(); // Backlight
        term_print("LOADED\n", C_GREEN);
    } else {
        term_print("FAILED\n", C_RED);
    }

    term_print("Reticulating Splines... ", C_WHITE);
    delay(500); term_print("DONE\n", C_GREEN);

    term_print("Waking Touch Controller (GT911)... ", C_WHITE);
    touch_online = init_touch(); 
    if (touch_online) {
        term_print("ACTIVE\n", C_GREEN);
    } else {
        term_print("NOT RESPONDING\n", C_RED);
    }

    term_print("Detecting MicroSD Card... ", C_WHITE);
    if (sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(10), &SPI))) {
        term_print("OK\n", C_GREEN);
    } else {
        term_print("DRIVE EMPTY\n", 0xFD20);
    }

    term_print("Ghost Core 0 (WiFi/GPS)... ", C_WHITE);
    init_wardrive_core();
    term_print("ONLINE\n", C_GREEN);

    term_print("\nAll systems GO. Launching GUI...\n");
    delay(1200);

    show_splash_screen();
    delay(2000);
    run_launcher(); 
}

void loop() { yield(); }

touch.cpp:

#include "touch.h"
#include <TAMC_GT911.h>
#include <Wire.h>

// Hard-coding T-Deck Plus pins
#define T_SDA 18
#define T_SCL 17
#define T_INT 16
#define T_RST 15

TAMC_GT911 tp = TAMC_GT911(T_SDA, T_SCL, T_INT, T_RST, 320, 240);

bool init_touch() {
    pinMode(T_RST, OUTPUT);
    pinMode(T_INT, OUTPUT);
    
    // Hardware Reset Pulse
    digitalWrite(T_RST, LOW);
    digitalWrite(T_INT, LOW);
    delay(30);
    digitalWrite(T_RST, HIGH);
    delay(150); 
    
    pinMode(T_INT, INPUT); // Hand control to the chip
    delay(50);

    // Try primary address
    Wire.beginTransmission(0x14);
    if (Wire.endTransmission() == 0) {
        tp.begin(0x14);
        tp.setRotation(ROTATION_NORMAL);
        return true;
    }
    
    // Try secondary address
    Wire.beginTransmission(0x5D);
    if (Wire.endTransmission() == 0) {
        tp.begin(0x5D);
        tp.setRotation(ROTATION_NORMAL);
        return true;
    }

    return false;
}

TouchData update_touch() {
    tp.read();
    TouchData d;
    d.pressed = (tp.touches > 0);
    if (d.pressed) {
        d.x = 320 - tp.points[0].y;
        d.y = tp.points[0].x;
    }
    return d;
}

hal.h:

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

