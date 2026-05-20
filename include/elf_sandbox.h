// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com

#ifndef ELF_SANDBOX_H
#define ELF_SANDBOX_H

// ============================================================
//  ELF SANDBOX — Hardware-Enforced ELF App Isolation
//  Pisces Moon OS v1.2
// ============================================================
//
//  HARDWARE: ESP32-S3 Permission Management System (PMS)
//  + World Controller (WCNTL)
//  ──────────────────────────────────────────────────────────
//  The ESP32-S3 uses the Xtensa LX7 core with Espressif's
//  Permission Management System (PMS) — their equivalent of
//  an ARM MPU. Not ARM terminology — Xtensa/Espressif specific.
//
//  World Controller provides two execution environments:
//    World 0 — Secure (OS core, Ghost Engine, SPI Treaty)
//    World 1 — Non-secure (ELF module execution context)
//
//  THE EXCEPTION HANDLER APPROACH
//  ──────────────────────────────────────────────────────────
//  Gemini correctly identified the critical trap: ESP-IDF's
//  panic handler intercepts ALL CPU exceptions and reboots.
//
//  Our solution: xt_set_exception_handler() registers a
//  custom handler at the Xtensa exception level, BELOW the
//  ESP-IDF panic handler. When a PMS violation fires
//  EXCCAUSE 28 (LoadStoreError):
//
//    1. Our handler runs first (before panic handler)
//    2. Check: is fault PC inside ELF PSRAM region?
//       YES → ELF bug: delete ELF task, record fault, return
//       NO  → OS bug: pass to original panic handler (reboot)
//    3. Ghost Engine on Core 0 is completely unaffected
//
//  This is why nobody else has shipped this on ESP32 — the
//  exception hijack is the hard part, not the PMS config.
//
//  LIMITATIONS (honest)
//  ──────────────────────────────────────────────────────────
//  - Read isolation: PMS write-protects OS SRAM. ELF can
//    still READ any address. No MMU = no virtual address
//    spaces. Protects against buggy ELFs, not adversarial.
//  - Compiled-in apps: Not sandboxed. SPI Bus Treaty covers
//    them at the resource arbitration level.
//  - Core 0: Ghost Engine is unaffected by anything here.
//
// ============================================================

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "elf_loader.h"

typedef enum {
    SANDBOX_OK          = 0,
    SANDBOX_FAULT       = 1,  // PMS violation — ELF killed, OS safe
    SANDBOX_TIMEOUT     = 2,
    SANDBOX_STACK_OVF   = 3,
    SANDBOX_NO_HW       = 4,  // Not ESP32-S3
    SANDBOX_SETUP_FAIL  = 5,
} SandboxResult;

typedef struct {
    bool        caught;
    uint32_t    fault_addr;
    uint32_t    fault_pc;
    const char* fault_type;   // "pms_write" / "pms_exec" / "stack_overflow"
    char        elf_name[32];
    uint32_t    timestamp_ms;
    int         exit_code;
} ElfFaultRecord;

extern ElfFaultRecord elf_last_fault;

bool          elf_sandbox_available();
SandboxResult elf_sandbox_run(ElfContext* ctx,
                               int (*elf_main_fn)(void*),
                               const char* name,
                               uint32_t timeout_ms = 60000);
void          elf_sandbox_print_fault();
void          elf_sandbox_clear_fault();

#endif // ELF_SANDBOX_H
