// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef SPI_TREATY_H
#define SPI_TREATY_H

// ============================================================
//  spi_treaty.h — SPI Bus Treaty
//
//  The SPI Bus Treaty is the architectural discipline that
//  makes the Ghost Engine safe. Any code that touches the
//  shared SPI bus must acquire the bus mutex first via
//  PM_SPI_TAKE() and release it via PM_SPI_GIVE().
//
//  This implementation is library-free. We own the mutex.
//  We set the timeout. We control the debug output.
//  No wrapper around someone else's semaphore.
//
//  Treaty participants:
//    T-Deck Plus:   LoRa + SD                    (2)
//    T-LoraPager:   LCD + LoRa + SD + NFC        (4)
//    Cardputer ADV: LCD + LoRa + SD (when ready) (3)
//
//  Plus Ghost Engine Core 0 contention on all devices.
//  The Ghost Engine is always running. The Treaty is why
//  it never crashes the bus.
//
//  The T-LoraPager additionally requires peripheral power
//  management via the XL9555 I2C GPIO expander. Every
//  peripheral — LoRa, GPS, NFC, SD, speaker, keyboard,
//  haptic — is power-gated and must be explicitly enabled
//  before use. Boot sequence is defined here.
// ============================================================

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <Arduino.h>

// ── Configuration ─────────────────────────────────────────
#define TREATY_TIMEOUT_MS   500
#define TREATY_DEBUG        0

#if TREATY_DEBUG
  #define _TREATY_LOG(who, action) \
      Serial.printf("[TREATY] %-16s %s @ %lums\n", \
                    who, action, (unsigned long)millis())
#else
  // Must expand to a valid expression (not empty), because the
  // PM_SPI_TAKE / PM_SPI_GIVE macros use it as the left operand of
  // a comma expression. Empty expansion produces "(, expr)" which
  // is a syntax error.
  #define _TREATY_LOG(who, action)  ((void)0)
#endif

// ── The mutex — defined once in wardrive.cpp ──────────────
extern SemaphoreHandle_t spi_mutex;

// ── Core Treaty macros — identical on all devices ─────────

#define PM_SPI_TAKE(who) \
    (_TREATY_LOG(who, "TAKE"), \
     spi_mutex \
         ? (xSemaphoreTakeRecursive(spi_mutex, \
                pdMS_TO_TICKS(TREATY_TIMEOUT_MS)) == pdTRUE) \
         : true)

#define PM_SPI_GIVE() \
    (_TREATY_LOG("", "GIVE"), \
     (void)(spi_mutex ? xSemaphoreGiveRecursive(spi_mutex) : pdFALSE))

#define PM_SPI_TAKE_ISR(pxWoken) \
    (spi_mutex \
         ? (xSemaphoreTakeFromISR(spi_mutex, pxWoken) == pdTRUE) \
         : true)

#define PM_SPI_GIVE_ISR(pxWoken) \
    ((void)(spi_mutex \
         ? xSemaphoreGiveFromISR(spi_mutex, pxWoken) \
         : pdFALSE))


// ── T-LORA PAGER: XL9555 power management ────────────────
// All peripherals are power-gated through the XL9555 I2C
// GPIO expander at 0x20. Direct I2C writes — no library.
//
// XL9555 registers:
//   0x02 = Output Port 0 (GPIO 0-7)
//   0x03 = Output Port 1 (GPIO 8-15, labeled GPIO10-17)
//   0x06 = Config Port 0 (0=output)
//   0x07 = Config Port 1 (0=output)
#ifdef DEVICE_TLORAPAGER

#include <Wire.h>
#include "hal_pins.h"

// Shadow registers — track current output state
// Defined in wardrive.cpp alongside spi_mutex
extern uint8_t _xl9555_p0;
extern uint8_t _xl9555_p1;

static inline void _xl9555_write_p0(uint8_t val) {
    _xl9555_p0 = val;
    Wire.beginTransmission(I2C_ADDR_IOEXP);
    Wire.write(0x02);
    Wire.write(val);
    Wire.endTransmission();
}

static inline void _xl9555_write_p1(uint8_t val) {
    _xl9555_p1 = val;
    Wire.beginTransmission(I2C_ADDR_IOEXP);
    Wire.write(0x03);
    Wire.write(val);
    Wire.endTransmission();
}

// Set or clear a single XL9555 output bit
// bit 0-7  → port 0
// bit 8-15 → port 1
static inline void _xl9555_set_bit(uint8_t bit, bool en) {
    if (bit < 8) {
        uint8_t v = en ? (_xl9555_p0 | (1 << bit))
                       : (_xl9555_p0 & ~(1 << bit));
        _xl9555_write_p0(v);
    } else {
        uint8_t b = bit - 8;
        uint8_t v = en ? (_xl9555_p1 | (1 << b))
                       : (_xl9555_p1 & ~(1 << b));
        _xl9555_write_p1(v);
    }
}

