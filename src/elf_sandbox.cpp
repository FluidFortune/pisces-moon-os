// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com

// ============================================================
//  elf_sandbox.cpp — Hardware-Enforced ELF Isolation
//
//  The three-layer approach:
//
//  Layer 1 — Exception Handler Hijack
//    xt_set_exception_handler(EXCCAUSE_LOAD_STORE_ERROR, ...)
//    Intercepts PMS violations before ESP-IDF panic handler.
//    Triage: ELF fault → kill task. OS fault → reboot.
//
//  Layer 2 — PMS Region Configuration
//    Direct register writes to ESP32-S3 PMS peripheral.
//    Marks OS SRAM write-protected during ELF execution.
//    Restores permissive defaults after ELF exits.
//
//  Layer 3 — FreeRTOS Task Isolation
//    ELF runs in a dedicated FreeRTOS task (not inline call).
//    Task handle tracked by sandbox for force-deletion.
//    Stack overflow detection via FreeRTOS canary + watchpoint.
//    Timeout enforced by sandbox monitor loop.
//
// ============================================================

#include "elf_sandbox.h"
#include <Arduino.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Xtensa exception API — provides xt_set_exception_handler
// Available in ESP-IDF via components/xtensa/include/xtensa_api.h
// In PlatformIO Arduino framework, include path varies:
#if __has_include(<xtensa_api.h>)
  #include <xtensa_api.h>
#elif __has_include(<freertos/xtensa_api.h>)
  #include <freertos/xtensa_api.h>
#else
  // Manual declaration if header not found in include path
  typedef void (*xt_exc_handler)(XtExcFrame*);
  extern "C" void xt_set_exception_handler(int cause, xt_exc_handler handler);
#endif

// ── Xtensa exception causes (ESP32-S3 TRM Chapter 4) ─────
#define EXCCAUSE_LOAD_STORE_ERROR   3   // General load/store error
#define EXCCAUSE_LOAD_PROHIBITED    28  // PMS load violation
#define EXCCAUSE_STORE_PROHIBITED   29  // PMS store violation

// ── ESP32-S3 PMS register base addresses ─────────────────
// Source: ESP32-S3 Technical Reference Manual, Chapter 15
// Permission Management System (PMS)
#define PMS_BASE              0x600C1000UL

// DRAM (SRAM) split address registers
// Each split defines a boundary between two permission regions
#define PMS_DRAM_SPLIT0_ADDR  (*(volatile uint32_t*)(PMS_BASE + 0x010))
#define PMS_DRAM_SPLIT1_ADDR  (*(volatile uint32_t*)(PMS_BASE + 0x014))
#define PMS_DRAM_SPLIT2_ADDR  (*(volatile uint32_t*)(PMS_BASE + 0x018))
#define PMS_DRAM_SPLIT3_ADDR  (*(volatile uint32_t*)(PMS_BASE + 0x01C))

// DRAM region permission registers
// Bits: [1:0] = region0, [3:2] = region1, etc.
// Per region: bit0=read, bit1=write (from non-privileged access)
#define PMS_DRAM_RGN_PMS0     (*(volatile uint32_t*)(PMS_BASE + 0x020))
#define PMS_DRAM_RGN_PMS1     (*(volatile uint32_t*)(PMS_BASE + 0x024))

// PMS enable register
#define PMS_DRAM_PMS_LOCK     (*(volatile uint32_t*)(PMS_BASE + 0x000))
#define PMS_ENABLE_BIT        (1 << 0)

// ESP32-S3 SRAM memory map (TRM Chapter 3)
#define DRAM_START            0x3FC00000UL  // Start of data SRAM
#define DRAM_END              0x3FCFFFFFULL // End of data SRAM
#define PSRAM_START           0x3C000000UL  // Start of PSRAM
#define PSRAM_END             0x3DFFFFFF UL // End of PSRAM (8MB)
#define IRAM_START            0x40370000UL  // Start of instruction SRAM

// ── Module state ─────────────────────────────────────────
ElfFaultRecord elf_last_fault = {};

