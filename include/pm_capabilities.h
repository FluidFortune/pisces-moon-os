// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  PM CAPABILITIES — Device Capability Descriptor
// ─────────────────────────────────────────────
//
// Pisces Moon is a modular OS. The same codebase runs on devices with
// very different hardware: keyboard handhelds, touch-only desk
// kiosks, devices with LoRa and GPS, devices without. The OS adapts
// to whatever the hardware provides.
//
// This header is the single source of truth for what each device
// can do. It resolves at compile time based on the -DDEVICE_*
// flag set in platformio.ini, and provides:
//
//   1. A const PMDeviceCapabilities struct that runtime code can
//      query to decide whether a feature is available.
//
//   2. A set of PM_HAS_* / PM_LACKS_* macros for code paths that
//      need compile-time decisions (saves flash on devices that
//      don't have a feature, since the code can be #ifdef'd out).
//
// The capability descriptor is also what the launcher uses to
// decide which apps to display. An app declares its requirements
// (e.g., needs_lora=true), the launcher compares against the
// current device, and apps that can't run are hidden.
//
// ENVELOPE — Pisces Moon targets ESP32-S3 family chipsets only.
// All current targets have WiFi + BLE + a color display + at least
// 320KB SRAM. Within that envelope, capabilities vary; this header
// describes the variation.

#ifndef PM_CAPABILITIES_H
#define PM_CAPABILITIES_H

#include <stdint.h>
#include <stdbool.h>

// ─────────────────────────────────────────────
//  FORM FACTOR — physical context the device lives in
// ─────────────────────────────────────────────
typedef enum {
    PM_FORM_HANDHELD,        // T-Deck Plus, T-LoRa Pager (pocket, battery, mobile)
    PM_FORM_KEYBOARD_DECK,   // Cardputer ADV (pocket-card with keyboard)
    PM_FORM_DESK_KIOSK,      // C28P (USB-powered desk fixture, stationary)
} PMFormFactor;

// ─────────────────────────────────────────────
//  CAPABILITY DESCRIPTOR — what this device has and lacks
// ─────────────────────────────────────────────
typedef struct {
    // ── Identity ──
    const char* device_id;       // "tdeck_plus", "tlorapager", etc.
    const char* device_label;    // Human-readable: "T-Deck Plus"
    const char* device_short;    // Short label: "T-Deck"

    // ── Form factor and power ──
    PMFormFactor form_factor;
    bool battery_powered;        // false if USB-only (C28P)

    // ── Display ──
    int  display_w;              // Native width in pixels
    int  display_h;              // Native height in pixels
    bool display_touch;          // Capacitive touchscreen present
    bool display_color;          // All current devices: true

    // ── Input ──
    bool input_keyboard;         // Physical QWERTY (T-Deck, Pager, Cardputer)
    bool input_trackball;        // T-Deck Plus only
    bool input_rotary;           // T-LoRa Pager only
    bool input_dpad;             // Hypothetical KodeDot
    bool input_touch;            // Same as display_touch — duplicated for clarity

    // ── Radios ──
    bool radio_wifi;             // All current: true
    bool radio_ble;              // All current: true
    bool radio_lora;             // T-Deck, Pager; Cardputer w/ header; C28P no
    bool radio_nfc;              // Pager only
    bool radio_ir;               // Cardputer only

    // ── I/O ──
    bool io_microphone;          // T-Deck, Pager, C28P (codec varies)
    bool io_speaker;             // T-Deck, Pager, C28P
    bool io_sd_spi;              // T-Deck, Pager, Cardputer (SPI-mode SD)
    bool io_sd_sdio;             // C28P (SDIO-mode SD — different driver)
    bool io_gps;                 // T-Deck, Pager; Cardputer optional; C28P no
    bool io_imu;                 // Pager (BHI260AP); others none
    bool io_haptic;              // Pager (DRV2605) only

    // ── Memory ──
    int  psram_mb;               // 0 on Cardputer ADV
    int  sram_kb;                // Approx free SRAM at idle
    int  flash_mb;               // Total flash partition space

    // ── Security posture (per-device defaults) ──
    bool ghost_partition_default; // T-Deck, Pager, Cardputer: true; C28P: false
                                  // (desk fixtures don't have the threat model
                                  // Ghost Partition is designed for)
} PMDeviceCapabilities;

