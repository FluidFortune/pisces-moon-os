// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

// ============================================================
//  variants/m5stack_cardputer_adv/pins_arduino.h
//
//  Arduino-ESP32 variant header for the M5Stack Cardputer ADV.
//  Hardware: Stamp-S3A core module (ESP32-S3FN8, 8MB flash).
//
//  Sources of truth:
//    docs.m5stack.com/en/core/Cardputer-Adv (PinMap section)
//    docs.m5stack.com/en/cap/Cap_LoRa868    (header pinout)
//
//  We follow the LilyGoLib precedent established by the
//  T-LoraPager variant: ship the variant header inside the
//  Pisces Moon repo and pull it in via -I in platformio.ini,
//  rather than depending on it being merged into Arduino-ESP32
//  core. The Cardputer ADV variant does not exist in
//  arduino-esp32 core 2.0.16; we provide it here.
// ============================================================

#define USB_VID          0x303a   // Espressif
#define USB_PID          0x1001
#define USB_MANUFACTURER "M5Stack"
#define USB_PRODUCT      "Cardputer-Adv"
#define USB_SERIAL       ""

// ── LED — none on the bare Stamp; RGB indicator shares
//    backlight power line per the M5 docs ("RGB LED Logic:
//    Shares power with display backlight").
//    Arduino's LED_BUILTIN is left undefined intentionally.
// #define LED_BUILTIN  -1

// ── SPI — shared bus for SD and the LoRa cap header.
//    Per Cardputer ADV PinMap "microSD" and EXT 2.54-14P:
//      MISO = G39
//      MOSI = G14
//      SCK  = G40
//      SS   = G12 (SD-only; LoRa CS on header is G5)
//    Designers MUST NOT assume one CS — multiple slaves on
//    this bus. SD_CS=12, LoRa_NSS=5. Treaty mandatory.
#define MOSI 14
#define MISO 39
#define SCK  40
#define SS   12   // SD card CS by default; LoRa uses G5

// ── I2C — single bus shared by codec, IMU, keyboard chip.
//    Per Cardputer ADV PinMap "Audio", "IMU", "Keyboard":
//      SDA = G8
//      SCL = G9
//    Devices on bus:
//      ES8311 audio codec (0x18)
//      BMI270 IMU         (0x68 or 0x69)
//      TCA8418 keyboard   (0x34)
#define SDA  8
#define SCL  9

// ── UART0 — USB CDC (Arduino HardwareSerial Serial)
//    On the Stamp-S3A the default `Serial` is the native USB-CDC
//    interface; TX/RX defines below are for completeness and
//    point at the EXT 2.54-14P UART pins per the M5 docs.
//    Naming is ESP32-perspective. The cap's "GPS-RX" (peripheral
//    receive) lands on G13 → ESP32 TX. The cap's "GPS-TX" lands
//    on G15 → ESP32 RX.
#define TX 13
#define RX 15

// ── A pin count for the analog-input mapping table that
//    Arduino-ESP32 expects every variant to expose.
static const uint8_t A0  = 1;
static const uint8_t A1  = 2;
static const uint8_t A2  = 3;
static const uint8_t A3  = 4;
static const uint8_t A4  = 5;
static const uint8_t A5  = 6;
static const uint8_t A6  = 7;
static const uint8_t A7  = 8;
static const uint8_t A8  = 9;
static const uint8_t A9  = 10;
static const uint8_t A10 = 11;
static const uint8_t A11 = 12;
static const uint8_t A12 = 13;
static const uint8_t A13 = 14;
static const uint8_t A14 = 15;
static const uint8_t A15 = 16;
static const uint8_t A16 = 17;
static const uint8_t A17 = 18;
static const uint8_t A18 = 19;
static const uint8_t A19 = 20;

// Battery ADC — per M5 docs, G10 has the battery divider.
#define BAT_ADC 10

#endif // Pins_Arduino_h
