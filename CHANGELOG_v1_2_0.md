# Pisces Moon OS — Changelog v1.2.0 "Multi-Device"

**Release date:** May 2026
**Author:** Eric Becker / Fluid Fortune
**License:** AGPL-3.0-or-later
**fluidfortune.com**

---

## Overview

v1.2.0 is the multi-device release. The codebase now targets three ESP32-S3 devices from a single PlatformIO project. The Ghost Engine architecture, SPI Bus Treaty, Bridge protocol, and all 52+ apps remain intact on the T-Deck Plus. New devices are added via a hardware abstraction layer that is fully isolated behind `#ifdef` blocks — the T-Deck Plus build is byte-for-byte identical to v1.1.1 when built with the `tdeck_plus` environment.

Cardputer ADV is now a fully supported target with WiFi, BLE, GPS, and LoRa concurrent operation — see the late-session addendum at the bottom of this changelog for the full story of how a no-PSRAM device came to run the OS.

---

## New: Multi-Device Hardware Abstraction Layer

### `include/hal_pins.h` — NEW

Per-device GPIO assignments for all three targets. Every pin definition is wrapped in `#ifdef DEVICE_*` blocks. T-Deck Plus pins are unchanged from v1.1.1 — new devices add alongside them without interference.

**Devices:**

- `DEVICE_TDECK_PLUS` — LilyGO T-Deck Plus (existing, unchanged)
- `DEVICE_TLORAPAGER` — LilyGO T-LoraPager (new)
- `DEVICE_CARDPUTER_ADV` — M5Stack Cardputer ADV with Cap LoRa-1262 (new, fully supported)

T-LoraPager pin assignments sourced from the T-LoraPager V1.0 Schematic (25-06-13) and the LilyGoLib official wiki hardware reference.

### `include/spi_treaty.h` — NEW

Multi-device SPI Bus Treaty implementation. Library-free throughout. All devices use our own `spi_mutex` with our own 500ms timeout and named ownership debug logging — not a wrapper around any third-party library semaphore.

**T-LoraPager additions:**

- Full XL9555 I2C GPIO expander implementation (direct Wire writes, no library). Shadow registers track port state.
- Per-peripheral power management macros: `PM_LORA_POWER_ON()`, `PM_GPS_POWER_ON()`, `PM_NFC_POWER_ON()`, `PM_SD_POWER_ON()`, `PM_SPK_POWER_ON()`, `PM_HAPTIC_POWER_ON()`, `PM_KB_POWER_ON()`
- `xl9555_init()` — configures all XL9555 pins as outputs, all peripherals off at boot
- `xl9555_boot_sequence()` — powers peripherals in Ghost Engine priority order: SD → LoRa → GPS → Keyboard → Speaker/Haptic → NFC
- No-op macros on T-Deck Plus and Cardputer ADV — compile to nothing

### `include/hal_display.h` — NEW

Per-device display initialization. Direct `Arduino_GFX` driver calls. No LilyGoLib. No third-party BSP.

- **T-Deck Plus:** ST7789 320×240, unchanged
- **T-LoraPager:** ST7796U 480×222 with y-offset 49 correction. The ST7796U is a 320×480 controller — the physical display window is 480×222 centered at y=49 in the controller frame buffer. Without this offset the image is shifted and clipped. Backlight via GPIO42 PWM → AW9364 16-level driver. Keyboard backlight via GPIO46 PWM (separate circuit).
- **Cardputer ADV:** ST7789V2 240×135 landscape, direct SPI with no framebuffer.

### `include/hal_input.h` — NEW

Per-device input abstraction. Direct GPIO and I2C. No library. Unified `pm_input_event_t` interface — all apps use `pm_input_read()` regardless of physical input device.

- **T-Deck Plus:** trackball GPIO polling (unchanged)
- **T-LoraPager:** Rotary encoder via Gray code state machine on GPIO40/41/7. TCA8418 keyboard controller via direct I2C at 0x34. Key event FIFO polling, ASCII keymap table, press/release discrimination. DRV2605 haptic driver via direct I2C at 0x5A. Init, mode setup, tactile click effect on keypress and encoder button. No library — direct register writes.
- **Cardputer ADV:** M5Cardputer keyboard via M5Cardputer library at I2C 0x34. Function-modifier handling for arrow keys, escape, tab, enter. Diagnostic Serial logging on every keypress for input development and debugging.

---

## Updated: `platformio.ini`

Single PlatformIO project, three build environments:

```
[env:tdeck_plus]     — default, exact match to v1.1.1 settings
[env:tlorapager]     — T-LoraPager, full pin assignments
[env:cardputer_adv]  — Cardputer ADV, full Cap LoRa-1262 support
```

T-Deck Plus environment preserves all v1.1.1 settings exactly:

- `monitor_speed = 115200`
- `upload_resetmethod = no_reset` with `--before`/`--after` flags
- `board_build.arduino.memory_type = qio_opi`
- `-Os -fPIC -mfix-esp32-psram-cache-issue`
- `CONFIG_SPIRAM_USE_MALLOC=1`
- All `lib_deps` at same versions as v1.1.1

No LilyGoLib dependency on any target. Direct hardware access throughout.

**PSRAM flag split** (Cardputer correctness fix): Shared build flags now split into `build_flags_common` (universal) and `build_flags_psram` (PSRAM-only). T-Deck Plus and T-LoRa Pager include the PSRAM block; Cardputer ADV does not, and instead defines `DEVICE_NO_PSRAM`. Previously Cardputer was inheriting `BOARD_HAS_PSRAM` and `CONFIG_SPIRAM_USE_MALLOC=1` from the shared base, causing the allocator to take PSRAM-probe-and-fail paths on every malloc.

---

## Updated: Bridge Protocol

### `src/bridge_app.cpp`

