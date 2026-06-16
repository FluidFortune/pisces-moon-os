// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef HAL_C5_H
#define HAL_C5_H

#ifdef DEVICE_C5

// ============================================================
//  hal_c5.h — BSP for the NM-CYD-C5 (RockBase "Colorful")
//
//  Source: github.com/RockBase-iot/NM-CYD-C5 (pinout table)
//
//  Chip:    ESP32-C5-WROOM-1, single-core RISC-V @ 240 MHz
//           16 MB flash, 8 MB PSRAM
//           Dual-band Wi-Fi 6 (2.4 + 5 GHz) — fleet-unique
//           BLE 5.3 + IEEE 802.15.4 (Zigbee/Thread)
//
//  Display: 2.8" 240×320 ST7789 (also runnable as ILI9341)
//  Touch:   XPT2046 RESISTIVE on SPI (NOT capacitive)
//  Storage: microSD via SPI (shared with LCD + touch)
//
//  ── SHARED SPI BUS ──
//  The board wires the LCD, the XPT2046 touch IC, and the SD slot
//  to ONE SPI peripheral (SCK=6, MISO=2, MOSI=7) with three chip
//  selects. The SPI Bus Treaty (spi_treaty.h) is therefore as
//  load-bearing on the C5 as on the T-Deck Plus or Pager — every
//  pixel push, touch read, and SD write must take the mutex first.
//
//  This is the OPPOSITE of the C28P (where SD has a dedicated
//  SDMMC controller and SPI carries only the LCD). On the C5
//  every consumer is on one bus.
//
//  ⚠ TBD pins clearly marked below. Source the demo project at
//  ⚠ NM-CYD-C5/Demos/Arduino to confirm LCD DC/RST/BL and the
//  ⚠ touch IRQ + RGB LED GPIOs before first flash. Sensible
//  ⚠ CYD-family defaults are filled in so the build links.
// ============================================================

// ── Shared SPI bus (LCD + XPT2046 touch + SD) ─────────────
// Confirmed from the README pinout table. These come from
// platformio.ini build_flags (-DPIN_SPI_*); guards prevent
// the redefinition-vs-command-line warning.
#ifndef PIN_SPI_SCK
  #define PIN_SPI_SCK    6
#endif
#ifndef PIN_SPI_MISO
  #define PIN_SPI_MISO   2
#endif
#ifndef PIN_SPI_MOSI
  #define PIN_SPI_MOSI   7
#endif

// ── LCD ST7789 (240×320 portrait via software rotation) ───
// CS=23 confirmed. DC/RST/BL not in the README's pinout table —
// values below are the CYD-family convention; verify from demo.
#ifndef PIN_LCD_MOSI
  #define PIN_LCD_MOSI   PIN_SPI_MOSI
#endif
#ifndef PIN_LCD_MISO
  #define PIN_LCD_MISO   PIN_SPI_MISO
#endif
#ifndef PIN_LCD_SCK
  #define PIN_LCD_SCK    PIN_SPI_SCK
#endif
#ifndef PIN_LCD_CS
  #define PIN_LCD_CS     23
#endif
#ifndef PIN_LCD_DC
  #define PIN_LCD_DC     15    // ⚠ TBD — verify against NM-CYD-C5 demo
#endif
#ifndef PIN_LCD_RST
  #define PIN_LCD_RST    -1    // ⚠ TBD — likely tied to EN (-1); verify
#endif
#ifndef PIN_LCD_BL
  #define PIN_LCD_BL     27    // ⚠ TBD — verify (CYD convention is 27)
#endif

// ── XPT2046 resistive touch (SPI peripheral, shared bus) ──
// CS=1 confirmed. PENIRQ pin not in the README; touch IRQ not
// strictly required (poll-based reads work) but desirable.
#ifndef PIN_TOUCH_MOSI
  #define PIN_TOUCH_MOSI PIN_SPI_MOSI
#endif
#ifndef PIN_TOUCH_MISO
  #define PIN_TOUCH_MISO PIN_SPI_MISO
#endif
#ifndef PIN_TOUCH_SCK
  #define PIN_TOUCH_SCK  PIN_SPI_SCK
#endif
#ifndef PIN_TOUCH_CS
  #define PIN_TOUCH_CS   1
#endif
#ifndef PIN_TOUCH_IRQ
  #define PIN_TOUCH_IRQ  -1    // ⚠ TBD — optional, poll fallback works
#endif
// XPT2046 control bytes (SER/DFR mode bits standard):
#define XPT2046_CMD_READ_X  0xD0
#define XPT2046_CMD_READ_Y  0x90