// ── XL9555 init — called from pm_hal_init() ──────────────
static inline void xl9555_init() {
    // Configure all pins as outputs
    Wire.beginTransmission(I2C_ADDR_IOEXP);
    Wire.write(0x06);
    Wire.write(0x00);
    Wire.endTransmission();
    Wire.beginTransmission(I2C_ADDR_IOEXP);
    Wire.write(0x07);
    Wire.write(0x00);
    Wire.endTransmission();
    // All peripherals off
    _xl9555_write_p0(0x00);
    _xl9555_write_p1(0x00);
    Serial.println("[HAL] XL9555 init — all peripherals off");
}

// ── Peripheral power macros ───────────────────────────────
#define PM_LORA_POWER_ON()    _xl9555_set_bit(IOEXP_LORA_EN,  true)
#define PM_LORA_POWER_OFF()   _xl9555_set_bit(IOEXP_LORA_EN,  false)
#define PM_GPS_POWER_ON()     _xl9555_set_bit(IOEXP_GPS_EN,   true)
#define PM_GPS_POWER_OFF()    _xl9555_set_bit(IOEXP_GPS_EN,   false)
#define PM_NFC_POWER_ON()     _xl9555_set_bit(IOEXP_NFC_EN,   true)
#define PM_NFC_POWER_OFF()    _xl9555_set_bit(IOEXP_NFC_EN,   false)
#define PM_SD_POWER_ON()      _xl9555_set_bit(IOEXP_SD_EN,    true)
#define PM_SD_POWER_OFF()     _xl9555_set_bit(IOEXP_SD_EN,    false)
#define PM_SPK_POWER_ON()     _xl9555_set_bit(IOEXP_SPK_EN,   true)
#define PM_SPK_POWER_OFF()    _xl9555_set_bit(IOEXP_SPK_EN,   false)
#define PM_HAPTIC_POWER_ON()  _xl9555_set_bit(IOEXP_HAPTIC_EN,true)
#define PM_HAPTIC_POWER_OFF() _xl9555_set_bit(IOEXP_HAPTIC_EN,false)
#define PM_KB_POWER_ON()      _xl9555_set_bit(IOEXP_KB_EN,    true)
#define PM_KB_POWER_OFF()     _xl9555_set_bit(IOEXP_KB_EN,    false)
#define PM_GPS_RESET() do { \
    _xl9555_set_bit(IOEXP_GPS_RST, false); \
    delay(10); \
    _xl9555_set_bit(IOEXP_GPS_RST, true); \
} while(0)

// ── Boot power sequence ───────────────────────────────────
// Called from pm_hal_init() after I2C is running.
// Order matters — Ghost Engine needs LoRa before its task
// launches on Core 0. SD must be up before filesystem init.
static inline void xl9555_boot_sequence() {
    // 1. SD — filesystem before anything else
    PM_SD_POWER_ON();
    delay(50);
    Serial.println("[HAL] SD power ON");

    // 2. LoRa — Ghost Engine needs it at task launch
    PM_LORA_POWER_ON();
    delay(20);
    Serial.println("[HAL] LoRa power ON");

    // 3. GPS — NMEA background task
    PM_GPS_POWER_ON();
    delay(20);
    Serial.println("[HAL] GPS power ON");

    // 4. Keyboard — before launcher renders
    PM_KB_POWER_ON();
    delay(20);
    Serial.println("[HAL] Keyboard power ON");

    // 5. Speaker + haptic — audio subsystem
    PM_SPK_POWER_ON();
    delay(10);
    PM_HAPTIC_POWER_ON();
    delay(10);
    Serial.println("[HAL] Audio + haptic power ON");

    // 6. NFC — last, not needed at boot
    PM_NFC_POWER_ON();
    delay(20);
    Serial.println("[HAL] NFC power ON");

    Serial.println("[HAL] Boot power sequence complete");
}

#endif // DEVICE_TLORAPAGER


// ── No-op power macros for T-Deck and Cardputer ──────────
// These devices have no power gating — peripherals are
// always on. Macros compile to nothing in release builds.
#ifndef DEVICE_TLORAPAGER
  #define PM_LORA_POWER_ON()
  #define PM_LORA_POWER_OFF()
  #define PM_GPS_POWER_ON()
  #define PM_GPS_POWER_OFF()
  #define PM_NFC_POWER_ON()
  #define PM_NFC_POWER_OFF()
  #define PM_SD_POWER_ON()
  #define PM_SD_POWER_OFF()
  #define PM_SPK_POWER_ON()
  #define PM_SPK_POWER_OFF()
  #define PM_HAPTIC_POWER_ON()
  #define PM_HAPTIC_POWER_OFF()
  #define PM_KB_POWER_ON()
  #define PM_KB_POWER_OFF()
  #define PM_GPS_RESET()
  static inline void xl9555_init() {}
  static inline void xl9555_boot_sequence() {}
#endif

// ── Safety check ─────────────────────────────────────────
#if !defined(DEVICE_TDECK_PLUS) && \
    !defined(DEVICE_TLORAPAGER) && \
    !defined(DEVICE_CARDPUTER_ADV)
  #error "No device target defined. Set DEVICE_TDECK_PLUS, \
DEVICE_TLORAPAGER, or DEVICE_CARDPUTER_ADV in platformio.ini."
#endif

#endif // SPI_TREATY_H
