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

  // Audio — T-Deck Plus has TWO I2S buses
  //
  //   OUT bus (I2S_NUM_0): MAX98357A amp → speaker
  //     No MCLK pin — the amp self-clocks off BCLK.
  //   IN  bus (I2S_NUM_1): ES7210 4-mic codec
  //     Requires MCLK for the codec's internal PLL.
  //
  // Values come from platformio.ini via PIN_I2S_OUT_* / PIN_I2S_IN_*.
  // Single-bus devices (C28P, Pager, Cardputer, Maxine) keep the
  // simpler PIN_I2S_* names — see the cross-device convention note
  // at the bottom of this file.
  #define I2S_OUT_SCLK      PIN_I2S_OUT_SCLK
  #define I2S_OUT_LRCK      PIN_I2S_OUT_LRCK
  #define I2S_OUT_DOUT      PIN_I2S_OUT_DOUT
  #define I2S_IN_MCLK       PIN_I2S_IN_MCLK
  #define I2S_IN_SCLK       PIN_I2S_IN_SCLK
  #define I2S_IN_LRCK       PIN_I2S_IN_LRCK
  #define I2S_IN_DIN        PIN_I2S_IN_DIN

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
  #define HAS_TOUCH         1
  #define HAS_KEYBOARD      1
  #define HAS_BATTERY       1

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
  #define HAS_TOUCH         0             // no touchscreen on Pager
  #define HAS_KEYBOARD      1             // TCA8418 matrix
  #define HAS_BATTERY       1

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
  #define HAS_TOUCH         0             // no touchscreen
  #define HAS_KEYBOARD      1             // M5 Cardputer keyboard
  #define HAS_BATTERY       1

#endif // DEVICE_CARDPUTER_ADV


// ── C28P (LCDwiki 2.8" ESP32-S3 Display) ─────────────────
// Entry-level kiosk target. Touch-only input, USB-powered desk
// fixture, audio I/O as the headline capability.
//
// INTERNAL DEVELOPMENT — pin assignments per LCDwiki documentation.
// Subject to first-bring-up confirmation.
#ifdef DEVICE_C28P

  #define SCREEN_W          240
  #define SCREEN_H          320           // native portrait
  #define SCREEN_DRIVER     ILI9341

  // Display SPI pins (defined via -D in platformio.ini)
  #define LCD_MOSI          PIN_LCD_MOSI
  #define LCD_MISO          PIN_LCD_MISO
  #define LCD_SCK           PIN_LCD_SCK
  #define LCD_CS            PIN_LCD_CS
  #define LCD_DC            PIN_LCD_DC
  #define LCD_RST           PIN_LCD_RST
  #define LCD_BL            PIN_LCD_BL

  // Touch (FT6336G capacitive, I2C shared bus)
  #define TOUCH_INT         PIN_TOUCH_INT
  #define TOUCH_RST         PIN_TOUCH_RST

  // I2C bus (touch + audio codec + future expansion)
  #define I2C_SDA           PIN_I2C_SDA
  #define I2C_SCL           PIN_I2C_SCL

  // I2S audio (ES8311 codec)
  #define I2S_MCLK          PIN_I2S_MCLK
  #define I2S_SCLK          PIN_I2S_SCLK
  #define I2S_LRCK          PIN_I2S_LRCK
  #define I2S_DOUT          PIN_I2S_DOUT
  #define I2S_DIN           PIN_I2S_DIN

  // MicroSD via SDIO 4-bit mode (separate from SPI bus)
  #define SD_CLK            PIN_SD_CLK
  #define SD_CMD            PIN_SD_CMD
  #define SD_D0             PIN_SD_D0
  #define SD_D1             PIN_SD_D1
  #define SD_D2             PIN_SD_D2
  #define SD_D3             PIN_SD_D3

  // Misc
  #define RGB_LED           PIN_RGB_LED   // single WS2812
  #define BAT_ADC           PIN_BAT_ADC
  #define BOOT_BTN          PIN_BOOT_BTN

  // Capabilities — C28P is touch-only kiosk
  #define HAS_TRACKBALL     0
  #define HAS_LORA          0
  #define HAS_GPS           0
  #define HAS_AUDIO         1             // headline capability
  #define HAS_NFC           0
  #define HAS_IMU           0
  #define HAS_HAPTIC        0
  #define HAS_ENCODER       0
  #define HAS_RTC           0
  #define HAS_NRF24         0
  #define HAS_IOEXP         0
  #define HAS_TOUCH         1
  #define HAS_KEYBOARD      0             // no physical keys
  #define HAS_BATTERY       0             // USB-powered desk fixture

#endif // DEVICE_C28P


