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

#include <Wire.h>
#include "keyboard.h"
#include "hal.h"
#include "pm_input.h"

// ============================================================
//  KEYBOARD INPUT
//
//  T-Deck Plus  : I2C keyboard at 0x55, returns ASCII per byte.
//                 Simple one-byte protocol; key code = ASCII.
//
//  T-LoraPager  : TCA8418 keypad matrix controller at 0x34.
//                 Reads scan codes from FIFO register 0x04,
//                 maps to ASCII via a keymap table.
//                 Bit 7 of scan code = press(1)/release(0);
//                 we only emit on press events.
//
//  Cardputer ADV: TCA8418 keypad scanner at 0x34 wrapped by the
//                 M5Cardputer library (1.1.1+). We translate the
//                 library's KeysState into single-char events.
//                 Fn-layer maps `; , . /` to arrow keys and Fn+`
//                 to ESC, matching the canonical Cardputer convention.
//
//  All paths return the same ASCII char interface so the rest
//  of the OS (terminals, notepad, voice-terminal, etc) works
//  unchanged across devices.
// ============================================================

#ifdef DEVICE_TDECK_PLUS
// ── T-DECK PLUS — I2C keyboard at 0x55 ───────────────────
#ifndef KEYBOARD_ADDR
#define KEYBOARD_ADDR 0x55
#endif

void init_keyboard() {
    Wire.beginTransmission(KEYBOARD_ADDR);
    if (Wire.endTransmission() == 0) {
        Serial.println("KB: Online (T-Deck @ 0x55).");
    } else {
        Serial.println("KB: Sync Error (T-Deck @ 0x55).");
    }
}

char get_keypress() {
    Wire.requestFrom(KEYBOARD_ADDR, 1);
    if (Wire.available()) {
        uint8_t key = Wire.read();
        if (key == 0) return 0;
        // Standard ASCII Mapping
        if (key == 0x08) return 8;   // Backspace
        if (key == 0x0D) return 13;  // Enter
        if (key == 0x1B) return 27;  // ESC/SYM
        return (char)key;
    }
    return 0;
}
#endif // DEVICE_TDECK_PLUS


#ifdef DEVICE_TLORAPAGER
// ── T-LORA PAGER — TCA8418 matrix keypad at 0x34 ─────────
//
// TCA8418 register map (subset we use):
//   0x01  CFG          — config: enable interrupts, etc
//   0x02  INT_STAT     — interrupt status (we don't use IRQ)
//   0x03  KEY_LCK_EC   — lock & event count
//   0x04  KEY_EVENT_A  — FIFO read; one byte per keypress event
//   0x1D  KP_GPIO_1    — pins as keypad rows (P00-P07)
//   0x1E  KP_GPIO_2    — pins as keypad rows (P10-P17)
//   0x1F  KP_GPIO_3    — pins as keypad rows (P20-P27)
//
// KEY_EVENT_A byte format:
//   bit 7        : 1 = press, 0 = release
//   bits 6..0    : key code (1-80 valid)
//
// Key code = row*10 + col + 1 (per datasheet)
//
// T-LoraPager wires the keyboard as a 7-row × 8-col matrix
// driven by TCA8418's first 7 ROW pins and first 8 COL pins.
// We configure those as keypad inputs via KP_GPIO registers.

#define TCA8418_ADDR        0x34
#define TCA8418_REG_CFG     0x01
#define TCA8418_REG_INT_STA 0x02
#define TCA8418_REG_KEY_LCK 0x03
#define TCA8418_REG_KEY_EVT 0x04
#define TCA8418_REG_KP_GPIO_1 0x1D
#define TCA8418_REG_KP_GPIO_2 0x1E
#define TCA8418_REG_KP_GPIO_3 0x1F

// Modifier-key state (sticky until next non-modifier press).
static bool kb_sym_held   = false;   // orange/$ key — secondary symbols
static bool kb_caps_lock  = false;