// ─────────────────────────────────────────────
//  DEVICE-SPECIFIC DESCRIPTOR RESOLUTION
//  Exactly one DEVICE_* flag must be defined at compile time.
//  The descriptor is exported as a const variable named
//  pm_device_caps that runtime code can query.
// ─────────────────────────────────────────────

#if defined(DEVICE_TDECK_PLUS)

static const PMDeviceCapabilities pm_device_caps = {
    .device_id              = "tdeck_plus",
    .device_label           = "T-Deck Plus",
    .device_short           = "T-Deck",
    .form_factor            = PM_FORM_HANDHELD,
    .battery_powered        = true,
    .display_w              = 320,
    .display_h              = 240,
    .display_touch          = true,
    .display_color          = true,
    .input_keyboard         = true,
    .input_trackball        = true,
    .input_rotary           = false,
    .input_dpad             = false,
    .input_touch            = true,
    .radio_wifi             = true,
    .radio_ble              = true,
    .radio_lora             = true,
    .radio_nfc              = false,
    .radio_ir               = false,
    .io_microphone          = true,
    .io_speaker             = true,
    .io_sd_spi              = true,
    .io_sd_sdio             = false,
    .io_gps                 = true,
    .io_imu                 = false,
    .io_haptic              = false,
    .psram_mb               = 8,
    .sram_kb                = 320,
    .flash_mb               = 16,
    .ghost_partition_default = true,
};

#elif defined(DEVICE_TLORAPAGER)

static const PMDeviceCapabilities pm_device_caps = {
    .device_id              = "tlorapager",
    .device_label           = "T-LoRa Pager",
    .device_short           = "Pager",
    .form_factor            = PM_FORM_HANDHELD,
    .battery_powered        = true,
    .display_w              = 480,
    .display_h              = 222,
    .display_touch          = false,
    .display_color          = true,
    .input_keyboard         = true,
    .input_trackball        = false,
    .input_rotary           = true,
    .input_dpad             = false,
    .input_touch            = false,
    .radio_wifi             = true,
    .radio_ble              = true,
    .radio_lora             = true,
    .radio_nfc              = true,
    .radio_ir               = false,
    .io_microphone          = true,
    .io_speaker             = true,
    .io_sd_spi              = true,
    .io_sd_sdio             = false,
    .io_gps                 = true,
    .io_imu                 = true,
    .io_haptic              = true,
    .psram_mb               = 8,
    .sram_kb                = 320,
    .flash_mb               = 16,
    .ghost_partition_default = true,
};

#elif defined(DEVICE_CARDPUTER_ADV)

static const PMDeviceCapabilities pm_device_caps = {
    .device_id              = "cardputer_adv",
    .device_label           = "Cardputer ADV",
    .device_short           = "Cardputer",
    .form_factor            = PM_FORM_KEYBOARD_DECK,
    .battery_powered        = true,
    .display_w              = 240,
    .display_h              = 135,
    .display_touch          = false,
    .display_color          = true,
    .input_keyboard         = true,
    .input_trackball        = false,
    .input_rotary           = false,
    .input_dpad             = false,
    .input_touch            = false,
    .radio_wifi             = true,
    .radio_ble              = true,
    .radio_lora             = true,    // with optional LoRa cap header
    .radio_nfc              = false,
    .radio_ir               = true,
    .io_microphone          = false,   // Cardputer has no mic
    .io_speaker             = true,    // PWM buzzer
    .io_sd_spi              = true,
    .io_sd_sdio             = false,
    .io_gps                 = false,   // optional cap not currently used
    .io_imu                 = false,
    .io_haptic              = false,
    .psram_mb               = 0,       // The hard target — NO PSRAM
    .sram_kb                = 320,
    .flash_mb               = 8,
    .ghost_partition_default = true,
};

#elif defined(DEVICE_C28P)

static const PMDeviceCapabilities pm_device_caps = {
    .device_id              = "c28p",
    .device_label           = "C28P Display",
    .device_short           = "C28P",
    .form_factor            = PM_FORM_DESK_KIOSK,
    .battery_powered        = false,   // USB-powered desk fixture
    .display_w              = 240,
    .display_h              = 320,     // native portrait
    .display_touch          = true,
    .display_color          = true,
    .input_keyboard         = false,
    .input_trackball        = false,
    .input_rotary           = false,
    .input_dpad             = false,
    .input_touch            = true,
    .radio_wifi             = true,
    .radio_ble              = true,
    .radio_lora             = false,
    .radio_nfc              = false,
    .radio_ir               = false,
    .io_microphone          = true,    // C28P highlight feature
    .io_speaker             = true,    // C28P highlight feature
    .io_sd_spi              = false,
    .io_sd_sdio             = true,    // SDIO mode, different driver
    .io_gps                 = false,
    .io_imu                 = false,
    .io_haptic              = false,
    .psram_mb               = 8,
    .sram_kb                = 320,
    .flash_mb               = 16,
    .ghost_partition_default = false,  // Desk fixtures don't fit the GP threat model
};