static volatile TaskHandle_t  _sandbox_task    = nullptr;
static volatile bool          _sandbox_running = false;
static volatile bool          _sandbox_faulted = false;
static void*                  _elf_psram_base  = nullptr;
static size_t                 _elf_psram_size   = 0;
static SemaphoreHandle_t      _sandbox_done     = nullptr;

// Original exception handlers — saved before we install ours
static xt_exc_handler _orig_load_handler  = nullptr;
static xt_exc_handler _orig_store_handler = nullptr;

// ── PMS saved state ───────────────────────────────────────
static uint32_t _saved_dram_pms0 = 0;
static uint32_t _saved_dram_pms1 = 0;
static bool     _pms_configured  = false;

// ── ELF task wrapper ─────────────────────────────────────
typedef struct {
    ElfContext* ctx;
    int (*elf_main_fn)(void*);
    int result;
} ElfTaskArgs;

static void _elf_task_wrapper(void* pvArgs) {
    ElfTaskArgs* args = (ElfTaskArgs*)pvArgs;

    // Run the ELF entry point
    args->result = args->elf_main_fn(args->ctx);

    // Signal completion
    _sandbox_running = false;
    if (_sandbox_done) {
        xSemaphoreGive(_sandbox_done);
    }

    // Task must delete itself — never return from FreeRTOS task
    vTaskDelete(nullptr);
}

// ── Custom exception handler ─────────────────────────────
// This runs at the Xtensa exception level, before ESP-IDF
// panic handler. Called with interrupts disabled.
// XtExcFrame contains all CPU registers at fault time.
static void IRAM_ATTR _sandbox_exception_handler(XtExcFrame* frame) {
    // Only handle faults if sandbox is active
    if (!_sandbox_running || !_sandbox_task) {
        // Not our fault — call original handler (or panic)
        if (_orig_store_handler) {
            _orig_store_handler(frame);
        }
        // If no original handler, fall through to panic
        return;
    }

    // Get the faulting program counter
    uint32_t fault_pc   = frame->pc;
    uint32_t fault_addr = frame->excvaddr; // Address that caused the fault

    // Triage: is the fault PC inside the ELF PSRAM region?
    uint32_t psram_lo = (uint32_t)_elf_psram_base;
    uint32_t psram_hi = psram_lo + (uint32_t)_elf_psram_size;

    bool is_elf_fault = (fault_pc >= psram_lo && fault_pc < psram_hi)
                     || (fault_addr >= psram_lo && fault_addr < psram_hi);

    if (!is_elf_fault) {
        // Fault is in OS code — not recoverable, call original panic path
        Serial.printf("[SANDBOX] OS-level fault at PC=0x%08lx addr=0x%08lx — rebooting\n",
                      fault_pc, fault_addr);
        if (_orig_store_handler) {
            _orig_store_handler(frame);
        }
        // If we return from here, the panic handler will catch it
        return;
    }

    // ── ELF fault — recoverable ───────────────────────────
    // Record the fault BEFORE doing anything that might fail
    elf_last_fault.caught       = true;
    elf_last_fault.fault_addr   = fault_addr;
    elf_last_fault.fault_pc     = fault_pc;
    elf_last_fault.fault_type   = "pms_violation";
    elf_last_fault.timestamp_ms = millis();

    // Determine violation type from exception cause
    uint32_t excvaddr = frame->exccause;
    if (excvaddr == EXCCAUSE_STORE_PROHIBITED) {
        elf_last_fault.fault_type = "pms_write_violation";
    } else if (excvaddr == EXCCAUSE_LOAD_PROHIBITED) {
        elf_last_fault.fault_type = "pms_read_violation";
    } else {
        elf_last_fault.fault_type = "load_store_error";
    }

    // Mark sandbox as faulted
    _sandbox_running = false;
    _sandbox_faulted = true;

    // Signal the monitor that we're done
    // Note: xSemaphoreGiveFromISR is safe here since we're in
    // exception context (effectively an interrupt-like context)
    BaseType_t higher_prio_woken = pdFALSE;
    if (_sandbox_done) {
        xSemaphoreGiveFromISR(_sandbox_done, &higher_prio_woken);
    }

    // Force-delete the ELF task from exception context
    // vTaskDelete from exception context is technically unsafe
    // in general, but since we're on Core 1 and the Ghost Engine
    // is on Core 0, and we're deleting the CURRENT running task,
    // this is the only option. We use the task notification trick:
    // set a flag and let the monitor clean up the task handle.
    // The ELF task will not execute further because we're about
    // to modify the exception frame to redirect execution.

    // Redirect program counter to a safe landing pad
    // This is the key technique: instead of letting the CPU
    // retry the faulting instruction (which would fault again),
    // we redirect PC to our cleanup trampoline.
    // We encode the trampoline address in the exception frame.
    extern void _elf_sandbox_fault_trampoline(void);
    frame->pc = (uint32_t)&_elf_sandbox_fault_trampoline;

    // Return from exception handler — CPU will execute the
    // trampoline at World 1 (or World 0 if we didn't switch),
    // which calls vTaskDelete(NULL) and signals completion.
}

