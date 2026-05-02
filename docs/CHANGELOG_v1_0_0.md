<!--
Pisces Moon OS
Copyright (C) 2026 Eric Becker / Fluid Fortune
SPDX-License-Identifier: AGPL-3.0-or-later

This program is free software: you can redistribute it
and/or modify it under the terms of the GNU Affero General
Public License as published by the Free Software Foundation,
either version 3 of the License, or any later version.

fluidfortune.com
-->

# Pisces Moon OS — Changelog

---

## v1.0.0 "The Arsenal" — April 2026

The first public release. The transition from personal project to open source firmware. This release expands the CYBER category from 6 to 14 apps, replaces the VM proxy architecture with direct API keys for portability, adds the Ghost Engine's per-session wardrive logging, and brings the codebase under AGPL-3.0 for public distribution.

### New — CYBER Expansion

Eight new tools added to the CYBER category, bringing the total to 14 apps across three pages:

- **BLE GATT Explorer** — NimBLE GATT client for service and characteristic enumeration. Reads, writes, and subscribes to characteristics. Logs sessions to `/cyber_logs/gatt_*.json`.
- **WPA Handshake Capture** — EAPOL 4-way handshake capture in promiscuous mode. ANonce, SNonce, and MIC extracted to `.hccapx` format for offline cracking with Hashcat. Auto-saves on complete handshake detection.
- **RF Spectrum Visualizer** — SX1262-based RSSI sweep from 150 MHz to 960 MHz. Scrolling waterfall display with configurable frequency ranges and dwell times. All working buffers in PSRAM.
- **Probe Request Intelligence** — 802.11 probe request analysis. Device fingerprinting via SSID patterns, OUI lookup, and signal characteristics. Three views: DEVICES, NETS, DETAIL.
- **Offline Packet Analysis** — Rules-based post-session analysis of wardrive logs. Six detection rules covering suspicious beacon patterns, deauth floods, and infrastructure anomalies. CRIT/WARN/INFO severity classification.
- **BLE Ducky** — Wireless HID keyboard injection over Bluetooth LE. Full DuckyScript parser. Payloads stored in `/payloads/*.txt`. No re-flash required to change payloads.
- **USB Ducky** — Wired USB HID keyboard injection. Requires the HID build environment (`pio run -e esp32s3_hid`). Standard build shows instruction screen explaining how to switch.
- **WiFi Ducky** — Network payload delivery. HTTP POST/GET to listeners, SSH exec stub, reverse C2 demo channel. Target configuration in `/payloads/wifi_targets.json`.

### New — Shared Text Buffer

- **`text_buffer.h` / `text_buffer.cpp`** — Reusable scrollback utility. PSRAM-backed circular buffer holding up to 200 lines × 56 chars. Word-wrap, trackball scroll, scrollbar indicator, auto-scroll-to-bottom on new content. First consumer: Gemini Terminal. Pattern available for Voice Terminal, SSH client, MicroPython REPL, and any future app displaying variable-length text.

### New — File Manager Enhancements

- **Multi-file select page** (`/select`) — Checkbox interface to download multiple files in one action. Uses programmatic anchor clicks to bypass browser popup blocking.
- **ZIP backup** (`/backup.zip`) — Single-request download of entire SD card contents. Session filename included in archive name.
- **`/ping` endpoint** — Lightweight diagnostic endpoint. No SD access. If this works but `/` doesn't, it's an SD contention issue. If this also fails, router AP/client isolation is blocking the connection.
- **AP isolation warning** on the device screen — many mesh routers have client isolation enabled by default, preventing WiFi devices from reaching each other. The screen now explains the fix.

### Changed — Authentication Architecture

The Gemini authentication was rewritten from OAuth+VM-proxy to direct API keys. This is the single most important change for public distribution — the OS now works without any server-side infrastructure.

- **Gemini Terminal** — OAuth device flow and VM proxy entirely removed. Direct HTTPS to `generativelanguage.googleapis.com` with `GEMINI_API_KEY` from `secrets.h`. Free tier: 15 requests/minute, 1M tokens/day. No billing required.
- **Voice Terminal** — Speech-to-Text and Text-to-Speech now use `GOOGLE_CLOUD_API_KEY` (separate from Gemini key, optional). If empty, Voice Terminal auto-starts in keyboard mode.
- **`secrets.h`** — `PISCES_VM_IP` and `PISCES_VM_PORT` removed. Replaced with `GEMINI_API_KEY` and `GOOGLE_CLOUD_API_KEY`. The file is gitignored; `secrets.h.example` is committed with placeholders and instructions.

