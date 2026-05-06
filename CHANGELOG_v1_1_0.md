# Pisces Moon OS — Changelog

## v1.1.0 — "ELF Treaty" — May 2026

This release closes a foundational gap in the SPI Bus Treaty and ships
significant stability fixes for the wardriving subsystem and concurrent
SD-using apps. The headline change is **ELF API v1.1** — third-party
ELF modules can now safely access the SD card without violating the
treaty.

---

### ELF API v1.1 — SPI Bus Treaty Compliance

**The problem (v1.0):** ELF modules called `ctx->sd->open()` directly,
bypassing the firmware's `spi_mutex`. Any ELF doing SD I/O could collide
with the Ghost Engine wardrive task on Core 0, active LoRa transmission,
or other firmware-side SPI operations. Result: random reboots, data
corruption, or silent bus arbitration failures.

**The fix (v1.1):** `ElfContext` now exposes nine SPI-safe SD helper
functions — `sd_open_read`, `sd_open_write`, `sd_read`, `sd_write`,
`sd_close`, `sd_exists`, `sd_mkdir`, `sd_remove`, `sd_size`. Each helper
internally takes `spi_mutex` before any bus access. Treaty violations
are now impossible by construction.

ElfContext also exposes `spi_mutex_ptr` for advanced ELF authors who
need direct mutex control.

A handle table tracks open files (max 8 per ELF). `elf_free_psram()`
auto-releases handles on ELF exit so a buggy module cannot leak slots.

API minor version bumped to `1.1`. v1.0 ELFs continue to work but are
treaty non-compliant — they should migrate to the helpers when convenient.

See `docs/ELF_v1_1_MIGRATION.md` for the full migration guide.

**Files changed:**
- `include/elf_loader.h` — ElfContext extended
- `src/elf_loader.cpp` — Helpers + handle table + cleanup

---

### Wardrive — Random Reboot Fix

**The bug:** Long wardrive sessions, especially during USB plug/unplug
events, caused random reboots. Root cause: `String` allocation inside
the `spi_mutex` critical section. PSRAM fragmentation under load
returned garbage `String` objects whose `c_str()` dereferenced invalid
memory, crashing during the `printf` write.

**The fix:**
1. Replaced `String` with stack-allocated `char[]` buffers inside the
   mutex section. No heap allocation while holding the bus.
2. Marked `bt_found` as `volatile` to prevent BLE callback / Core 1 race.
3. Bumped wardrive task stack from 8KB to 12KB to accommodate
   `WiFi.scanNetworks` + BLE callbacks + GPS parse + SD writes.
4. Header tap zone updated from `ty < 30` to `ty < 40` (v1.1 standard).

**Files changed:** `src/wardrive.cpp`, `include/wardrive.h`

---

### SPI Bus Treaty Audit — Apps Fixed

Audited 41 `.cpp` files for treaty compliance during the wardrive +
covert-game scenario (user wardriving while running a game as cover).
Seven files needed mutex coverage on SD operations:

| File | Fix |
|---|---|
| `galaga.cpp` | High-score load/save wrapped in `spi_mutex` (500ms) |
| `pacman.cpp` | High-score load/save wrapped (500ms) |
| `snake.cpp` | High-score load/save wrapped (500ms) |
| `simcity.cpp` | Save/load wrapped (1000ms — bigger 16KB I/O) |
| `doom_app.cpp` | WAD lookup wrapped, header tap fixed to `ty < 40` |
| `notepad.cpp` | `/logs` mkdir + save wrapped, header tap fixed |
| `filesystem.cpp` | **Major refactor** — file viewer now loads to PSRAM under mutex, releases, then paginates from buffer instead of holding the bus during entire viewing session |

The `filesystem.cpp` refactor is the architectural improvement worth
calling out: previously, viewing a long log file would lock the SPI bus
for minutes, starving Ghost Engine. The new approach treats file load
as a brief mutex-held operation followed by mutex-free pagination.

---

### Header Tap Standardization

All apps now use `ty < 40` as the universal "tap header to exit" zone.
This had drifted to `ty < 30` in some files (notepad, filesystem, doom)
which made tapping the header inconsistent with the rest of the OS.

