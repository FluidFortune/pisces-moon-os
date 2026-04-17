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

// ============================================================
//  gamepad.h — Pisces Moon OS Gamepad Abstraction Layer
//  8BitDo Zero 2 BLE HID Driver + Unified Input API
// ============================================================
//
//  SPLIT ARCHITECTURE:
//    gamepad.h   — structs, constants, extern declaration, API surface
//    gamepad.cpp — g_gamepad definition + all function bodies
//
//  The global GamepadState g_gamepad is defined once in gamepad.cpp.
//  Any file that includes gamepad.h sees it via the extern declaration.
//
//  ESP32-S3 BLE Constraint:
//    Classic Bluetooth (BR/EDR) is NOT supported on ESP32-S3.
//    The Zero 2 MUST be in BLE mode.
//    Pairing: Hold SELECT + RIGHT shoulder for 3 seconds → LED blinks PURPLE
//    Purple = BLE/Android mode. Blue = Classic BT (will NOT connect).
//
//  NimBLE coexistence:
//    The wardrive subsystem already initializes NimBLE via init_wardrive_core().
//    gamepad_init() DOES NOT call NimBLEDevice::init() again.
//    It attaches a second scan callback to the existing NimBLE instance.
//    Wardrive BLE scanning and gamepad HID connection share the stack cleanly.
//
//  HOME button — OS contract:
//    gamepad_poll() intercepts HOME before any application sees it.
//    It returns true when HOME is pressed. The caller MUST exit to launcher.
//    No ELF module or game can override this behavior.
//
//  Input mapping — 8BitDo Zero 2:
//    D-Pad     → GP_UP / GP_DOWN / GP_LEFT / GP_RIGHT
//    A         → GP_A    (confirm / fire / jump)
//    B         → GP_B    (cancel / back)
//    X         → GP_X    (select / inventory)
//    Y         → GP_Y    (map / special)
//    L         → GP_L    (left shoulder)
//    R         → GP_R    (right shoulder)
//    START     → GP_START
//    SELECT    → GP_SELECT
//    HOME      → GP_HOME  (OS reserved — always returns to launcher)
//
// ============================================================

#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <Arduino.h>
#include <NimBLEDevice.h>

// ============================================================
//  Button bitmask constants
// ============================================================
#define GP_UP       (1 << 0)
#define GP_DOWN     (1 << 1)
#define GP_LEFT     (1 << 2)
#define GP_RIGHT    (1 << 3)
#define GP_A        (1 << 4)   // Confirm / fire / jump
#define GP_B        (1 << 5)   // Cancel / back
#define GP_X        (1 << 6)   // Inventory / select
#define GP_Y        (1 << 7)   // Map / special
#define GP_L        (1 << 8)   // Left shoulder
#define GP_R        (1 << 9)   // Right shoulder
#define GP_START    (1 << 10)
#define GP_SELECT   (1 << 11)
#define GP_HOME     (1 << 12)  // OS reserved — always returns to launcher

// ============================================================
//  GamepadState — polled each frame via gamepad_poll()
// ============================================================
struct GamepadState {
    uint16_t buttons;           // Currently held buttons (bitmask)
    uint16_t pressed;           // Buttons newly pressed this frame (edge)
    uint16_t released;          // Buttons released this frame (edge)
    int8_t   axis_lx;           // Left analog X: -127 to +127
    int8_t   axis_ly;           // Left analog Y: -127 to +127
    bool     connected;         // True while Zero 2 is connected
    uint32_t last_input_ms;     // millis() of last non-idle input
    char     device_name[32];   // BLE device name of connected controller
};

// ============================================================
//  Global singleton — DEFINED in gamepad.cpp, extern here.
//  Every .cpp that includes gamepad.h can read/write g_gamepad.
// ============================================================
extern GamepadState g_gamepad;

// ============================================================
//  Public API — bodies in gamepad.cpp
// ============================================================

// Initialize gamepad subsystem.
// Attaches BLE scan callbacks to the existing NimBLE instance.
// Call AFTER init_wardrive_core() (which initializes NimBLE).
// Loads saved MAC from SD for direct reconnect if available.
// Non-blocking — BLE connection happens asynchronously.
void gamepad_init();

// Poll gamepad state — call once per main loop / app loop iteration.
// Updates pressed/released edge detection.
// Checks for HOME button (OS intercept).
// Returns true if HOME was pressed — caller MUST return to launcher.
bool gamepad_poll();

// Force disconnect from the current controller.
// Use before deep sleep or when explicitly needed.
void gamepad_disconnect();