### Changed — Ghost Engine

- **Per-session wardrive logs** — Each boot creates a new `wardrive_NNNN.csv` file instead of appending to a monolithic log. Filename exposed via `wardrive_get_log_filename()` for UI display and file manager naming.
- **`sd_in_use` traffic flag** — New global flag alongside `wifi_in_use`. Set by WiFi File Manager while serving, checked by wardrive task before every SD write. The `flushBLEQueue()` and WiFi scan log writes skip their disk operations entirely while the file manager is active, eliminating SD contention during file transfers.
- **Traffic flags relocated to main.cpp** — Both `wifi_in_use` and `sd_in_use` definitions moved from `gemini_client.cpp` to `main.cpp` where they belong architecturally. All consumers extern them.

### Fixed — Critical Bugs

- **Mesh Messenger exit** — Header tap was being consumed by the channel-switching code. The entire header area routed touches to channel tabs regardless of Y position. Now the top half of the header exits, the bottom half switches channels.
- **BT Radar exit** — `pScan->start(3, false)` blocked Core 1 for 3 seconds per scan cycle. During that window the touch handler couldn't see header taps. Changed to `start(0, false)` continuous non-blocking scan.
- **Clock** — Previously displayed `millis()/1000` (uptime since boot) rather than real time. Now performs NTP sync on entry if WiFi is connected. Falls back gracefully to uptime with an on-screen message if no WiFi.
- **Calendar** — Previously hardcoded to March 2026 with day 20 highlighted. Now syncs NTP, calculates correct day-of-week for each month via Zeller's congruence, highlights today, and has trackball-based month navigation with a jump-to-today shortcut (`T` key).
- **Galaga stutter** — `drawHUD()` was issuing a full 320×18 `fillRect` every frame regardless of whether score, lives, or stage had changed. Added dirty tracking (`lastHudScore`, `lastHudLives`, `lastHudStage`); HUD now only redraws when a tracked value changes.
- **Pac-Man Y-axis inverted** — Rolling the trackball up moved Pac-Man down, rolling down moved him up. The trackball Y mapping was reversed relative to the keyboard WASD mapping. Fixed.
- **Snake border artifacts** — The grid border rectangle overlapped cell positions at column 0 and row 0. Every time the snake touched an edge, its cell redraw chewed a hole in the border. Border replaced with horizontal lines above and below the grid that don't intersect cell positions. Grid height corrected from 196 to 192 pixels (exact `ROWS × CELL`), eliminating a 4-pixel dead strip at the bottom.
- **Wardrive BLE count** — `bt_found` was never reset between scan windows. The display showed cumulative devices since boot, climbing indefinitely. Now resets at the start of each 2-second BLE window.
- **WiFi File Manager upload** — The `path` argument from the hidden form field wasn't reliably available during `UPLOAD_FILE_START` in the data callback. Capture moved to `_handleUploadDone` where args are always parsed.
- **WiFi File Manager second launch** — `_server.stop()` left the TCP socket in a half-closed state and old handlers still registered. Added explicit `_server.close()` before `_server.begin()` on every launch so multi-session use works reliably.
- **Audio Recorder SRAM pressure** — DMA buffers were 8×1024 (16KB), static `readBuf` was 8KB in BSS, warmup and discard were 8KB each on stack. Total 40KB SRAM contention. Reduced to DMA 4×512 (4KB), `readBuf` relocated to PSRAM via `ps_malloc`, warmup reduced to 512 bytes, discard reuses `readBuf`.
- **Voice Terminal SRAM pressure** — Same pattern as Audio Recorder. Same fix.
- **Offline Packet Analysis forward reference** — `OAFile*` pointer declared before the `OAFile` struct definition. Reordered so struct exists before pointer. Also replaced deprecated `DynamicJsonDocument(32768)` with `JsonDocument` for ArduinoJson 7.x compatibility.
- **`gemini_client.h` fallback defines** — Added `#ifndef GEMINI_API_KEY` / `#ifndef GOOGLE_CLOUD_API_KEY` guards so the build doesn't fail with undeclared identifier errors if `secrets.h` is missing or outdated. Undefined keys fall through to empty strings; `gemini_has_key()` returns false gracefully.
- **Launcher app array** — `AppEntry apps[8]` was too small for the 14-app CYBER category. Expanded to `apps[16]`. Also removed the early OAuth block that triggered `gemini_oauth_flow()` at boot — no longer needed with API key auth.
- **USB Ducky build errors** — `NimBLEHIDDevice` API mismatch resolved by rewriting as raw GATT server. `USBHIDKeyboard` not bool-convertible; `USB.connected()` doesn't exist in ESPUSB; both replaced with a 5-second warm-up delay before first injection. Orphaned `lastConn` variable removed.