- `ghost_engine` field added to Bridge status response: `"active"` when Ghost Engine is running, `"idle"` when paused.
- `wardrive_mode` field added: `"scan"` or `"promiscuous"`.
- Version strings corrected to `PISCES_OS_VERSION` macro — no more hardcoded "1.0.0" drift.
- Baud rate comment corrected to 921600.
- Header comment corrected to 921600 baud.

---

## Updated: Ghost Engine & Wardrive

### `src/wardrive.cpp`

- `_wifi_enc_str()` helper — full encryption detail replaces binary OPEN/WPA. Now emits: OPEN/WEP/WPA/WPA2/WPA2/WPA3/WPA3/WAPI. Required by The Clinician security donut chart.
- `BLEResult` struct extended with `addr_type[8]` and `mfg_hex[32]` fields.
- `enqueueBLE()` extended to extract manufacturer data hex (first 8 bytes) and address type (public/random) from NimBLE `onResult` callback.
- `ble_seen` JSON emission extended with `addr_type` and `mfg_data` fields — `pm_bridge.py` v1.2 was already expecting these for full BLE CSV logging.
- `_wardrive_on_pkt()` — now emits `probe_seen` JSON for 802.11 probe-request frames when Bridge streaming active. Enables `pm_bridge.py` probe CSV logging and The Clinician device fingerprinting analysis. Previously only emitted generic `pkt` events.
- `wardrive_raw_log` flag added to `wardrive.h` — when true, `wifi_seen` emits for every observation (not just new/updated networks). Enables `PMStats.timeline()` and `PMStats.persistence()` in The Clinician.

### `include/wardrive.h`

- `wardrive_raw_log` extern declaration added.
- Documentation updated.

---

## Updated: Mesh Messenger

### `src/mesh_messenger.cpp`

**Bridge streaming integration:**

RX path now emits `mesh_link` JSON when `wardrive_bridge_streaming` is true:

```json
{"event":"mesh_link","from":"!abc123","to":"!def456",
 "rssi":-95,"snr":7.5,"freq":915.0,"sf":7,"bw":125,
 "quality":65,"lat":34.05,"lng":-118.24}
```

`pm_bridge.py` v1.2 was ready and waiting — firmware was not emitting. This closes the LoRa data pipeline to The Clinician.

- Added `extern TinyGPSPlus gps` and `extern volatile bool wardrive_bridge_streaming` includes.
- Added `#include <TinyGPSPlus.h>` for GPS coordinate injection into `mesh_link` events.

---

## Updated: Probe Intel

### `src/probe_intel.cpp`

- Promiscuous mode now emits `probe_seen` JSON over Serial when Bridge streaming active. `pm_bridge.py` logs to `probes_*.csv`. The Clinician receives live probe data for device fingerprinting and movement pattern analysis.
- `extern volatile bool wardrive_bridge_streaming` added.

---

## Updated: ELF Loader & Sandbox

### `src/elf_loader.cpp` + `include/elf_loader.h`

- ELF execution now routes through `elf_sandbox_run()` instead of direct `elf_main_fn(ctx)` call.
- `ElfContext` extended with `psram_base` and `psram_size` fields (v1.2 API) for sandbox fault triage.
- `_elf_internal::current_elf_name` added for fault logging.
- Manifest read, `elf_scan_apps`, and `elf_execute` file open all wrapped with `spi_mutex` — Treaty compliance complete.

### `src/elf_sandbox.cpp` + `include/elf_sandbox.h` — NEW

Hardware-enforced ELF app isolation using the ESP32-S3 Permission Management System (PMS) and Xtensa exception handler hijacking.

**Three-layer architecture:**

**Layer 1 — Exception Handler Hijack.** `xt_set_exception_handler()` registers our handler for `EXCCAUSE_LOAD_PROHIBITED` (28) and `EXCCAUSE_STORE_PROHIBITED` (29) — the PMS violation exceptions. Our handler intercepts BEFORE the ESP-IDF panic handler. Triage:

- Fault PC inside ELF PSRAM region → redirect PC to `_elf_sandbox_fault_trampoline()`, kill ELF task, OS safe
- Fault PC in OS code → pass to original panic handler, reboot

**Layer 2 — PMS Region Configuration.** Direct register writes to 0x600C1000 (PMS base address, ESP32-S3 TRM Chapter 15). OS SRAM marked read-only during ELF execution. Restored to permissive defaults after ELF exits. Sanity check on register values before applying.

**Layer 3 — FreeRTOS Task Isolation.** ELF runs in a dedicated task (priority 5, below Ghost Engine priority 7). Semaphore-based completion signaling. Configurable timeout (default 60s). Fault trampoline in IRAM.

**Result:** a buggy ELF that writes a null pointer, overflows its stack, or attempts to corrupt OS SRAM gets surgically killed. The launcher recovers. The Ghost Engine never pauses.

This is the first documented hardware-enforced ELF sandbox on the ESP32-S3 for a general-purpose OS.

**Limitations (honest):**

- Read isolation only — PMS write-protects OS SRAM. ELF code can still READ any address. No MMU = no virtual address spaces. Protects against buggy ELFs, not adversarial code.
- Compiled-in apps are not sandboxed. The SPI Bus Treaty is their protection layer.

---

## Updated: Version Strings

All on-screen version displays now show **v1.2.0**:

- `src/main.cpp` BIOS boot screen, splash, file header
- `src/launcher.cpp` version tag
- `src/about_app.cpp` version display
- `PISCES_OS_VERSION` macro updated to `"1.2.0"` and referenced throughout

Launcher footer continues to display the Ghost Engine status indicator: `*GE:ON` (blinking green) when active, `GE:--` (dimmed) when idle. Turns "always collecting" from implicit to auditable.

---

## Updated: Security

### `include/security_config.h`

- Real PIN values removed — replaced with placeholders: `YOUR_TACTICAL_PIN_HERE`, `YOUR_DECOY_PIN_HERE`, `YOUR_NUKE_PIN_HERE`.
- `GHOST_PARTITION_ENABLED` commented out by default — opt-in rather than opt-out for security feature activation.

### `include/security_config.h.example` — NEW