// ── MicroSD (SPI, shared bus) ─────────────────────────────
#ifndef PIN_SD_MOSI
  #define PIN_SD_MOSI    PIN_SPI_MOSI
#endif
#ifndef PIN_SD_MISO
  #define PIN_SD_MISO    PIN_SPI_MISO
#endif
#ifndef PIN_SD_SCK
  #define PIN_SD_SCK     PIN_SPI_SCK
#endif
#ifndef PIN_SD_CS
  #define PIN_SD_CS      10
#endif

// ── LP-UART (P5 connector — GPS-ready, plug-and-play) ─────
// The board exposes a low-power UART on the "P5" header,
// pre-wired for the NM-ATGM336H GPS module. The handoff said
// "no GPS UART" — but the C5 board actually does have one.
// We can either: (a) plug in an ATGM336H and run a real GPS
// feed, or (b) re-purpose this UART for PMU1 to the P4 peer.
//   GPS:  RX=4 (C5 receives NMEA), TX=5 (C5 transmits to GPS)
#define PIN_GPS_RX     4
#define PIN_GPS_TX     5

// ── I2C — Extend IO (CN1) ─────────────────────────────────
// IO9 = SDA, IO8 = SCL (best read of the CN1 table — verify
// with a multimeter at first bring-up; the README lists "IO9
// IO8" in that pin order under the 4-pin connector).
#define PIN_I2C_SDA    9
#define PIN_I2C_SCL    8

// ── PMU1 UART (peer link to P4) ───────────────────────────
// Two viable paths on this board:
//   1. Use the LP-UART (P5 GPS connector) at GPIO 4/5 — easiest
//      physical wiring (already broken out), but blocks GPS.
//   2. Use the 12-pin FPC2 (IO26 is exposed on P1) plus another
//      free GPIO — keeps GPS available but harder physical wiring.
// Default to path 1 (GPS-free, simpler) and let the operator
// override via build_flags if they want GPS instead.
#define PMU1_UART_NUM  1
#define PMU1_BAUD      921600
#ifndef PIN_PMU1_TX
  #define PIN_PMU1_TX  PIN_GPS_TX   // GPIO 5
#endif
#ifndef PIN_PMU1_RX
  #define PIN_PMU1_RX  PIN_GPS_RX   // GPIO 4
#endif

// ── Extend IO / FPC2 (available GPIOs for expansion) ──────
// P1 4-pin: IO4, IO8, IO26, GND
// FPC2 12-pin: IO2, IO6, IO7, IO10, GND, IO4, IO8, IO5, IO9,
//              USB D-, USB D+, GND
// IO26 is the only GPIO not already routed on-board.
#define PIN_EXPAND_IO  26

// ── RGB LED (board has a status NeoPixel) ─────────────────
// arduino-esp32's c5 variant pins_arduino.h defines PIN_RGB_LED = 27
// by default. We don't override it here — if the actual board uses a
// different pin, set PIN_PM_RGB_LED via build_flags and reference
// that instead. Leaving PIN_RGB_LED to the variant avoids fighting
// arduino-esp32's RGBLed runtime helpers.

// ── BOOT button ───────────────────────────────────────────
// Standard ESP32-C5 module convention.
#ifndef PIN_BOOT_BTN
  #define PIN_BOOT_BTN   9     // shared with I2C_SDA — boot reads
                               // strap at reset, then released
#endif

// ── Screen geometry ───────────────────────────────────────
// Native panel: ST7789 240×320 portrait native GRAM. Driven at
// rotation 0 so gfx->width()=240, gfx->height()=320 matches the
// C28P interaction model.
//
// C5_PANEL_W / C5_PANEL_H describe the XPT2046 touch overlay's raw
// ADC axes, NOT the LCD orientation — the touch X-axis runs along
// the panel's long edge (320 px) and Y along the short edge (240 px),
// so we map raw_x→0..319, raw_y→0..239 and let c5_boot.cpp's
// C5_ROTATE_TO_PORTRAIT rotate 90° to land in 240×320 portrait coords.
#define C5_PANEL_W   320
#define C5_PANEL_H   240
#ifndef SCREEN_W
  #define SCREEN_W   240    // portrait logical
#endif
#ifndef SCREEN_H
  #define SCREEN_H   320
#endif

// ── Capability summary ────────────────────────────────────
// Defined in platformio.ini build_flags:
//   PM_FORM_FACTOR_KIOSK, PM_HAS_TOUCH, PM_HAS_WIFI_5GHZ,
//   PM_HAS_GPS (because the LP-UART is GPS-ready), PM_NO_LORA,
//   PM_NO_KEYBOARD, PM_NO_AUDIO_IN, PM_NO_AUDIO_OUT.

#endif // DEVICE_C5
#endif // HAL_C5_H