### Fixed — GPS

- **UART RX buffer** — Increased from default 256 bytes to 512 bytes to prevent overflow during WiFi scan operations when GPS NMEA sentences arrive in bursts.
- **GPS drain loops** — Added before and after `WiFi.scanNetworks()` to keep GPS parsing current during the scan window where it would otherwise starve for CPU time.
- **Auto-baud hunter** — Alternates between 38400 and 9600 baud every 5 seconds of silence to automatically adapt to whichever GPS module variant is installed (L76K uses 9600; some variants use 38400).

### Added — Licensing

- **AGPL-3.0-or-later** — All first-party source files now carry the license header. Third-party game engines (Pac-Man, Doom, Galaga, SimCity) are exempt and retain their original licensing.
- **`LICENSE` file** in the repo root contains the full AGPL-3.0 text.
- **README.md** — Complete rewrite for public consumption. Hardware requirements, installation, API key setup, app descriptions by category, WiFi file manager usage, SD card layout, dependency table, and contribution guidelines.
- **`docs/GETTING_YOUR_API_KEY.md`** — Step-by-step for both the free Gemini API key and the optional Google Cloud Speech/TTS key. Includes troubleshooting and security notes.

### Developer-Facing Changes

- **PlatformIO default environment** — Added `default_envs = esp32s3` to `[platformio]` section. `pio run` no longer builds both environments by default. Use `-e esp32s3_hid` explicitly when you need the USB HID build.
- **`wardrive.h`** — Added `wardrive_get_log_filename()` declaration. Added `sd_in_use` extern.
- **`main.cpp`** — Added `wifi_in_use` and `sd_in_use` definitions.

### Known Limitations

- **WiFi File Manager AP isolation** — Some mesh routers (Eero, Google WiFi, Orbi) and ISP-provided routers enable client isolation by default, which blocks device-to-device traffic on the same WiFi. The file manager shows a warning on-screen. The fix is router configuration, not firmware.
- **GPS cold start** — Without a battery-backed backup RAM, each full power cycle is a cold start. First fix after power-on takes 30-90 seconds outdoors. Warm starts (within the backup RAM retention window) take 5-15 seconds.
- **Voice Terminal TTS playback** — The Text-to-Speech response is saved to `/tmp_tts.mp3` on SD card. Direct I2S playback integration is pending a future release. Transcription and AI response flow work fully.
- **Chess, Baseball, Voice Terminal scrollback** — Share the same "painted and forgotten" pattern that Gemini Terminal had. Will be migrated to `text_buffer.h` in a follow-up release.

---

## Earlier Releases

### v0.9.6 "ELF On A Shelf"
- ELF application loader with PSRAM-backed runtime linking
- Retro emulator pack (NES, Game Boy, Atari) loadable from `/apps/`
- Voice Terminal, SSH client, MicroPython shell, Gamepad setup
- Expanded INTEL category

### v0.9.5
- COMMS category additions: LoRa PTT, Mesh Messenger
- SYSTEM category expansion

### v0.9
- Initial alpha
- Ghost Partition dual-partition security model
- Wardrive Core 0 background task
- SPI Bus Treaty architectural protocol
- Launcher, PIN authentication, BIOS boot screen
- Core apps: GPS, Wardrive, Notepad, Calculator, Clock, Calendar, Etch, Snake, Pac-Man, Galaga, Chess, File Browser, About, System

---

*Pisces Moon OS — fluidfortune.com*