Template with placeholder values and documentation. Commit this. Do not commit `security_config.h`.

### `.gitignore`

- `include/security_config.h` added — prevents accidental PIN commit after local configuration.

**Required local git operation (one-time):**

```sh
git rm --cached include/security_config.h
git add include/security_config.h.example
git commit -m "chore: remove tracked security_config.h, add example"
```

---

## Updated: SPI Bus Treaty — Completion Pass

The following files had direct `sd.open()` / `sd.exists()` calls outside mutex paths. All wrapped in v1.2.0:

- `src/nosql_store.cpp` — all 5 public functions wrapped with `_nm_take()`/`_nm_give()` helpers
- `src/database.cpp` — both public functions wrapped with `_db_take()`/`_db_give()` helpers
- `src/elf_loader.cpp` — manifest read, directory scan, and ELF file open wrapped
- `src/filesystem.cpp` — on-device file manager fully wrapped (see "SPI Bus Treaty — Audit and Mutex Hardening" section below)

The systematic Treaty audit was also completed for v1.2.0 — see the dedicated audit section below.

---

## T-LoraPager Hardware Notes

Confirmed from schematic and official wiki:

| Peripheral | GPIO / Bus | Notes |
|---|---|---|
| Display ST7796U | CS=38, DC=37, BL=42 | y-offset 49 required |
| LoRa SX1262 | CS=36, IRQ=14, RST=47, BUSY=48 | Shared SPI |
| SD Card | CS=21 | Shared SPI |
| NFC ST25R3916 | CS=39, IRQ=5 | Shared SPI |
| GPS MIA-M10Q | TX=4, RX=12, PPS=13 | UART |
| I2C bus | SDA=3, SCL=2 | All I2C devices |
| Rotary encoder | A=40, B=41, BTN=7 | Direct GPIO |
| Keyboard TCA8418 | I2C 0x34 | KEY_INT=6 |
| IMU BHI260AP | I2C 0x28 | HIRQ=8 |
| Haptic DRV2605 | I2C 0x5A | |
| RTC PCF85063A | I2C 0x51 | RTC_INT=1 |
| Audio ES8311 | I2C 0x18, I2S 10/11/18/45/17 | |
| XL9555 expander | I2C 0x20 | Power gates all peripherals |

**First flash recommendation:** flash LilyGo factory firmware first to confirm all hardware works, then flash Pisces Moon.

---

## Cardputer ADV Hardware Notes

| Peripheral | GPIO / Bus | Notes |
|---|---|---|
| Display ST7789V2 | MOSI=35, SCK=36, CS=37, DC=34, RST=33, BL=38 | 240×135 landscape, FSPI bus (display only) |
| LoRa SX1262 (Cap) | CS=5, RST=3, IRQ=4, BUSY=6 | HSPI bus, shared with SD |
| SD Card | CS=21 | HSPI bus, shared with LoRa |
| GPS AT6668 (Cap) | RX=15, TX=13 | UART at 115200 baud, multi-constellation |
| I2C bus | SDA=8, SCL=9 | M5Cardputer keyboard at 0x34, Cap PI4IOE expander at 0x43 |
| ES8311 I2S audio | SCLK=41, DOUT=46, LRCK=43, DIN=42 | |
| IR transmitter | GPIO 44 | |
| Battery ADC | GPIO 10 | |
| Boot button | GPIO 0 | |

**PI4IOE5V6408 I/O expander (Cap LoRa-1262):** Custom driver in `src/pi4ioe_cap.cpp` and `include/pi4ioe_cap.h`. Drives the SX1262 RF switch enable. Probes I2C 0x43; passes silently if absent (Cap LoRa-868 variant) or if the Cap is not attached. No library dependency.

**HSPI bus routing for SX1262:** Mesh Messenger and any future LoRa-using app uses the `cardputerSdSPI` SPI instance (HSPI bus) on Cardputer, not the default `SPI` instance (FSPI bus, which serves the display). The SPI Bus Treaty mutex coordinates LoRa and SD card access on the shared HSPI bus.

---

## Updated: Ghost Engine — Wardrive v2.7

### `src/wardrive.cpp` — Pager session-file race fix

Restores the Ghost Engine invariant on T-LoRa Pager. The Ghost Engine's defining property — *the device is always collecting, the operator never has to think about it* — was silently violated on Pager between v1.2.0-pre and v2.6. The task was alive and scanning, but its CSV writes were going nowhere.

**Root cause:** T-LoRa Pager defers SD card mount to a background task because the shared SPI bus can't always service the card during the first few seconds of boot. The wardrive task previously created its session CSV at task startup, after a fixed 15-second wait for `g_sd_ready`. If the late mount completed after that 15-second window:

- The `if (g_sd_ready)` branch was skipped
- The code fell into a timestamp-based fallback that set `_current_log_file` to a phantom name like `/wardrive_0015.csv`
- The fallback never actually called `sd.open(... O_CREAT)`
- Subsequent writes used `O_APPEND` without `O_CREAT`, which cannot create a missing file — SdFat silently dropped them

Net effect on Pager: the task ran for the entire session, scanning WiFi and BLE, accumulating frame counts in RAM, displaying the live UI — but no rows ever landed on the SD card. The Ghost Engine appeared to be running. It was not collecting.

T-Deck Plus was not affected. T-Deck mounts SD synchronously at boot before the wardrive task spawns, so `g_sd_ready` is always true when the task starts.

**Fix — Lazy session-file creation.** The task no longer tries to create the session file at startup. Instead, `ensure_session_file()` is called on every scan-loop iteration. It is a fast no-op once `_current_log_file` is set; on the first iteration where `g_sd_ready` is true and `sd_in_use` is false, it claims the next available `/wardrive_NNNN.csv` via `_find_next_session_number()` and writes the CSV header. The filename is only adopted after the header write succeeds — if the open or write fails, the next iteration retries cleanly.

T-Deck behavior is unchanged. Pager benefits without changes to the radio scan logic, the BLE callbacks, or the pause/unpause behavior.