---

### Bridge App — Live Visualizer + Wardrive Streaming

The Bridge app got a complete UI rebuild for v1.1. Previously it showed
static text directing users to a specific URL. Now it's a real-time
window into what the device is broadcasting:

- **Status strip** — device ID (last 8 hex of MAC), host connection
  state, **WARDRIVE state indicator** (IDLE / ACTIVE / STREAM with
  blinking dot when active), total TX events, total RX commands
- **WiFi bar** — fills 0-20 networks detected; over 20 maxes the bar
- **BLE bar** — same pattern with discovered Bluetooth devices
- **LoRa oscilloscope trace** — shared center line, spikes UP for TX,
  spikes DOWN for RX, decays smoothly over ~500ms
- **NFC/RFID pulse** — `*` when active, `.` when idle, `--` when the
  peripheral isn't present (T-Deck Plus has no native NFC/RFID, so
  these stay dim — won't crash if a board ships without them)
- **GPS block** — full lat/lng/altitude/sats when locked, "NO FIX"
  with searching counter when not, "(not present)" if module absent
- **JSON visualizer** — rolling line showing the last event, with
  `<=` for outgoing data and `=>` for incoming commands; flashes
  green on TX events, fades to grey over 600ms

**Bridge → Wardrive Streaming Integration:**

When the host sends `wardrive_start`, Bridge now flips two flags:
- `wardrive_active = true` → starts the Ghost Engine scan task
- `wardrive_bridge_streaming = true` → enables live JSON event emission

The wardrive task emits per-detection JSON events on USB Serial:

```json
{"event":"wifi_seen","mac":"AA:BB:CC:DD:EE:FF","ssid":"PiscesMoon-AP",
 "rssi":-52,"ch":6,"enc":"WPA","lat":34.067219,"lng":-118.20405}
{"event":"ble_seen","mac":"11:22:33:44:55:66","name":"iPhone",
 "rssi":-68,"lat":34.067219,"lng":-118.20405}
```

The host application receives detections in real time without polling.
SSID/name fields are sanitized for safe JSON (quotes/backslashes
replaced with underscores). Streaming events are emitted **outside**
the SPI mutex critical section so they don't extend bus hold time.

`wardrive_stop` clears both flags. Header tap to exit Bridge also
clears the streaming flag so the wardrive task stops emitting if it
keeps running independently.

**Defensive design:** every radio subsystem is queried through a
`*_subsystem_ready()` capability probe. If a peripheral isn't
attached or hasn't initialized, its visualizer sits idle rather
than crashing the bridge.

The `ready` event JSON now correctly reports `"version":"1.1.0"`.

The Bridge app is no longer described as the "WEB BRIDGE" — it's
a general-purpose USB Serial bridge that any host application can
connect to (Lety, the SDL2 emulator, Android tablets, custom dashboards).
The screen reflects this by showing what's leaving the device, not
advertising a specific destination.

**Files changed:** `src/bridge_app.cpp`, `src/wardrive.cpp`, `include/wardrive.h`

---

### Versioning

Version bumped from `1.0.1` to `1.1.0` across:
- `platformio.ini` — `PISCES_OS_VERSION` build flag
- `src/main.cpp` — BIOS screen + splash banner ("ELF TREATY")
- `src/launcher.cpp` — footer version + comment header
- `src/about_app.cpp` — about screen
- ELF API minor: 0 → 1

---

### Compatibility

- **Backward compatible** with v1.0 ELF modules — they still load and run
- **Existing built-in apps** unchanged in behavior — only internal
  treaty compliance was tightened
- **User data preserved** on flash if using the same partition table

### What's Not Changed

- ELF API major version stays at 1 (no breaking changes)
- Partition layout unchanged
- All app IDs preserved
- All file formats unchanged

---

## v1.0.1 — "The Arsenal" — April 2026

(See `docs/CHANGELOG_v1_0_1.md`)

## v1.0.0 — Initial Release — March 2026

(See `docs/CHANGELOG_v1_0_0.md`)