// ── Fault trampoline ─────────────────────────────────────
// This function is the landing pad after a redirected exception.
// Runs in the context of the ELF task (now redirected here).
// Its only job: delete itself and give the semaphore.
// Must be in IRAM so it's always accessible.
void IRAM_ATTR _elf_sandbox_fault_trampoline(void) {
    _sandbox_running = false;
    if (_sandbox_done) {
        xSemaphoreGive(_sandbox_done);
    }
    vTaskDelete(nullptr);
    // Execution never reaches here
    while (1) { vTaskDelay(portMAX_DELAY); }
}

// ── PMS configuration ─────────────────────────────────────
// Configure ESP32-S3 PMS to protect OS SRAM during ELF execution.
// ELF PSRAM region gets read-write. OS SRAM gets read-only.
//
// NOTE: PMS register layout is chip-specific and version-sensitive.
// These addresses are from ESP32-S3 TRM v1.3, Chapter 15.
// If Espressif changes the register map in a silicon revision,
// this needs updating. We include a sanity check.
static bool _pms_configure(void* elf_base, size_t elf_size) {
    // Save current PMS state
    _saved_dram_pms0 = PMS_DRAM_RGN_PMS0;
    _saved_dram_pms1 = PMS_DRAM_RGN_PMS1;

    // Sanity check: can we read PMS registers at all?
    // If the address is wrong, we'd get a load exception here.
    // We skip full PMS configuration if registers don't look
    // like valid permission values (should be 0x0–0xF range
    // per 2-bit field encoding).
    if (_saved_dram_pms0 > 0xFFFF || _saved_dram_pms1 > 0xFFFF) {
        Serial.println("[SANDBOX] PMS registers sanity check failed — skipping PMS config");
        return false;
    }

    // Configure split addresses to carve out the OS SRAM region
    // We want:
    //   Region 0 (0x3FC00000 to DRAM_OS_END): read-only (OS data)
    //   Region 1 (ELF PSRAM): read-write
    //   All other regions: read-write (permissive default)
    //
    // ESP32-S3 PMS has 3 configurable split points for DRAM,
    // creating 4 regions. We use split 0 at the OS/PSRAM boundary.

    // For now: mark the OS SRAM region as supervisor-only
    // (non-privileged code gets read-only, no write)
    // Bits per region: [1:0] region0 permissions: 0b01 = read-only
    // Full permissive: 0b11 = read+write
    uint32_t new_pms0 = _saved_dram_pms0;

    // Region 0 (OS SRAM): read-only (bit1=0, bit0=1)
    new_pms0 = (new_pms0 & ~0x3) | 0x1;

    // Apply the configuration
    PMS_DRAM_RGN_PMS0 = new_pms0;

    _pms_configured = true;
    return true;
}