**Filename ribbon UI.** The wardrive app's filename ribbon previously showed `waiting...` on Pager forever (because `_current_log_file` was set to the phantom name but the file didn't exist). It now correctly displays `waiting...` while no file exists yet, and refreshes live to show the real filename the moment SD comes up and the session file is created.

**Diagnostic output.** The task prints to Serial:

```
[WARDRIVE] Task started — session file will be created when SD is ready
[WARDRIVE] Session file created: /wardrive_NNNN.csv
[WARDRIVE] Failed to create session file ... — will retry
```

The "Session file created" message gives an exact timestamp for when the Ghost Engine started writing on a given boot. Useful for field validation.

---

## New: Ghost Ride The Whip

### `src/wardrive_inspect.cpp` + `include/wardrive_inspect.h` — NEW

Read-only diagnostic browser for the wardrive CSV logs. The Ghost Engine keeps writing to the card; this app lets the operator check what's been laid down without stopping the engine. *Ghost rides the whip — the engine drives itself, you check the rear-view.*

Lists every `/wardrive_*.csv` file on the SD card with:

- Filename
- File size in bytes
- Data row count (line count minus the CSV header)

**Field diagnostic flow:**

| Outcome | Meaning |
|---|---|
| One file, growing | Rotation broken (regression check) |
| Many files, all only 1 line | SD writes failing at row level (likely GPS) |
| Many files of varying line counts | Working correctly |
| No files | SD never mounted, or wardrive never went active |

**SPI Bus Treaty compliance.** Every SdFat call wrapped in `PM_SPI_TAKE` / `PM_SPI_GIVE`. The app sets `sd_in_use = true` before scanning so the Ghost Engine pauses its writes during the inventory pass. Line-counting releases the mutex between files so the engine resumes writing if scanning takes longer than expected.

**Display:**

- T-Deck Plus (320×240): 9 rows, page with N/P, trackball select
- T-LoRa Pager (480×222): 9 rows, M rescan, N/P page, Q exit
- M5Stack Cardputer ADV (240×135): 10 rows, Fn-arrow select, R rescan

Available under TOOLS category in the launcher.

---

## Updated: Cardputer ADV — Field Fixes

### `src/main.cpp` — Splash overlap fix

The rainbow splash chip icon was rendering at y=72 on the 135-pixel Cardputer screen, which placed it directly behind the "Pisces Moon" title text. Visually: logo inside the words.

Fixed with a `#ifdef DEVICE_CARDPUTER_ADV` branch in `showRainbowSplash()` that uses Cardputer-specific geometry:

- chip icon centered at y=22 (well above title)
- title at y=40 (24px size-3 text, y=40–64)
- "Powered by Gemini." at y=78
- "Limited only by your imagination." at y=98
- version footer at y=118

T-Deck and Pager splash layout preserved verbatim in the `#else` branch.

### `src/main.cpp` — BIOS auto-scroll

The BIOS boot sequence renders ~15 lines top-to-bottom. On the Cardputer's 135-pixel screen only the first 9 lines were visible; everything after that drew below the screen and was lost.

**Fix:** a ring buffer of recent boot entries plus a repaint helper. When `drawBootLine()` or `drawBootSection()` would write past the visible content area, the body region is cleared and the most-recent ~9 entries are replayed shifted upward by one slot. The new entry appears at the bottom slot. T-Deck and Pager paths are unchanged (gated entirely by `#ifdef DEVICE_CARDPUTER_ADV`).

Memory cost: ~5 KB ring on Cardputer only. (Further reduced to bounded 12-entry ring during memory-recovery work — see late-session addendum.)

### `src/launcher_cardputer.cpp` — Arrow + Enter handling

The launcher previously only handled A/D/H/L and 13 for navigation. Pressing Fn-arrow keys on the Cardputer keyboard had no effect even though the keyboard driver was correctly translating Fn+, → `PM_KEY_LEFT` etc.

**Fix:** explicit handlers added for `PM_KEY_LEFT`, `PM_KEY_RIGHT`, `PM_KEY_UP`, `PM_KEY_DOWN`, and `PM_KEY_ENTER`. The existing letter handlers are preserved as fallbacks. Up/Down jump to the previous/next category's first app — the closest analog to vertical navigation in the flat one-row Cardputer launcher.

Footer hint updated from `<-/-> ENTER 1-7 CAT Q SLP` to `fn-arrows ENT 1-7 Q SLP` so the affordance honestly reflects what keys the launcher accepts.

### `src/keyboard.cpp` — Diagnostic instrumentation

Added Serial logging to `init_keyboard()`, `cp_kb_translate()`, and `get_keypress()` so the Serial monitor reports exactly what the M5Cardputer library is detecting on every press. Format:

```
[CP-KB] isChange=1 nowPressed=1 prevPressed=0
[CP-KB] fn=1 enter=0 ... word=[;]
[CP-KB] → UP
```

Diagnostic only — no behavioral changes. Useful for future iteration on the keyboard mapping; can be removed once Cardputer input is proven stable across all apps.

### WASD → ESDF on Cardputer

The Cardputer's keyboard layout offsets W to sit above A rather than between Q and S, placing the inverted-T navigation cluster on **E/S/D/F** instead of W/A/S/D. Games and apps that use W for up-direction input now accept E on Cardputer. S, D, and F continue to mean left/down/right as expected. Other devices continue to use W/A/S/D unchanged.

Affected: Snake, Pac-Man, Galaga, SimCity, Pole Position, Chess, Tetris, Etch, and any other apps with WASD-style cursor or directional input. Each game's input handler now branches via `#ifdef DEVICE_CARDPUTER_ADV` so the correct key maps to the up direction on each device.

---

## Updated: Licensing — AGPL Headers Batch

63 source files and headers that previously lacked the standard AGPL SPDX header have been brought into compliance. Total files now carrying `SPDX-License-Identifier: AGPL-3.0-or-later`: 128.

