// pm_shell — Pisces Moon Sovereign Shell Firmware
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#pragma once

// ─────────────────────────────────────────────
//  pm_shell.h — Public API for the Lua REPL task
//
//  This header is intentionally minimal. The REPL is self-contained
//  inside its own FreeRTOS task — main.cpp just calls pm_shell_setup()
//  once during boot and then can sleep forever.
//
//  No Pisces Moon dependencies. This firmware is independent.
// ─────────────────────────────────────────────

#ifdef __cplusplus
extern "C" {
#endif

// Spawn the shell task. Called once from main.cpp's setup() after
// Serial.begin() has completed and any USB-CDC enumeration delay
// has elapsed.
//
// Side effects:
//   - Creates a single lua_State for the lifetime of the firmware
//   - Registers all pm.* bindings into the global table
//   - Overrides Lua's default print() to write through Serial
//   - Spawns "PmShell" task pinned to core 0 with 12 KB stack
//   - Prints the welcome banner over Serial
//
// Idempotent: calling twice is a no-op (logs a warning the second
// time and returns).
void pm_shell_setup();

// Run a single Lua statement directly from C code. Useful for boot-
// time autorun (e.g. an /scripts/init.lua hook in a later phase) and
// for tests. Returns true on success, false on Lua error. Errors are
// printed to Serial automatically; the lua_State remains usable for
// subsequent calls.
//
// Safe to call from the shell task itself OR from setup(), but NOT
// from interrupt context (Lua's GC is not ISR-safe).
bool pm_shell_eval(const char* lua_source);

// Returns true once the shell task has finished registering bindings
// and is reading from Serial. Useful for main.cpp diagnostics.
bool pm_shell_ready();

#ifdef __cplusplus
}
#endif