// ── Keymap (verbatim from LilyGoLib LilyGo_LoRa_Pager.cpp) ─
// LilyGo defines a 4-row x 10-col matrix:
//   Row 0: q w e r t y u i o p
//   Row 1: a s d f g h j k l ENTER
//   Row 2: \0 z x c v b n m \0 \0
//   Row 3: SPACE \0 \0 \0 \0 \0 \0 \0 \0 \0
// And these special TCA8418 scan codes (their raw byte values):
//   symbol_key_value = 0x1E  (SYM modifier — produces symbol_map char)
//   alt_key_value    = 0x14  (ALT modifier)
//   caps_key_value   = 0x1C  (CAPS LOCK toggle)
//   char_b_value     = 0x19  (special B / probably duplicate B)
//   backspace_value  = 0x1D  (backspace)
//
// Scan codes use TCA8418 standard: row*10 + col + 1.
// Row 0 (Q row): scan codes 1-10
// Row 1 (A row): scan codes 11-20
// Row 2 (Z row): scan codes 21-30
// Row 3 (SPACE row): scan codes 31-40
//
// Normal keymap (no modifier) and symbol map (with SYM held):
static const char kb_keymap[4][10] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\n'},
    { 0 , 'z', 'x', 'c', 'v', 'b', 'n', 'm',  0 ,  0 },
    {' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 }
};
static const char kb_symbol_map[4][10] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'},
    {'*', '/', '+', '-', '=', ':','\'', '"', '@',  0 },
    { 0 , '_', '$', ';', '?', '!', ',', '.',  0 ,  0 },
    {' ',  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 ,  0 }
};

// LilyGo's named-key raw scan code values (FROM LilyGoKeyboardConfigure_t)
// IMPORTANT: These are the values AFTER LilyGo's internal `k--` decrement
// in their update() function. The raw TCA8418 FIFO byte is `value + 1`.
// We replicate their flow exactly to match real hardware behavior.
//
// HARDWARE NOTE: LilyGo's struct labels two separate keys, "symbol_key"
// (0x1E) and "alt_key" (0x14), as if both physically exist. On T-LoraPager
// hardware only ONE secondary-layer modifier exists — the small orange
// key at the bottom-left. It emits the value LilyGo labels "alt_key"
// (raw 0x15 → decrement 0x14), but its function is the symbol/numeric
// layer toggle. There is no separate ALT key on this keyboard. We treat
// 0x14 as SYM here regardless of LilyGo's struct naming.
#define KB_SYM_VAL        0x14    // Orange key — secondary character layer
#define KB_CAPS_VAL       0x1C    // CAP key — uppercase lock (also Shift)
#define KB_BACKSPACE_VAL  0x1D    // Backspace
#define KB_CHAR_B_VAL     0x19    // Special B handling (rarely used)

static void tca_write_reg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(TCA8418_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static uint8_t tca_read_reg(uint8_t reg) {
    Wire.beginTransmission(TCA8418_ADDR);
    Wire.write(reg);
    // CRITICAL: pass `false` to send a repeated START rather than STOP.
    // TCA8418 (and most I2C devices) require this between writing the
    // register pointer and reading the value. With STOP+START the device
    // loses the register pointer and the read returns garbage or 0.
    Wire.endTransmission(false);
    Wire.requestFrom(TCA8418_ADDR, 1);
    return Wire.available() ? Wire.read() : 0;
}

void init_keyboard() {
    // Ping the TCA8418 — read CFG register, expect a response.
    Wire.beginTransmission(TCA8418_ADDR);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Serial.printf("KB: TCA8418 not responding at 0x34 (err=%d)\n", err);
        return;
    }
    Serial.println("KB: TCA8418 detected at 0x34");

    // Read current config so we can see what state the chip is in.
    uint8_t cfg_before = tca_read_reg(TCA8418_REG_CFG);
    Serial.printf("KB: CFG register before init = 0x%02X\n", cfg_before);

    // Configure matrix per LilyGo's keyboardConfig:
    //   kb_rows = 4, kb_cols = 10
    // KP_GPIO_1 (rows R0-R7):     bits 0-3 = R0-R3 active     => 0x0F
    // KP_GPIO_2 (cols C0-C7):     bits 0-7 = C0-C7 active     => 0xFF
    // KP_GPIO_3 (cols C8-C9):     bits 0-1 = C8-C9 active     => 0x03
    tca_write_reg(TCA8418_REG_KP_GPIO_1, 0x0F);  // 4 rows
    tca_write_reg(TCA8418_REG_KP_GPIO_2, 0xFF);  // cols C0-C7
    tca_write_reg(TCA8418_REG_KP_GPIO_3, 0x03);  // cols C8-C9

    // Clear any stale events from FIFO.
    uint8_t lck = tca_read_reg(TCA8418_REG_KEY_LCK);
    Serial.printf("KB: stale event count = %d\n", lck & 0x0F);
    while (tca_read_reg(TCA8418_REG_KEY_LCK) & 0x0F) {
        (void)tca_read_reg(TCA8418_REG_KEY_EVT);
    }

    // Enable keypad with auto-increment and overflow reset.
    // Bit 0 = KE_IEN (key events generate interrupts — also populates FIFO)
    // Bit 5 = INT_CFG (level-triggered)
    tca_write_reg(TCA8418_REG_CFG, 0x21);

    uint8_t cfg_after = tca_read_reg(TCA8418_REG_CFG);
    Serial.printf("KB: CFG register after init = 0x%02X\n", cfg_after);
    Serial.println("KB: Online (T-LoraPager TCA8418 @ 0x34).");
}