**Files modified:**

- 16 `src/*.cpp` files (beacon_spotter, ble_gatt_explorer, database, doom_app, hash_tool, micropython_app, net_scanner, nosql_store, offline_pkt_analysis, pkt_sniffer, retro_elf_pack, rf_spectrum, ssh_client, wifi_ducky, wifi_manager, wpa_handshake)
- 47 `include/*.h` files

**`src/doom_app.cpp` — Special header.** Gets a modified header noting that the Doom engine itself (esp32-doom, GPL-2.0) and any WAD content (id Software / Bethesda) are not distributed with Pisces Moon and are not covered by AGPL. The integration glue (the file itself) is yours under AGPL.

**Files deliberately excluded:**

- Classic-name games (Snake, Pac-Man, Galaga, SimCity and their `include/` headers) — re-implementations using trademarked names; no AGPL header added to avoid asserting copyright over those names.
- Retired / dead code (`src/third_party/galaga.cpp` and `src/stubbs.cpp`, both wrapped in `#if 0`).
- Third-party verbatim file (`variants/lilygo_tlora_pager/pins_arduino.h`).
- Template file (`include/secrets.h` with instructional comments).

---

## Updated: SPI Bus Treaty — Audit and Mutex Hardening

The session-long pattern recognition that every ESP32-S3 handheld with both LoRa and SD card on a shared SPI bus has the same arbitration requirement was formalized into two documents and one significant code change in v1.2.0.

### Whitepaper section

A new `docs/SPI-BUS-TREATY.md` (also published at fluidfortune.com) documents the Treaty as a named architectural pattern that solves a structural problem across the entire ESP32-S3 + LoRa + SD device class. Covers the physics of the SPI conflict, four real-world failures from the field (Mesh Messenger packet drops, wardrive silent writes, file manager crash-loop, NRF24 current leak), the Treaty's design (named ownership, ISR-safe variants, sentinel flags), and what it means for other devices in the same hardware class (T-Beam, Heltec, KodeDot, etc.).

The Treaty's existence is what makes the Ghost Engine's defining claim — *the device is always collecting* — actually achievable on hardware where the bus could theoretically corrupt under arbitrary task interleaving.

### Systematic audit

Every `xSemaphoreTake(spi_mutex, ...)` call site in the codebase was walked and verified against its release paths. Result: **54 take sites across 12 files, 11 of 12 clean.** The only file with real bugs was `elf_loader.cpp`. Audit document published as `docs/SPI-TREATY-AUDIT.md`.

### File manager Treaty fix

`src/filesystem.cpp` — the original bug that started this session's investigation. The on-device file manager was crash-looping on T-Deck Plus and T-LoRa Pager because `run_filesystem()` called `sd.open(directory)` and `dir.openNext()` without taking the SPI mutex. The wardrive task simultaneously writing CSV rows on the same bus corrupted SdFat state.

Fixed in v1.2.0:

- `view_text_file()` — full Treaty discipline (`sd_in_use=true` on entry, mutex taken before SD ops, both released on every exit path including error/exit-key/touchscreen-tap)
- `run_filesystem()` — directory scan wrapped in mutex, sentinel set/cleared correctly, crash-loop on failed open replaced with graceful error display
- Added `pm_is_exit_key()` handler so Pager and Cardputer users can leave the file manager via Q (they have no touchscreen for header-tap exit)

### ELF Loader bugs

Two real bugs found by the audit, both fixed in v1.2.0:

**Bug 1 — `elf_execute()` mutex leak.** Took the mutex at function entry but only released it on 1 of 7 exit paths. Every successful ELF execution leaked the mutex permanently; subsequent SPI operations would block forever. Fixed by releasing the mutex immediately after the SD read completes — ELF parsing and execution don't need the bus held. Three additional explicit releases added to the pre-read early-return paths.

**Bug 2 — `elf_scan_apps()` recursive deadlock.** Called `elf_load_manifest()` inside its per-file loop while holding the mutex; `elf_load_manifest()` itself took the mutex again. With the non-recursive `xSemaphoreCreateMutex()`, this would have deadlocked on any valid ELF manifest. Fixed by converting `spi_mutex` to a recursive mutex (see below).

Plus a one-line fix for `elf_scan_apps()`'s `/apps/` missing early-return path that was leaking the mutex.

Neither bug had surfaced in field testing because no users had `.elf` modules installed in `/apps/` yet, but they would have broken RetroPack and any future ELF module the moment they were exercised.

### Recursive mutex conversion

`spi_mutex` was previously created with `xSemaphoreCreateMutex()` — a non-recursive mutex. Converted to `xSemaphoreCreateRecursiveMutex()` to prevent the entire class of nested-acquisition deadlock:

- `main.cpp:1151` — creation call updated
- All 54 take sites in 12 files — `xSemaphoreTake` → `xSemaphoreTakeRecursive`
- All 56 give sites in 12 files — `xSemaphoreGive` → `xSemaphoreGiveRecursive`
- `PM_SPI_TAKE` / `PM_SPI_GIVE` macros in `include/spi_treaty.h` updated identically
- ISR variants (`PM_SPI_TAKE_ISR` / `PM_SPI_GIVE_ISR`) unchanged — FreeRTOS doesn't permit recursive calls from ISR context, and no code in the codebase currently uses them anyway

Cost is a few microseconds per take/give for the recursion counter. Benefit is that any future code that nests two SPI-taking functions (intentionally or accidentally) works correctly instead of deadlocking.

Also patched a latent bug in `_TREATY_LOG`: when `TREATY_DEBUG=0` the macro expanded to nothing, leaving the surrounding comma expression `(, expr)` syntactically invalid. Surfaced when `wardrive_inspect.cpp` became the first file to actually use the `PM_SPI_TAKE` macro as designed. Fixed by expanding to `((void)0)` when debug is off.

---

## Late-session addendum: Cardputer memory, Wardrive, Mesh reliability