#else
#error "Pisces Moon: no DEVICE_* flag defined. Set one of DEVICE_TDECK_PLUS, DEVICE_TLORAPAGER, DEVICE_CARDPUTER_ADV, or DEVICE_C28P in platformio.ini."
#endif

// ─────────────────────────────────────────────
//  APP REQUIREMENTS — declared by each app
// ─────────────────────────────────────────────
typedef struct {
    bool needs_keyboard;
    bool needs_touch;
    bool needs_trackball;
    bool needs_rotary;
    bool needs_dpad;
    bool needs_lora;
    bool needs_gps;
    bool needs_nfc;
    bool needs_microphone;
    bool needs_speaker;
    bool needs_imu;
    int  min_psram_mb;       // 0 = no PSRAM requirement
    int  min_sram_kb;        // 0 = no minimum
    int  min_display_w;      // 0 = no minimum
    int  min_display_h;      // 0 = no minimum
} PMAppRequirements;

// ─────────────────────────────────────────────
//  CAPABILITY CHECK — does the current device satisfy
//  the given app's requirements?
//
//  Used by the launcher to decide whether to display an
//  app's tile. Used by apps themselves at init to refuse
//  to run gracefully (rather than crash) if the device
//  doesn't meet requirements.
// ─────────────────────────────────────────────
static inline bool pm_caps_supports(const PMAppRequirements* req) {
    const PMDeviceCapabilities* d = &pm_device_caps;
    if (req->needs_keyboard   && !d->input_keyboard) return false;
    if (req->needs_touch      && !d->input_touch)    return false;
    if (req->needs_trackball  && !d->input_trackball)return false;
    if (req->needs_rotary     && !d->input_rotary)   return false;
    if (req->needs_dpad       && !d->input_dpad)     return false;
    if (req->needs_lora       && !d->radio_lora)     return false;
    if (req->needs_gps        && !d->io_gps)         return false;
    if (req->needs_nfc        && !d->radio_nfc)      return false;
    if (req->needs_microphone && !d->io_microphone)  return false;
    if (req->needs_speaker    && !d->io_speaker)     return false;
    if (req->needs_imu        && !d->io_imu)         return false;
    if (req->min_psram_mb     >  d->psram_mb)        return false;
    if (req->min_sram_kb      >  d->sram_kb)         return false;
    if (req->min_display_w    >  d->display_w)       return false;
    if (req->min_display_h    >  d->display_h)       return false;
    return true;
}

// ─────────────────────────────────────────────
//  CONVENIENCE MACROS
//  For code paths that need compile-time decisions
//  (e.g., #ifdef'ing out an entire driver when the
//  device doesn't have that hardware).
// ─────────────────────────────────────────────

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER)
    #define PM_HAS_GPS 1
#endif

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)
    #define PM_HAS_LORA 1
    #define PM_HAS_KEYBOARD 1
#endif

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_C28P)
    #define PM_HAS_TOUCH 1
#endif

#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_C28P)
    #define PM_HAS_MIC 1
    #define PM_HAS_SPEAKER 1
#endif

#if defined(DEVICE_TLORAPAGER)
    #define PM_HAS_NFC 1
    #define PM_HAS_ROTARY 1
    #define PM_HAS_IMU 1
    #define PM_HAS_HAPTIC 1
#endif

#if defined(DEVICE_TDECK_PLUS)
    #define PM_HAS_TRACKBALL 1
#endif

#if defined(DEVICE_CARDPUTER_ADV)
    #define PM_NO_PSRAM 1
    #define PM_HAS_IR 1
#endif

#if defined(DEVICE_C28P)
    #define PM_FORM_KIOSK 1
    #define PM_NO_BATTERY 1
    #define PM_SD_SDIO 1
#endif

#endif // PM_CAPABILITIES_H