char get_keypress() {
    // Quick check: any events pending?
    uint8_t lck = tca_read_reg(TCA8418_REG_KEY_LCK);
    uint8_t count = lck & 0x0F;
    if (count == 0) return 0;

    // Read one event from the FIFO.
    uint8_t evt = tca_read_reg(TCA8418_REG_KEY_EVT);
    if (evt == 0) return 0;

    bool pressed = (evt & 0x80) != 0;
    uint8_t k = evt & 0x7F;

    // Diagnostic — keep on for now until layout confirmed
    Serial.printf("[KB] raw=0x%02X (%d) %s\n", k, k, pressed ? "DOWN" : "UP");

    // Skip GPIO-style events (raw > 96 means non-matrix pin event).
    if (k > 96) return 0;

    // ── LilyGo's k-- decrement step ────────────────────────
    // After this, k is the matrix index. row = k/10, col = k%10.
    // Special-key values (alt, caps, etc) are compared against
    // this DECREMENTED k, not the raw FIFO byte.
    k--;

    // Reject out-of-range matrix indices.
    if (k / 10 >= 4) return 0;

    // ── Handle special keys (modifier toggles) ─────────────
    // Orange key — SYM (symbol/numeric) layer, held-only modifier.
    // Unlike CAPS_LOCK below, SYM is NOT a sticky toggle: it's
    // engaged only while the orange key is physically held. This
    // matches the behavior of Shift on a conventional keyboard and
    // is what muscle memory expects from any modern handheld.
    //
    // Press → kb_sym_held = true
    // Release → kb_sym_held = false
    //
    // Previously this was implemented as a sticky toggle (one press
    // to engage, another to disengage), inherited from LilyGo's
    // reference implementation. That created a class of bug where
    // returning from one screen with SYM toggled on left it engaged
    // in the next screen — most visibly, the PIN screen left SYM
    // engaged on exit, so the launcher saw every key as its symbol
    // layer remap and ignored all navigation input.
    if (k == KB_SYM_VAL) {
        kb_sym_held = pressed;
        return 0;
    }
    if (k == KB_CAPS_VAL) {
        // CAPS_LOCK remains a sticky toggle (standard convention).
        if (pressed) kb_caps_lock = !kb_caps_lock;
        return 0;
    }
    if (k == KB_BACKSPACE_VAL) {
        if (pressed) return 8;
        return 0;
    }
    if (k == KB_CHAR_B_VAL) {
        if (pressed) return kb_caps_lock ? 'B' : 'b';
        return 0;
    }

    // Ignore release events for normal keys (we already emitted on press).
    if (!pressed) return 0;

    // ── Matrix lookup ──────────────────────────────────────
    uint8_t row = k / 10;
    uint8_t col = k % 10;

    // Control characters (Enter, Space) are layer-independent —
    // they should emit the same character whether SYM is engaged
    // or not. Check the normal keymap first for these.
    char normalChar = kb_keymap[row][col];
    if (normalChar == '\n' || normalChar == ' ') {
        // Convert '\n' (LF) to 13 (CR) for PIN screen, Gemini terminal,
        // and other apps that check for CR.
        return (normalChar == '\n') ? 13 : ' ';
    }

    char c;
    if (kb_sym_held) {
        c = kb_symbol_map[row][col];
        // SYM is a held-only modifier (like Shift). The orange key
        // must be physically held down for the symbol layer to be
        // active. Release it and the next keypress reverts to the
        // normal map.
    } else {
        c = normalChar;
        if (c >= 'a' && c <= 'z' && kb_caps_lock) {
            c = c - 'a' + 'A';
        }
    }

    return c;
}
#endif // DEVICE_TLORAPAGER