// ── MAXINE (Sunton ESP32-8048S050C, 5" 800x480) ──────────
// Large-format kiosk target. Same SOFTWARE class as the C28P
// (touch-only, no keyboard, no trackball, no radios beyond
// WiFi/BLE) on a much larger 800x480 IPS panel driven over a
// parallel RGB interface (ST7262) rather than SPI. Touch is a
// GT911 capacitive controller on I2C.
//
// Operated in PORTRAIT to match the C28P interaction model:
// the panel is natively 800x480 landscape, so portrait gives a
// 480x800 logical surface (SCREEN_W x SCREEN_H below). The RGB
// panel + rotation is set up in main.cpp's gfx instantiation;
// everything above the gfx pointer is resolution-driven and
// scales from these two numbers.
//
// NO BUILT-IN MICROPHONE on this board. Audio is output-only
// (speaker via amp). Recording software is compiled out:
// HAS_AUDIO stays 1 (speaker/tones work) but HAS_AUDIO_IN is 0
// and the env omits the recording apps from build_src_filter.
//
// INTERNAL DEVELOPMENT — RGB/touch pins below are a STARTING
// HYPOTHESIS sourced from Sunton/Guition 8048-family references
// (Arduino_GFX + TAMC_GT911 community configs). The RGB pin map
// VARIES between 8048 variants — CONFIRM AGAINST YOUR BOARD AT
// FIRST BRING-UP before trusting these. Pins only matter to the
// gfx instantiation in main.cpp; the launcher/games are
// pin-agnostic.
#ifdef DEVICE_MAXINE

  #define SCREEN_W          480           // portrait logical width
  #define SCREEN_H          800           // portrait logical height
  #define SCREEN_DRIVER     ST7262        // RGB parallel panel
  #define SCREEN_IS_RGB     1             // parallel RGB, not SPI

  // Shared kiosk-UI marker. Both touch-only kiosk boards set this
  // so capability-driven code can treat them alike where useful.
  // (Maxine still runs its OWN maxine_boot/maxine_dpad files; this
  // is for capability checks, not code-path sharing.)
  #define PM_TOUCH_KIOSK    1

  // ── RGB panel control + data pins (via -D in platformio.ini) ──
  #define RGB_DE            PIN_RGB_DE
  #define RGB_VSYNC         PIN_RGB_VSYNC
  #define RGB_HSYNC         PIN_RGB_HSYNC
  #define RGB_PCLK          PIN_RGB_PCLK
  #define RGB_BL            PIN_RGB_BL
  // R0-R4, G0-G5, B0-B4 (RGB565 = 16 data lines) defined in env

  // Touch (GT911 capacitive, I2C)
  #define TOUCH_SDA         PIN_TOUCH_SDA
  #define TOUCH_SCL         PIN_TOUCH_SCL
  #define TOUCH_INT         PIN_TOUCH_INT
  #define TOUCH_RST         PIN_TOUCH_RST
  #define I2C_ADDR_TOUCH    0x5D          // GT911 default (0x14 alt)

  // I2S audio (output only — no mic on this board)
  #define I2S_MCLK          PIN_I2S_MCLK
  #define I2S_SCLK          PIN_I2S_SCLK
  #define I2S_LRCK          PIN_I2S_LRCK
  #define I2S_DOUT          PIN_I2S_DOUT

  // MicroSD (SPI, separate from RGB bus)
  #define SD_CS             PIN_SD_CS

  // Misc
  #define BOOT_BTN          PIN_BOOT_BTN

  // Capabilities — large-format touch kiosk, no mic
  #define HAS_TRACKBALL     0
  #define HAS_LORA          0
  #define HAS_GPS           0
  #define HAS_AUDIO         1             // output only (speaker/tones)
  #define HAS_AUDIO_IN      0             // NO built-in microphone
  #define HAS_NFC           0
  #define HAS_IMU           0
  #define HAS_HAPTIC        0
  #define HAS_ENCODER       0
  #define HAS_RTC           0
  #define HAS_NRF24         0
  #define HAS_IOEXP         0
  #define HAS_TOUCH         1
  #define HAS_KEYBOARD      0             // no physical keys
  #define HAS_BATTERY       0             // USB/DC-powered fixture

#endif // DEVICE_MAXINE