This addendum records the final v1.2.0 stabilization pass after Cardputer ADV hardware logs were available. The Cardputer ADV target is now treated as a real no-PSRAM device in the build and has been validated running WiFi scanning, BLE observer scanning, GPS UART, and LoRa/SX1262 support in the same firmware image.

### Cardputer ADV memory map correction

The raw datasheet number, "512 KB SRAM", was misleading for the wardrive failure. On ESP32-S3, that total includes memory regions that are not all available as normal 8-bit heap. The practical constraint for Cardputer ADV is internal DRAM, with Bluetooth also reducing the available DRAM region when the BT stack is linked.

The initial field symptom looked impossible:

- Wardrive task-spawn heap was approximately 97 KB.
- `WiFi.mode(WIFI_STA)` consumed approximately 52-54 KB.
- `NimBLEDevice::init()` needed approximately 48 KB.
- WiFi + BLE together therefore required roughly 100 KB before GPS, LoRa, UI refresh, SD logging, and BLE advertisement allocations.

The final working build changed the picture completely. Hardware log confirmed:

```
[WIFI-MODE] SCANNER mode active, free heap: 219660
[WARDRIVE/CP] Free heap after WiFi init: 174840
[WARDRIVE/CP] Lazy BLE init done, free heap: 127184
[WARDRIVE/CP] scan: n=24 total=24 mode=1 heap=125020
[GPS] Locked at 115200 baud (... valid_fix=YES)
```

That is the important acceptance result for v1.2.0: Cardputer ADV now runs WiFi STA scanning + NimBLE observer scanning + GPS UART + LoRa-capable firmware with roughly **125 KB free heap** during active wardrive scanning.

### PlatformIO PSRAM flag split

`platformio.ini` now separates shared build flags from PSRAM-only build flags.

- `build_flags_common` contains only flags that are valid for every ESP32-S3 target.
- `build_flags_psram` contains `BOARD_HAS_PSRAM`, `CONFIG_SPIRAM_USE_MALLOC=1`, and the PSRAM cache workaround.
- `tdeck_plus` and `tlorapager` include `build_flags_psram`.
- `cardputer_adv` does not include the PSRAM block and instead defines `DEVICE_NO_PSRAM`.

This matters because Cardputer ADV has no PSRAM. Inheriting `BOARD_HAS_PSRAM` and `CONFIG_SPIRAM_USE_MALLOC=1` made the build behave as if external RAM existed. The runtime allocator had to walk PSRAM-aware paths on a board where every allocation must land in internal DRAM. Removing those flags made the memory model honest and removed one source of allocator unpredictability.

### Static memory reduction (81KB recovered)

The Cardputer memory work also moved large always-on arrays out of global/static storage and into app-lifetime allocations. The key idea is simple: a handheld OS should not reserve RAM for dormant apps.

**Changed areas:**

- `src/mesh_messenger.cpp` — channel message store is allocated when Mesh Messenger opens and freed on exit.
- `src/audio_player.cpp` — playlist storage is allocated only while the audio player is active.
- `src/audio_recorder.cpp` — recording list storage is allocated only while the recorder is active.
- `src/retro_elf_pack.cpp` — ROM list storage is allocated only while browsing RetroPack.
- `src/apps_elf.cpp` — ELF app manifest list is allocated only while browsing ELF apps.
- `src/main.cpp` — Cardputer boot-ring history reduced to a bounded 12-entry ring.

**Measured build impact:**

- Cardputer static RAM dropped from roughly 190 KB to roughly **109 KB**.
- Current final build reports `RAM: 109368 bytes` for `cardputer_adv`.

This also improved user-visible rendering behavior. With less always-on state crowding internal DRAM, app transitions leave a cleaner allocator working set. The Cardputer no longer leaves screen artifacts across app transitions, including in Snake, even though the other targets still show artifact-prone paths in some games.

### GFX framebuffer audit

The Cardputer display path uses `Arduino_ST7789` directly in `src/main.cpp`; it does not instantiate `Arduino_Canvas` or a full-screen `Panel_fb` framebuffer for the 240×135 display. The suspected 63 KB or 126 KB full-framebuffer loss was therefore not the primary RAM sink.

The display and peripheral bus topology is:

- Display: dedicated SPI pins for ST7789V2 (FSPI bus).
- SD + Cap LoRa-1262: separate shared SPI bus (HSPI) protected by the SPI Bus Treaty.

This is a favorable architecture for Cardputer. Display refreshes do not need to contend with SD/LoRa traffic on the same physical bus.

### NimBLE and WiFi sdkconfig attempts documented as non-solutions

The Cardputer environment now keeps an explicit historical note in `platformio.ini`: injecting `CONFIG_BT_NIMBLE_*`, `CONFIG_ESP_WIFI_*`, or coex-related sdkconfig values through PlatformIO build flags does not reliably tune Arduino-ESP32's precompiled framework libraries.

Those flags reach project-built code such as NimBLE-Arduino, but they do not rebuild Arduino-ESP32's precompiled `libbt.a` and `libwifi.a`. The mismatch caused NimBLE init hangs during earlier experiments. The v1.2.0 solution is architectural, not a fake sdkconfig tune:

- reclaim static RAM,
- remove false PSRAM behavior,
- defer expensive boot clients,
- mode-lock WiFi roles,
- lazy-init NimBLE after WiFi scan allocations settle,
- monitor heap and largest internal block during scan cycles.

### Wardrive heavy-load hardening

`src/wardrive.cpp` received an additional Cardputer-specific hardening pass after WiFi+BLE+GPS was proven possible.

Key changes:

