#ifndef INPUT_H
#define INPUT_H

#include "hal.h"

// ═══════════════════════════════════════════════════════════
//  UNIFIED INPUT ABSTRACTION
//  All navigation in Pisces Moon apps should call these
//  functions, not read GPIO directly. This makes the OS
//  portable across T-Deck (trackball), FrankenDot (BLE HID),
//  and Kode Dot (D-Pad GPIO) without changing any app code.
// ═══════════════════════════════════════════════════════════

struct InputEvent {
    enum Type { NONE, UP, DOWN, LEFT, RIGHT, SELECT, BACK, MENU } type;
};

// Call once in setup()
void input_init();

// Call in app loop — returns next pending event (non-blocking)
InputEvent input_poll();

// ─────────────────────────────────────────────
//  8BITDO ZERO 2 BLE HID MAPPING (FrankenDot)
//  D-Pad → UP/DOWN/LEFT/RIGHT
//  A button → SELECT
//  B button → BACK  
//  Start    → MENU
// ─────────────────────────────────────────────
#ifdef INPUT_BLE_GAMEPAD
  // BLE HID client implementation in input.cpp
  // Uses NimBLE-Arduino (already in lib_deps)
  // 8BitDo Zero 2 pairs as standard BLE HID gamepad
  // HID report byte 0: hat switch (D-Pad)
  // HID report byte 1: buttons bitmask
  extern bool ble_gamepad_connected;
#endif

// ─────────────────────────────────────────────
//  TRACKBALL GPIO (T-Deck Plus)
// ─────────────────────────────────────────────
#ifdef INPUT_TRACKBALL
  // Thin wrapper over existing trackball.h
  // Debounce handled inside input_poll()
#endif

#endif