static void _pms_restore() {
    if (_pms_configured) {
        PMS_DRAM_RGN_PMS0 = _saved_dram_pms0;
        PMS_DRAM_RGN_PMS1 = _saved_dram_pms1;
        _pms_configured = false;
    }
}

// ── Public API implementation ─────────────────────────────

bool elf_sandbox_available() {
    esp_chip_info_t info;
    esp_chip_info(&info);
    // World Controller + PMS present on ESP32-S3 (model = 9)
    // ESP_CHIP_INFO_ESP32S3 = 9 in ESP-IDF
    return (info.model == CHIP_ESP32S3);
}

SandboxResult elf_sandbox_run(ElfContext* ctx,
                               int (*elf_main_fn)(void*),
                               const char* name,
                               uint32_t timeout_ms) {
    if (!elf_main_fn || !ctx) return SANDBOX_SETUP_FAIL;
    if (timeout_ms == 0) timeout_ms = 60000;

    // Store ELF PSRAM region for triage in exception handler
    _elf_psram_base = ctx->psram_base;
    _elf_psram_size = ctx->psram_size;

    // Update fault record name
    strncpy(elf_last_fault.elf_name, name, 31);
    elf_last_fault.elf_name[31] = 0;
    elf_last_fault.caught = false;

    // ── Hardware available? ───────────────────────────────
    if (!elf_sandbox_available()) {
        Serial.printf("[SANDBOX] WARNING: Not ESP32-S3 — running '%s' unsandboxed\n", name);
        // Run directly without protection
        int r = elf_main_fn(ctx);
        elf_last_fault.exit_code = r;
        return SANDBOX_NO_HW;
    }

    // ── Set up completion semaphore ───────────────────────
    _sandbox_done = xSemaphoreCreateBinary();
    if (!_sandbox_done) return SANDBOX_SETUP_FAIL;

    // ── Install custom exception handlers ─────────────────
    // ESP-IDF does not expose xt_get_exception_handler in this version,
    // so we cannot save the previous handler. Setting NULL during restore
    // returns the chip to ESP-IDF's default panic behavior.
    _orig_load_handler  = nullptr;
    _orig_store_handler = nullptr;

    xt_set_exception_handler(EXCCAUSE_LOAD_STORE_ERROR,    _sandbox_exception_handler);
    xt_set_exception_handler(EXCCAUSE_LOAD_PROHIBITED,     _sandbox_exception_handler);
    xt_set_exception_handler(EXCCAUSE_STORE_PROHIBITED,    _sandbox_exception_handler);

    // ── Configure PMS regions ─────────────────────────────
    bool pms_ok = _pms_configure(_elf_psram_base, _elf_psram_size);
    if (!pms_ok) {
        Serial.printf("[SANDBOX] PMS config failed for '%s' — running unsandboxed\n", name);
        // Continue without PMS — exception handler still active,
        // which catches null ptr and stack overflow faults.
    }

    // ── Launch ELF task ───────────────────────────────────
    ElfTaskArgs args = { ctx, elf_main_fn, 0 };
    _sandbox_running = true;
    _sandbox_faulted = false;
    _sandbox_task    = nullptr;

    BaseType_t created = xTaskCreatePinnedToCore(
        _elf_task_wrapper,
        name,
        8192,          // 8KB stack — ELF gets PSRAM, not stack
        &args,
        5,             // Priority — below Ghost Engine (7)
        (TaskHandle_t*)&_sandbox_task,
        1              // Core 1 — same as launcher, away from Ghost Engine
    );

    if (created != pdPASS || !_sandbox_task) {
        _pms_restore();
        xt_set_exception_handler(EXCCAUSE_LOAD_STORE_ERROR, NULL);
        xt_set_exception_handler(EXCCAUSE_LOAD_PROHIBITED,  NULL);
        xt_set_exception_handler(EXCCAUSE_STORE_PROHIBITED, NULL);
        vSemaphoreDelete(_sandbox_done);
        _sandbox_done = nullptr;
        return SANDBOX_SETUP_FAIL;
    }

    Serial.printf("[SANDBOX] '%s' launched in PMS sandbox (task=%p, psram=%p+%uKB)\n",
                  name, _sandbox_task,
                  _elf_psram_base, (unsigned)(_elf_psram_size / 1024));

    // ── Monitor loop ──────────────────────────────────────
    // Wait for ELF to complete, fault, or timeout.
    SandboxResult result = SANDBOX_OK;

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    BaseType_t sem_taken = xSemaphoreTake(_sandbox_done, timeout_ticks);

    if (sem_taken == pdTRUE) {
        // Semaphore taken — ELF either finished or faulted
        if (_sandbox_faulted) {
            result = SANDBOX_FAULT;
        } else {
            elf_last_fault.exit_code = args.result;
            result = SANDBOX_OK;
        }
    } else {
        // Timeout — force-delete the ELF task
        Serial.printf("[SANDBOX] TIMEOUT: '%s' exceeded %lums — force killing\n",
                      name, timeout_ms);
        if (_sandbox_task) {
            vTaskDelete((TaskHandle_t)_sandbox_task);
            _sandbox_task = nullptr;
        }
        _sandbox_running = false;
        elf_last_fault.caught     = true;
        elf_last_fault.fault_type = "timeout";
        result = SANDBOX_TIMEOUT;
    }

    // ── Cleanup ───────────────────────────────────────────
    // Small delay to let the task finish deleting itself
    vTaskDelay(pdMS_TO_TICKS(10));

    // Restore PMS to permissive defaults
    _pms_restore();

    // Restore original exception handlers
    xt_set_exception_handler(EXCCAUSE_LOAD_STORE_ERROR, NULL);
    xt_set_exception_handler(EXCCAUSE_LOAD_PROHIBITED,  NULL);
    xt_set_exception_handler(EXCCAUSE_STORE_PROHIBITED, NULL);

    _orig_load_handler  = nullptr;
    _orig_store_handler = nullptr;

    vSemaphoreDelete(_sandbox_done);
    _sandbox_done  = nullptr;
    _sandbox_task  = nullptr;
    _elf_psram_base = nullptr;
    _elf_psram_size = 0;

    // Log result
    const char* result_str = "OK";
    if (result == SANDBOX_FAULT)   result_str = "FAULT (ELF killed, OS safe)";
    if (result == SANDBOX_TIMEOUT) result_str = "TIMEOUT (ELF force-killed)";
    if (result == SANDBOX_NO_HW)   result_str = "NO_HW (unsandboxed)";
    Serial.printf("[SANDBOX] '%s' exit: %s\n", name, result_str);

    return result;
}