- Cardputer BLE callback avoids heap-heavy `std::string` use on the advertisement hot path. MAC and name extraction use fixed `char` buffers.
- NimBLE duplicate cache is capped at 64 entries.
- Cardputer passive BLE scan interval/window is reduced to 240/60, with a 1-second BLE scan window.
- Cardputer WiFi AP result buffer is allocated once and reused instead of repeatedly allocating/freeing scan buffers.
- Serial diagnostics now include largest internal free block: `largest=...`, not just total heap.
- BLE scan windows are skipped under pressure.
- NimBLE is fully deinitialized if heap or largest-block floors are breached, allowing the session to recover instead of crashing in the next advertisement callback.
- The 15-second boot-settling delay was reduced to 100 milliseconds on Cardputer. The other devices spawn the wardrive task at boot and need the settling time; on Cardputer the task is spawned lazily when the user enters the wardrive app, by which point boot has long since settled.
- WiFi-first scan window: the toggle now starts in the WiFi scan branch, ensuring WiFi runs first and frees temporary allocations before NimBLE's lazy init claims its heap. (Previously the toggle started in BLE.)

This does not guarantee that an extreme BLE environment can never stress the device. It does mean the failure mode is now managed: skip or tear down BLE windows before the allocator is driven into the few-kilobyte danger zone.

### Lazy boot init

Cardputer now skips two boot-time subsystems that consume meaningful heap:

- WiFi autoconnect (skipped — user must launch WiFi Connect explicitly; credentials persist on SD)
- Gemini client init (skipped — Gemini chat self-initializes on first entry via existing idempotent path)

Approximately 25KB additional heap recovered at boot. T-Deck Plus and T-LoRa Pager retain both boot-time initializations (PSRAM headroom makes them irrelevant on those devices).

### WiFi mode-locking

New module: `include/wifi_mode.h` and `src/wifi_mode.cpp`.

Tracks current WiFi mode via global enum: `WIFI_MODE_PM_OFF`, `WIFI_MODE_PM_CLIENT`, `WIFI_MODE_PM_SCANNER`. Every WiFi-using app calls `request_wifi_mode(mode, app_name)` at entry. On Cardputer, conflicting modes prompt the user with a modal dialog and require explicit confirmation before tearing down the current mode and initializing the new one. On T-Deck Plus and T-LoRa Pager, the same call is permissive — both modes coexist without prompting.

Wardrive teardown function added (`wardrive_teardown()` in `src/wardrive.cpp`, declared in `include/wardrive.h`): cleanly stops the wardrive FreeRTOS task, deinitializes NimBLE, releases the WiFi driver, closes the session file. Used by the mode-lock layer when switching from SCANNER to CLIENT mode. Recovers approximately 61KB of heap.

### Cardputer launcher sleep UX fix

The Cardputer launcher no longer maps top-level Q/Esc to device sleep. Cardputer has a physical power switch, and Q is used throughout the OS as back/cancel/exit. The top-level launcher now treats Q/Esc as "already home" and does nothing.

Sleep remains available as an explicit launcher action: `SYSTEM → SLEEP`.

This prevents a common field confusion where pressing Q once exits Wardrive back to the launcher, then pressing Q again appears to "crash" the device because the launcher put it into deep sleep.

### Mesh Messenger channel and RX reliability fixes

`src/mesh_messenger.cpp` received the shared fix for Cardputer ADV, T-Deck Plus, and T-LoRa Pager.

**Bug 1: transmitted packets encoded `MESH_HOP_LIMIT` into the header's `hop_start` bits.** The receiver used those bits as a channel hint, so every Pisces-originated packet appeared to belong to channel 3 (`#pisces`) when `MESH_HOP_LIMIT` was 3. TX now uses the active channel:

```cpp
hdr->flags = makeFlags(MESH_HOP_LIMIT, false, ch);
```

**Bug 2: RX used RadioLib's blocking `receive()` path**, which can hold the shared SPI mutex for a long timeout at SF11/BW250. RX now uses RadioLib's interrupt-driven pattern:

- `setPacketReceivedAction(meshRxISR)`,
- ISR sets a `volatile rxFlag`,
- main loop calls `readData()` only when a packet is actually pending,
- radio is re-armed with `startReceive()` after RX, TX, and channel changes.

**Bug 3: a received packet was added to the in-memory message store but the UI did not redraw immediately.** The receive poller now returns a UI-update signal, and the main loop calls `drawMessagesAndInput()` after a valid message.

**Bug 4: mixed old/new firmware could still mark a valid `#general` packet as `hdrch=3`** because old peers were still encoding hop-limit into the channel hint. RX now displays on `currentCh`, the frequency the radio was actually tuned to, while logging the packet header channel separately for diagnostics:

```
[MESH] RX ch0 hdrch=3 from !043a1d: ...
```

Once all devices run the fixed firmware, same-channel messages should report matching channel and header-channel values, for example `RX ch0 hdrch=0`.

### Mesh Messenger MicroSD transcript

Mesh messages are now saved to MicroSD.

`src/mesh_messenger.cpp` appends sent and received messages to:

```
/mesh_logs/messages.csv
```

The transcript format is:

```
millis,direction,channel,header_channel,node_id,sender,rssi,snr,text
```

The implementation is intentionally append-and-close:

- take the SPI Treaty mutex,
- create `/mesh_logs` if needed,
- open `/mesh_logs/messages.csv` with `O_APPEND`,
- write a CSV row,
- sync(),
- close,
- release the mutex.

This costs a little more per message than keeping a file handle open, but it is the right durability tradeoff for a handheld device. A sleep event, app exit, reset, or crash should not lose already written mesh messages. The on-screen message store remains a small RAM ring buffer; the SD card is the durable transcript.

If SD is not ready when Mesh Messenger opens, the app displays a system message that transcript logging is disabled for that run.

### Post-validation Mesh polish pass

After bidirectional Pisces-to-Pisces messaging was validated on hardware across all three devices, a final pre-release pass closed several Mesh-app gaps surfaced by field use.

**T-LoRa Pager channel switching via rotary wheel.** The Pager has no Tab key. The rotary encoder's vertical movement now switches Mesh channels: scroll up = previous channel, scroll down = next channel. The rotary button continues to send the current typed message. T-Deck Plus retains Tab-key channel switching and its existing message-scroll behavior on the rotary. Cardputer retains Tab-key channel switching.

