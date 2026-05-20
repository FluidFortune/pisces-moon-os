// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef HAL_PINS_H
#define HAL_PINS_H

// ============================================================
//  hal_pins.h — Per-device GPIO assignments
//
//  Sources:
//    T-Deck Plus:   existing confirmed schematic
//    T-LoraPager:   T-LoraPager V1.0 SCH 25-06-13.pdf
//                   + LilyGoLib official wiki hardware reference
//    Cardputer ADV: TBD — hardware in transit
// ============================================================


// ── T-DECK PLUS (ESP32-S3) ───────────────────────────────
#ifdef DEVICE_TDECK_PLUS

  #define SCREEN_W          320
  #define SCREEN_H          240
  #define SCREEN_DRIVER     ST7789

  // SPI bus
  #define SPI_MOSI          41
  #define SPI_MISO          38
  #define SPI_SCK           40

  // Display
  #define LCD_CS            12
  #define LCD_DC            11
  #define LCD_RST           -1
  #define LCD_BL            42

  // LoRa SX1262
  #define LORA_CS           9
  #define LORA_IRQ          45
  #define LORA_RST          17
  #define LORA_BUSY         13

  // SD card
  #define SD_CS             39

  // GPS
  #define GPS_RX            6
  #define GPS_TX            7
  #define GPS_PPS           -1

  // I2C
  #define I2C_SDA           18
  #define I2C_SCL           8

  // Input — trackball
  #define TRK_UP            3
  #define TRK_DOWN          15
  #define TRK_LEFT          1
  #define TRK_RIGHT         2
  #define TRK_CLICK         0

  // Audio
  #define I2S_MCLK          -1
  #define I2S_SCLK          47
  #define I2S_LRCK          -1
  #define I2S_DOUT          -1
  #define I2S_DIN           46

  // Capabilities
  #define HAS_TRACKBALL     1
  #define HAS_LORA          1
  #define HAS_GPS           1
  #define HAS_AUDIO         1
  #define HAS_NFC           0
  #define HAS_IMU           0
  #define HAS_HAPTIC        0
  #define HAS_ENCODER       0
  #define HAS_RTC           0
  #define HAS_NRF24         0
  #define HAS_IOEXP         0

#endif // DEVICE_TDECK_PLUS