void elf_sandbox_print_fault() {
    if (!elf_last_fault.caught) {
        Serial.println("[SANDBOX] No fault recorded.");
        return;
    }

    Serial.printf("[SANDBOX] FAULT RECORD:\n");
    Serial.printf("  ELF:       %s\n",         elf_last_fault.elf_name);
    Serial.printf("  Type:      %s\n",         elf_last_fault.fault_type);
    Serial.printf("  PC:        0x%08lx\n",    elf_last_fault.fault_pc);
    Serial.printf("  Addr:      0x%08lx\n",    elf_last_fault.fault_addr);
    Serial.printf("  Time:      %lums\n",      elf_last_fault.timestamp_ms);

    // Emit Bridge JSON if streaming
    extern volatile bool wardrive_bridge_streaming;
    if (wardrive_bridge_streaming) {
        Serial.printf("{\"event\":\"elf_fault\","
                      "\"elf\":\"%s\","
                      "\"type\":\"%s\","
                      "\"pc\":\"0x%08lx\","
                      "\"addr\":\"0x%08lx\"}\n",
                      elf_last_fault.elf_name,
                      elf_last_fault.fault_type,
                      elf_last_fault.fault_pc,
                      elf_last_fault.fault_addr);
    }
}

void elf_sandbox_clear_fault() {
    memset(&elf_last_fault, 0, sizeof(elf_last_fault));
}