#ifdef DEVICE_CARDPUTER_ADV
// ── M5STACK CARDPUTER ADV — M5Cardputer-wrapped TCA8418 ──
//
// The Cardputer ADV uses a TCA8418 I2C keypad scanner at 0x34
// (same chip family as the T-LoraPager) but with a different
// 56-key physical layout and the M5Cardputer library handling
// scancode → ASCII translation, shift/caps resolution, and Fn
// modifier tracking.
//
// We translate M5Cardputer's KeysState struct into single-char
// events matching the get_keypress() contract used by every app.
// The library's update() must be called frequently — the chip
// has a 10-event FIFO and long blocking calls cause silent drops.
//
// Fn layer mapping (matches Cardputer keycap labels):
//   Fn + `       → 27   (PM_KEY_ESC)
//   Fn + ;       → 0x81 (PM_KEY_UP)
//   Fn + ,       → 0x83 (PM_KEY_LEFT)
//   Fn + .       → 0x82 (PM_KEY_DOWN)
//   Fn + /       → 0x84 (PM_KEY_RIGHT)
//   Fn + Bksp    → 0x89 (PM_KEY_DEL — forward delete)
//
// Plain (non-Fn) keys:
//   letters / numbers / symbols → ASCII (shift/caps-resolved)
//   Enter   → 13
//   Bksp    → 8
//   Tab     → 9
//   Space   → ' '
//
// Library version requirement: M5Cardputer 1.1.1 minimum.
// Version 1.1.0 has a known bug where z/c/b/m don't uppercase
// correctly. Pin in platformio.ini lib_deps.

#include <M5Cardputer.h>

#define CP_KB_FIFO_DEPTH 8
static char    cp_kb_fifo[CP_KB_FIFO_DEPTH];
static uint8_t cp_kb_fifo_head = 0;
static uint8_t cp_kb_fifo_tail = 0;
static uint8_t cp_kb_fifo_count = 0;
static bool    cp_kb_initialized = false;
static bool    cp_kb_prev_pressed = false;

// Push one char into the FIFO. Silent drop on overflow — the
// FIFO depth matches the TCA8418's hardware queue so overflow
// only happens under truly pathological load (apps that block
// for hundreds of ms while keys are being mashed).
static void cp_kb_push(char c) {
    if (c == 0) return;
    if (cp_kb_fifo_count >= CP_KB_FIFO_DEPTH) return;
    cp_kb_fifo[cp_kb_fifo_head] = c;
    cp_kb_fifo_head = (cp_kb_fifo_head + 1) % CP_KB_FIFO_DEPTH;
    cp_kb_fifo_count++;
}

// Pop one char from the FIFO; returns 0 if empty.
static char cp_kb_pop() {
    if (cp_kb_fifo_count == 0) return 0;
    char c = cp_kb_fifo[cp_kb_fifo_tail];
    cp_kb_fifo_tail = (cp_kb_fifo_tail + 1) % CP_KB_FIFO_DEPTH;
    cp_kb_fifo_count--;
    return c;
}