**Cardputer ADV compact two-row input bar.** The 240×135 display previously couldn't fit the channel/status text and the typing cursor on the same row — users could send messages but couldn't see what they were typing. The input bar now uses two compact rows: channel and status on top, typed text on bottom. Long typed text rolls left and displays the newest tail of the buffer, prefixed with a `<` marker when older characters have scrolled off-screen.

**Channel 0 renamed `#general` → `#LongFast`.** The old `#general` name implied a Pisces-internal channel. The new `#LongFast` name correctly identifies channel 0 as the Meshtastic-compatible RF lane. Channels 1-3 retain their Pisces-internal names (`#local`, `#emergency`, `#pisces`).

**LongFast RF parameter correction.** Pisces Mesh previously used coding rate 4/8 (Pisces-to-Pisces compatible but not Meshtastic-compatible). Channel 0 now uses the documented Meshtastic LongFast parameters: 906.875 MHz, BW 250 kHz, SF 11, CR 4/5. This was a single-line fix but a substantive interop unlock — with the wrong coding rate, stock Meshtastic packets were being rejected by the Pisces demodulator at layer 1, before any application code ever saw them.

**16-byte Meshtastic-compatible packet header.** Pisces packets on `#LongFast` now use the Meshtastic 16-byte header shape with `flags`, `channel hash`, `nextHop`, and `relayNode` fields. The LongFast channel hash (`0x02`) is correctly set. This means Pisces packets on the `#LongFast` lane look like legitimate Meshtastic packets at the header layer, even though the payload remains Pisces-format (see Meshtastic interop status below).

**Heard-node visibility.** When a Pisces device receives an undecodable packet on `#LongFast` (a real stock-Meshtastic packet with an encrypted payload, or a Pisces packet from a peer on incompatible firmware), the Mesh app now displays a `~node` system-style row in the message view indicating that raw or encrypted traffic was heard. This converts what was previously a silent failure ("I sent from Meshtastic, nothing happened on Pisces") into an actionable diagnostic ("Pisces heard 3 raw/encrypted packets — Meshtastic devices are nearby but I can't decode their payloads yet").

**Meshtastic upstream attribution.** A Meshtastic attribution block was added near the top of `src/mesh_messenger.cpp` (line 13) explicitly naming the Meshtastic firmware and protobuf repositories, noting their GPL-3.0 licensing, stating Pisces Moon's AGPL-3.0-or-later license, and clarifying that no Meshtastic source file is vendored directly. Complementary attribution lives in `NOTICES.md` at the repository root.

**Meshtastic interop status (as of v1.2.0):**
- RF layer (frequency, modulation, coding rate, sync word): **interoperable.** Pisces and Meshtastic hear each other's packets.
- Packet header layer (16-byte structure, channel hash, hop fields): **interoperable.** Pisces packets on `#LongFast` are correctly framed.
- Payload layer (AES-256-CTR encryption with LongFast PSK, protobuf message encoding): **NOT yet implemented.** Pisces sends and receives plaintext-format Pisces payloads on `#LongFast`. Stock Meshtastic text messages will be heard at the RF and header layers but cannot yet be decoded. Pisces messages will be heard by Meshtastic at the RF and header layers but will not display because they aren't AES-encrypted protobuf.

Full Meshtastic message decode and send is roadmapped as a future architectural layer. The walkie-talkie-style Pisces-to-Pisces messaging on `#local`, `#emergency`, and `#pisces` channels remains plaintext by design — these channels serve a different use case from the Meshtastic-interoperable `#LongFast` lane.

For end-user documentation of the current Mesh state, see the [Mesh status page on fluidfortune.com](https://fluidfortune.com/mesh-status).

### Cap LoRa-1262 PI4IOE5V6408 driver

New: `src/pi4ioe_cap.cpp` and `include/pi4ioe_cap.h`.

Driver for the PI4IOE5V6408 I2C I/O expander on the M5Stack Cap LoRa-1262. Drives the SX1262 RF switch enable so the radio can actually transmit and receive on-antenna. Direct register writes via Wire — no library dependency. Probes I2C address 0x43 at boot; passes silently if absent (Cap LoRa-868 variant, or no Cap attached). The driver is unconditional in the Cardputer build; if the chip is not present, `pi4ioe_cap_init()` logs and returns harmlessly.

### Final build verification

All three release targets build after the memory, wardrive, launcher, mesh RX, mesh transcript, and WASD-to-ESDF changes:

```
cardputer_adv  SUCCESS  RAM 109368  Flash 1863933
tdeck_plus     SUCCESS  RAM 110336  Flash 1801469
tlorapager     SUCCESS  RAM 109060  Flash 1796861
```

Existing SdFat/FS and board-variant warnings remain. No new build breakage was introduced by the shared mesh changes or the per-game input remapping.

---

## Roadmap

Future work (post-v1.2):

- `wardrive_raw_log_start/stop` Bridge commands
- WPS IE parsing from beacon frames for Clinician
- T-LoraPager full hardware validation and app polish
- NFC app for T-LoraPager (ST25R3916) — driver layer works in v1.2.0, app polish pending
- BHI260AP IMU app for T-LoraPager
- Promotion of `request_wifi_mode()` integration to remaining WiFi-using apps (beacon spotter, packet sniffer, probe intel, WPA handshake, Gemini, Bridge HTTP, file manager) — currently only wardrive uses the mode lock. Other apps still call WiFi APIs directly and rely on the user not creating conflicts. v1.2.1 cleanup.
- SPI Treaty defensive improvements: `portMAX_DELAY` → finite timeouts, runtime mutex-holder assertions via `xSemaphoreGetMutexHolder()`, inline pattern documentation in `spi_treaty.h`

Eric will signal when work transitions from v1.2 patches to v1.3 proper.

---

**Pisces Moon OS — Fluid Fortune — fluidfortune.com**
**AGPL-3.0-or-later — DEF CON 34 CFP #1349**
**The Ghost Engine never stops. The SPI Bus Treaty is why.**