// Active pairing flow — blocking UI, call from SYSTEM → GAMEPAD → PAIR button.
// Shows on-screen instructions, scans 30s, connects, saves MAC to /gamepad.cfg.
// Returns true if pairing succeeded.
bool gamepad_pair();

// Returns true if a MAC address has been saved on SD (/gamepad.cfg).
// Use to decide whether to show "PAIRED" or "NOT PAIRED" in the setup screen.
bool gamepad_is_paired();

// Deletes the saved MAC from SD (/gamepad.cfg) and disconnects.
// Next boot will require full pairing via gamepad_pair() again.
void gamepad_forget();

// ============================================================
//  Inline convenience helpers — safe to inline in header
//  because they only read g_gamepad, they don't define storage.
// ============================================================

// True if any button in mask is currently held down
inline bool gamepad_held(uint16_t mask)     { return (g_gamepad.buttons  & mask) != 0; }

// True if any button in mask was just pressed this frame
inline bool gamepad_pressed(uint16_t mask)  { return (g_gamepad.pressed  & mask) != 0; }

// True if any button in mask was just released this frame
inline bool gamepad_released(uint16_t mask) { return (g_gamepad.released & mask) != 0; }

// D-pad directional helpers — map to trackball equivalents for existing apps
inline bool gamepad_up()     { return gamepad_held(GP_UP);    }
inline bool gamepad_down()   { return gamepad_held(GP_DOWN);  }
inline bool gamepad_left()   { return gamepad_held(GP_LEFT);  }
inline bool gamepad_right()  { return gamepad_held(GP_RIGHT); }

// Action helpers
inline bool gamepad_action() { return gamepad_pressed(GP_A);   } // Fire / confirm
inline bool gamepad_back()   { return gamepad_pressed(GP_B);   } // Cancel / back

// ============================================================
//  Retro input adapters — convert GamepadState to emulator
//  button byte formats. Used by retro_elf_pack.h.
// ============================================================

// NES button byte (Nofrendo format: active-high, standard NES order)
// Bit: 7=Right 6=Left 5=Down 4=Up 3=Start 2=Select 1=B 0=A
inline uint8_t gamepad_to_nes() {
    uint8_t b = 0;
    if (g_gamepad.buttons & GP_A)      b |= 0x01;
    if (g_gamepad.buttons & GP_B)      b |= 0x02;
    if (g_gamepad.buttons & GP_SELECT) b |= 0x04;
    if (g_gamepad.buttons & GP_START)  b |= 0x08;
    if (g_gamepad.buttons & GP_UP)     b |= 0x10;
    if (g_gamepad.buttons & GP_DOWN)   b |= 0x20;
    if (g_gamepad.buttons & GP_LEFT)   b |= 0x40;
    if (g_gamepad.buttons & GP_RIGHT)  b |= 0x80;
    return b;
}

// Game Boy button byte (Gnuboy format: active-high)
// Bit: 7=Down 6=Up 5=Left 4=Right 3=Start 2=Select 1=B 0=A
inline uint8_t gamepad_to_gb() {
    uint8_t b = 0;
    if (g_gamepad.buttons & GP_A)      b |= 0x01;
    if (g_gamepad.buttons & GP_B)      b |= 0x02;
    if (g_gamepad.buttons & GP_SELECT) b |= 0x04;
    if (g_gamepad.buttons & GP_START)  b |= 0x08;
    if (g_gamepad.buttons & GP_RIGHT)  b |= 0x10;
    if (g_gamepad.buttons & GP_LEFT)   b |= 0x20;
    if (g_gamepad.buttons & GP_UP)     b |= 0x40;
    if (g_gamepad.buttons & GP_DOWN)   b |= 0x80;
    return b;
}

// Atari 2600 joystick byte (Stella format: active-LOW, 0xFF = idle)
// Bit: 4=Fire 3=Right 2=Left 1=Down 0=Up (all active-low)
inline uint8_t gamepad_to_atari() {
    uint8_t b = 0xFF; // All bits high = no input
    if (g_gamepad.buttons & GP_UP)          b &= ~0x01;
    if (g_gamepad.buttons & GP_DOWN)        b &= ~0x02;
    if (g_gamepad.buttons & GP_LEFT)        b &= ~0x04;
    if (g_gamepad.buttons & GP_RIGHT)       b &= ~0x08;
    if (g_gamepad.buttons & (GP_A | GP_B))  b &= ~0x10; // Fire
    return b;
}

#endif // GAMEPAD_H