// Translate one M5 KeysState rising edge into 0..N FIFO pushes.
// Called only when isChange() && isPressed() && !was_pressed_before.
static void cp_kb_translate(const Keyboard_Class::KeysState &s) {
    // ── Diagnostic logging — visible in Serial monitor ──────
    // Reports raw KeysState contents on every detected keypress.
    // Helps verify M5Cardputer library is correctly interpreting
    // hardware events. Comment out once Cardputer input is stable.
    Serial.printf("[CP-KB] fn=%d enter=%d del=%d tab=%d shift=%d ctrl=%d opt=%d alt=%d ",
                  s.fn ? 1 : 0,
                  s.enter ? 1 : 0,
                  s.del ? 1 : 0,
                  s.tab ? 1 : 0,
                  s.shift ? 1 : 0,
                  s.ctrl ? 1 : 0,
                  s.opt ? 1 : 0,
                  s.alt ? 1 : 0);
    Serial.print("word=[");
    for (auto c : s.word) {
        if (c >= 32 && c < 127) Serial.print(c);
        else                    Serial.printf("\\x%02X", (unsigned char)c);
    }
    Serial.println("]");

    // Fn layer takes priority — arrows, ESC, forward delete.
    // We skip text input entirely when Fn is held.
    if (s.fn) {
        for (auto c : s.word) {
            if (c == '`') { cp_kb_push(PM_KEY_ESC);   Serial.println("[CP-KB] → ESC"); return; }
            if (c == ';') { cp_kb_push(PM_KEY_UP);    Serial.println("[CP-KB] → UP"); return; }
            if (c == ',') { cp_kb_push(PM_KEY_LEFT);  Serial.println("[CP-KB] → LEFT"); return; }
            if (c == '.') { cp_kb_push(PM_KEY_DOWN);  Serial.println("[CP-KB] → DOWN"); return; }
            if (c == '/') { cp_kb_push(PM_KEY_RIGHT); Serial.println("[CP-KB] → RIGHT"); return; }
        }
        if (s.del) { cp_kb_push(PM_KEY_DEL); Serial.println("[CP-KB] → DEL"); return; }
        // Fn alone or unmapped Fn combos: drop.
        Serial.println("[CP-KB] → (fn dropped)");
        return;
    }

    // Special non-printable keys. Each has a boolean flag and
    // does NOT appear in word; that's the library's contract.
    if (s.enter) { cp_kb_push(PM_KEY_ENTER);     Serial.println("[CP-KB] → ENTER"); return; }
    if (s.del)   { cp_kb_push(PM_KEY_BACKSPACE); Serial.println("[CP-KB] → BACKSPACE"); return; }
    if (s.tab)   { cp_kb_push(PM_KEY_TAB);       Serial.println("[CP-KB] → TAB"); return; }

    // Printable text input — chars in word are already
    // shift/ctrl/caps-resolved by the library.
    for (auto c : s.word) {
        if (c >= 32 && c < 127) {
            cp_kb_push(c);
            Serial.printf("[CP-KB] → '%c' (0x%02X)\n", c, (unsigned char)c);
        }
    }
}

void init_keyboard() {
    if (cp_kb_initialized) {
        Serial.println("[CP-KB] init_keyboard() called but already initialized");
        return;
    }
    Serial.println("[CP-KB] init_keyboard() starting — calling M5Cardputer.begin()");
    auto cfg = M5.config();
    // Keep M5Unified from reinitializing USB-CDC after Arduino has
    // already brought it up via ARDUINO_USB_CDC_ON_BOOT.
    cfg.serial_baudrate = 0;
    M5Cardputer.begin(cfg, true);   // true = enable keyboard
    cp_kb_initialized = true;
    Serial.println("[CP-KB] M5Cardputer.begin() returned");
    Serial.println("KB: Online (Cardputer ADV M5Cardputer @ 0x34).");
}

char get_keypress() {
    // Lazy init in case setup() didn't call init_keyboard().
    if (!cp_kb_initialized) {
        Serial.println("[CP-KB] Lazy init triggered from get_keypress()");
        auto cfg = M5.config();
        cfg.serial_baudrate = 0;
        M5Cardputer.begin(cfg, true);
        cp_kb_initialized = true;
    }

    // Always poll the M5 library first so the TCA8418 FIFO stays
    // current. Without this the chip's 10-event queue overflows
    // silently during fast typing.
    M5Cardputer.update();
    if (M5Cardputer.Keyboard.isChange()) {
        bool nowPressed = M5Cardputer.Keyboard.isPressed();
        Serial.printf("[CP-KB] isChange=1 nowPressed=%d prevPressed=%d\n",
                      nowPressed ? 1 : 0, cp_kb_prev_pressed ? 1 : 0);
        if (nowPressed && !cp_kb_prev_pressed) {
            // Rising edge — translate the current key state.
            Keyboard_Class::KeysState s = M5Cardputer.Keyboard.keysState();
            cp_kb_translate(s);
        }
        cp_kb_prev_pressed = nowPressed;
    }

    return cp_kb_pop();
}
#endif // DEVICE_CARDPUTER_ADV