// ── T-LORA PAGER (ESP32-S3) ──────────────────────────────
// Sources: T-LoraPager V1.0 SCH 25-06-13.pdf
//          LilyGoLib wiki hardware reference (official)
#ifdef DEVICE_TLORAPAGER

  #define SCREEN_W          480
  #define SCREEN_H          222
  #define SCREEN_DRIVER     ST7796

  // ── SPI bus — shared by LCD + LoRa + SD + NFC ────────
  // SPI BUS TREATY IS NON-OPTIONAL ON THIS DEVICE
  // Four Treaty participants + Ghost Engine on Core 0
  #define SPI_MOSI          34
  #define SPI_MISO          33
  #define SPI_SCK           35

  // Display ST7796U
  #define LCD_CS            38
  #define LCD_DC            37
  #define LCD_RST           -1   // via XL9555 GPIO6 (NC on this board)
  #define LCD_BL            42   // direct GPIO to AW9364 backlight driver

  // Keyboard backlight (separate from display)
  #define KB_BL             46

  // LoRa SX1262
  #define LORA_CS           36
  #define LORA_IRQ          14
  #define LORA_RST          47
  #define LORA_BUSY         48
  // LoRa power: XL9555 GPIO3

  // SD card
  #define SD_CS             21
  // SD power:   XL9555 GPIO14
  // SD detect:  XL9555 GPIO12

  // NFC ST25R3916
  #define NFC_CS            39
  #define NFC_IRQ           5
  // NFC power:  XL9555 GPIO5
  // NFC shares SPI bus — Treaty required

  // GPS MIA-M10Q (UART)
  // Note: GPS_TX/RX naming is from GPS module perspective
  #define GPS_TX            4    // GPS TX → ESP32 RX (we receive)
  #define GPS_RX            12   // GPS RX → ESP32 TX (we send)
  #define GPS_PPS           13
  // GPS power:  XL9555 GPIO4
  // GPS reset:  XL9555 GPIO7

  // I2C bus — shared by ALL I2C devices
  #define I2C_SDA           3    // NOTE: swapped vs T-Deck (SDA=3, SCL=2)
  #define I2C_SCL           2
  #define RTC_INT           1

  // I2C device addresses (confirmed from official wiki)
  #define I2C_ADDR_CODEC    0x18  // ES8311 audio codec
  #define I2C_ADDR_IOEXP    0x20  // XL9555 GPIO expander
  #define I2C_ADDR_IMU      0x28  // BHI260AP smart sensor
  #define I2C_ADDR_RTC      0x51  // PCF85063A RTC
  #define I2C_ADDR_GAUGE    0x55  // BQ27220 battery gauge
  #define I2C_ADDR_PMU      0x6B  // BQ25896 charger
  #define I2C_ADDR_KEYBOARD 0x34  // TCA8418 keyboard controller
  #define I2C_ADDR_HAPTIC   0x5A  // DRV2605 haptic driver

  // Rotary encoder
  #define ENCODER_A         40
  #define ENCODER_B         41
  #define ENCODER_BTN       7    // center click — was "IO7 unknown" in schematic

  // Keyboard TCA8418
  #define KEY_INT           6
  // Keyboard power:  XL9555 GPIO10
  // Keyboard reset:  XL9555 GPIO2

  // BHI260AP AI IMU
  #define IMU_HIRQ          8
  // I2C shared bus

  // I2S Audio ES8311
  // NOTE: ASDOUT/DSDIN are swapped from schematic extraction —
  //       wiki is authoritative
  #define I2S_MCLK          10
  #define I2S_SCLK          11
  #define I2S_LRCK          18
  #define I2S_DOUT          45   // DAC output (to speaker)
  #define I2S_DIN           17   // ADC input (from mic)
  // Speaker power: XL9555 GPIO1
  // Haptic power:  XL9555 GPIO0

  // NRF24L01 via 12-pin expansion port
  // These are the ESP32 UART1 pins repurposed for NRF24
  #define NRF24_CE          43   // UART1 TX doubles as NRF24 CE
  #define NRF24_CS          44   // UART1 RX doubles as NRF24 CS
  // NRF24 shares main SPI bus (MOSI/MISO/SCK)
  // XL9555 GPIO9 = NRF24 PA shield Tx/Rx control

  // XL9555 GPIO expander pin map
  // Access via I2C at 0x20
  #define IOEXP_HAPTIC_EN   0    // XL9555 P00 — DRV2605 enable
  #define IOEXP_SPK_EN      1    // XL9555 P01 — speaker amp enable
  #define IOEXP_KB_RST      2    // XL9555 P02 — keyboard reset
  #define IOEXP_LORA_EN     3    // XL9555 P03 — LoRa power
  #define IOEXP_GPS_EN      4    // XL9555 P04 — GNSS power
  #define IOEXP_NFC_EN      5    // XL9555 P05 — NFC power
  #define IOEXP_LCD_RST     6    // XL9555 P06 — display reset (NC)
  #define IOEXP_GPS_RST     7    // XL9555 P07 — GNSS reset
  #define IOEXP_KB_EN       8    // XL9555 P10 — keyboard power (wiki says GPIO8=KB)
  #define IOEXP_EXT_9       9    // XL9555 P11 — free (external socket)
  #define IOEXP_KB_PWR      10   // XL9555 P12 — keyboard power supply
  #define IOEXP_EXT_11      11   // XL9555 P13 — free (external socket)
  #define IOEXP_SD_DET      12   // XL9555 P14 — SD card detect
  #define IOEXP_SD_EN       14   // XL9555 P16 — SD power enable

  // Boot button
  #define BOOT_BTN          0

  // Capabilities
  #define HAS_TRACKBALL     0
  #define HAS_LORA          1
  #define HAS_GPS           1
  #define HAS_AUDIO         1
  #define HAS_NFC           1
  #define HAS_IMU           1
  #define HAS_HAPTIC        1
  #define HAS_ENCODER       1
  #define HAS_RTC           1
  #define HAS_NRF24         1
  #define HAS_IOEXP         1

  // SPI Bus Treaty note:
  // This device has MORE SPI Treaty participants than T-Deck:
  //   LCD + LoRa + SD + NFC = 4 participants
  //   Plus Ghost Engine Core 0 contention
  // LilyGoLib provides lockSPI()/unlockSPI() — our Treaty macros
  // wrap these. See spi_treaty.h for DEVICE_TLORAPAGER path.

#endif // DEVICE_TLORAPAGER


// ── CARDPUTER ADV (ESP32-S3) ─────────────────────────────
// Pin assignments TBD — hardware in transit
#ifdef DEVICE_CARDPUTER_ADV

  #define SCREEN_W          240
  #define SCREEN_H          135
  #define SCREEN_DRIVER     ST7789

  // TODO: fill in all pins when hardware arrives

  // Capabilities (base — headers not yet installed)
  #define HAS_TRACKBALL     0
  #define HAS_LORA          0
  #define HAS_GPS           0
  #define HAS_AUDIO         1
  #define HAS_NFC           0
  #define HAS_IMU           0
  #define HAS_HAPTIC        0
  #define HAS_ENCODER       0
  #define HAS_RTC           0
  #define HAS_NRF24         0
  #define HAS_IOEXP         0

#endif // DEVICE_CARDPUTER_ADV


// ── RUNTIME CAPABILITY CHECKS ────────────────────────────
// Apps use these instead of device defines directly
// Launcher uses these to show/hide apps per device

static inline int pm_has_lora(void)    { return HAS_LORA;    }
static inline int pm_has_gps(void)     { return HAS_GPS;     }
static inline int pm_has_nfc(void)     { return HAS_NFC;     }
static inline int pm_has_imu(void)     { return HAS_IMU;     }
static inline int pm_has_haptic(void)  { return HAS_HAPTIC;  }
static inline int pm_has_audio(void)   { return HAS_AUDIO;   }
static inline int pm_has_encoder(void) { return HAS_ENCODER; }
static inline int pm_has_rtc(void)     { return HAS_RTC;     }
static inline int pm_has_nrf24(void)   { return HAS_NRF24;   }
static inline int pm_has_ioexp(void)   { return HAS_IOEXP;   }

#endif // HAL_PINS_H