// ── NM-CYD-C5 (ESP32-C5 RockBase "Colorful") ─────────────
// Touch-only desk fixture, USB-powered. Same SOFTWARE class as the
// C28P (touch-only, no keyboard, no radios beyond WiFi/BLE) but on
// a single-core RISC-V chip with the fleet-unique dual-band Wi-Fi 6
// capability (2.4 + 5 GHz). Pin defines come from platformio.ini
// build_flags (PIN_LCD_*, PIN_TOUCH_*, PIN_SD_*, PIN_I2C_*) and from
// include/hal_c5.h; this block is just the HAS_* capability summary
// that hal_pins.h's pm_has_*() helpers depend on.
#ifdef DEVICE_C5

  // Screen geometry mirrors hal_c5.h (portrait via software rotation
  // of the native 320×240 landscape ST7789 panel).
  #ifndef SCREEN_W
    #define SCREEN_W        240
  #endif
  #ifndef SCREEN_H
    #define SCREEN_H        320
  #endif
  #define SCREEN_DRIVER     ST7789

  // Capabilities — RockBase NM-CYD-C5
  #define HAS_TRACKBALL     0
  #define HAS_LORA          0
  #define HAS_GPS           0             // GPS sourced over PMU1 from P4 peer
  #define HAS_AUDIO         0             // no audio hardware on this build
  #define HAS_AUDIO_IN      0             // no microphone
  #define HAS_NFC           0
  #define HAS_IMU           0
  #define HAS_HAPTIC        0
  #define HAS_ENCODER       0
  #define HAS_RTC           0
  #define HAS_NRF24         0
  #define HAS_IOEXP         0
  #define HAS_TOUCH         1             // XPT2046 resistive (still touch)
  #define HAS_KEYBOARD      0
  #define HAS_BATTERY       0             // USB-powered

#endif // DEVICE_C5


// Default HAS_AUDIO_IN for boards that predate the split (they all
// have mics where they have audio). Maxine overrides to 0 above.
#ifndef HAS_AUDIO_IN
  #define HAS_AUDIO_IN      HAS_AUDIO
#endif


// ── RUNTIME CAPABILITY CHECKS ────────────────────────────
// Apps use these instead of device defines directly
// Launcher uses these to show/hide apps per device

static inline int pm_has_lora(void)    { return HAS_LORA;    }
static inline int pm_has_gps(void)     { return HAS_GPS;     }
static inline int pm_has_nfc(void)     { return HAS_NFC;     }
static inline int pm_has_imu(void)     { return HAS_IMU;     }
static inline int pm_has_haptic(void)  { return HAS_HAPTIC;  }
static inline int pm_has_audio(void)   { return HAS_AUDIO;   }
static inline int pm_has_audio_in(void){ return HAS_AUDIO_IN; }
static inline int pm_has_encoder(void) { return HAS_ENCODER; }
static inline int pm_has_rtc(void)     { return HAS_RTC;     }
static inline int pm_has_nrf24(void)   { return HAS_NRF24;   }
static inline int pm_has_ioexp(void)   { return HAS_IOEXP;   }
static inline int pm_has_touch(void)   { return HAS_TOUCH;   }
static inline int pm_has_keyboard(void){ return HAS_KEYBOARD;}
static inline int pm_has_battery(void) { return HAS_BATTERY; }

// ── PIN_I2S_* NAMING CONVENTION (v1.3 standardization) ──
//
// Two patterns exist, depending on the device's audio bus topology:
//
//   SINGLE-BUS devices — one codec speaks both TX (DAC out) and RX
//   (ADC in) on the same set of clocks. The codec switches direction
//   under our control; only one direction is active at a time.
//     C28P, T-LoRa Pager, Cardputer ADV, Maxine
//
//     PIN_I2S_MCLK   master clock (optional; -1 if codec self-clocks)
//     PIN_I2S_SCLK   bit clock (BCLK in some docs)
//     PIN_I2S_LRCK   word select / left-right clock (WS / LRC in some docs)
//     PIN_I2S_DOUT   data out (DAC samples → codec)
//     PIN_I2S_DIN    data in  (ADC samples ← codec)   // omitted if no mic
//
//   DUAL-BUS devices — separate output and input chips on separate
//   I2S peripherals, each with their own clock pins. Both buses can
//   in principle run simultaneously.
//     T-Deck Plus only
//
//     PIN_I2S_OUT_SCLK / _LRCK / _DOUT          (→ amp/speaker)
//     PIN_I2S_IN_MCLK / _SCLK / _LRCK / _DIN    (← mic codec)
//
// Files that touch the single-bus codec (game_audio.cpp, c28p_audio.cpp,
// nosql/wardrive/pager codec drivers) reference PIN_I2S_* directly.
// Files specific to T-Deck Plus's dual-bus topology (audio_player.cpp
// for output, audio_recorder.cpp for ES7210 input) use the OUT/IN-
// prefixed names. Older private constants like I2S_BCLK / ES7210_MCLK
// remain as file-local aliases over the standardized macros so the
// drivers' inline comments still read with their familiar spellings.
//
// New devices: pick whichever convention matches their topology. Do
// NOT mix — a single-bus device should not define PIN_I2S_OUT_*.
// ───────────────────────────────────────────────────────────

#endif // HAL_PINS_H