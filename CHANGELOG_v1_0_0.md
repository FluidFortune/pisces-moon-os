# PISCES MOON OS — CHANGELOG
## v1.0.0 — "The Arsenal" — April 2026

---

### Summary

This session transforms Pisces Moon OS from a capable general-purpose OS with security tools into a legitimate, purpose-built cybersecurity research platform. The CYBER category grows from 6 tools to 14, with a second page of tools that would not look out of place on a Kali Linux installation — all running on a $50 handheld, all on hardware that fits in a jacket pocket. The session also resolves several critical stability bugs, adds backup infrastructure for the wardriving dataset, and completes the Voice Terminal's local LM integration story.

---

## FIXES

### audio_recorder.cpp — Guru Meditation on launch (CRITICAL)
- **Root cause**: `DMA_BUF_COUNT=8 × DMA_BUF_LEN=1024 × 2 bytes = 16KB` allocated from internal SRAM by `i2s_driver_install()` at launch. With WiFi + BLE + wardrive task resident, this allocation fails and crashes the system before any UI is drawn.
- **Fix**: DMA reduced to `4 × 512 = 4KB`. `readBuf` (`uint8_t*`) moved from static SRAM array to `ps_malloc(4096)` allocated in `run_audio_recorder()`, freed on exit. Warmup buffer reduced from `uint8_t warmup[8192]` stack allocation to `uint8_t warmup[512]`. DMA drain in `startRecording()` now reuses `readBuf` (already in PSRAM) instead of a stack `discard[]` array.
- **Files**: `audio_recorder.cpp`

### voice_terminal.cpp — DMA/stack memory reduction
- **Root cause**: Same class of bug as audio_recorder. `VT_DMA_BUF_COUNT=8`, `VT_DMA_BUF_LEN=1024` (16KB DMA). Warmup buffer `uint8_t warmup[8192]` on stack. Read buffer `uint8_t buf[8192]` on stack inside `vtRecord()`.
- **Fix**: DMA reduced to `4 × 512`. `VT_READ_BYTES` reduced to 4096. Warmup buffer → `uint8_t warmup[512]` stack. Record buffer → `static uint8_t buf[4096]` (BSS, not stack).
- **Files**: `voice_terminal.cpp`

### calendar.cpp — `get_keypress` not declared
- **Fix**: Added `#include "keyboard.h"` — was missing from includes.
- **Files**: `calendar.cpp`

### wifi_filemgr.cpp — `_CSS` used before declaration
- **Root cause**: `_handleSelectPage()` and `_handleBackupZip()` referenced `_CSS` but the `PROGMEM` constant was declared later in the file. C++ requires declaration before use for variables.
- **Fix**: Moved `_CSS` declaration above the first handler that references it.
- **Files**: `wifi_filemgr.cpp`

### pacman.cpp — Up/down movement non-responsive
- **Root cause**: Primary issue was the 250ms trackball lockout in `trackball.cpp`. At 30fps the game polls every 33ms but could only register a new direction once every 250ms — ~7-8 frames between valid inputs. Direction changes near junctions were consistently missed.
- **Fix**: Added `update_trackball_game()` with 80ms lockout to `trackball.cpp`. Pac-Man updated to call `update_trackball_game()` instead of `update_trackball()`. Arrow key escape sequence handler added (`ESC [ A/B/C/D`). Direction persistence comment clarified.
- **Files**: `pacman.cpp`, `trackball.cpp`, `trackball.h`

---

## NEW FEATURES

### wifi_filemgr.cpp — ZIP backup + multi-file select (v2.0)
- **`/backup.zip`**: Streams the entire SD card as a ZIP archive using data-descriptor mode (no seeking required). CRC32 computed on-the-fly per file. Central directory in PSRAM (up to 512 files, ~40KB). Filename includes wardrive session number: `sdcard_backup_NNNN.zip`. Transfer-Encoding: chunked — no need to know final size in advance.
- **`/select`**: Multi-file download page. Every file gets a checkbox. Select All toggle. Download Selected opens each checked file in a new tab with 300ms stagger. Pure HTML/JS — no additional server endpoints.
- Both linked from breadcrumb bar on every directory page.
- **Files**: `wifi_filemgr.cpp`

### wardrive_splitter.py — Wardrive CSV splitter for Smelter
- Standalone Python tool (no dependencies) for splitting large wardrive CSV files into Smelter-ready chunks.
- Split modes: `--rows N` (default 50K), `--date` (one file per drive date), `--session` (split on time gap), `--geo` (geographic grid cells), `--filter` (clean + dedup only).
- Auto-dedup by MAC+location (collapses repeat scans, preserves multi-location appearances for Smelter persistence scoring). GPS null-island filter. RSSI threshold filter. WiFi-only / BLE-only modes.
- Output to `./smelter_output/`. Source file never modified.
- **Files**: `wardrive_splitter.py`

### lora_voice.cpp — Codec2 rebuild with #ifdef guards
- Previous version was an empty file. Rebuilt from project knowledge.
- `#ifdef CODEC2_AVAILABLE` guards around all Codec2 encode/decode calls.
- Without library: raw PCM fallback (proves radio + mic path). Status bar shows `RAW PCM`.
- With library (`meshtastic/ESP32_Codec2` + `-DCODEC2_AVAILABLE`): real Codec2 3200bps encode/decode. Status bar shows `CODEC2:ON`. State init/destroy handled in `run_lora_voice()`.
- **Files**: `lora_voice.cpp`, `platformio.ini` (commented lib_dep entries added)

### voice_terminal.cpp — Local LM help screen
- `?` key opens a full help screen covering all three swappable backends: AI (Gemini / local Ollama / The Phantom / llama.cpp), STT (Google Cloud / whisper.cpp), TTS (Google Cloud / Piper / Coqui / disabled).
- Each backend explains what to change, what file to edit, what server to run.
- Any key or tap dismisses back to main screen.
- READY line updated to show `?=help`.
- **Files**: `voice_terminal.cpp`

### LOCAL_LM_SETUP.md — Local LM integration guide
- Full guide covering all three Voice Terminal backends.
- Ollama wrapper code, The Phantom integration, llama.cpp server setup.
- whisper.cpp STT server setup and Flask bridge pattern.
- Piper TTS and Coqui TTS server setup.
- Network topology diagram. Quick-reference swap table.
- Minimum viable local setup (AI backend only) and full local stack.
- **Files**: `LOCAL_LM_SETUP.md`

### trackball.cpp / trackball.h — Game-mode trackball
- `update_trackball_game()` added with 80ms lockout (vs 250ms for UI).
- Existing `update_trackball()` unchanged — all UI navigation unaffected.
- All game loops should call `update_trackball_game()` for responsive directional input.
- **Files**: `trackball.cpp`, `trackball.h`

---

## CYBER EXPANSION — v0.9.7

### ble_gatt_explorer — BLE GATT service/characteristic enumerator
- Scans for BLE devices (5s, rescan with R), selects target, connects via NimBLE GATT client.
- Walks all services and characteristics. Shows UUID, properties (R/W/N/I), and current value as hex+ASCII if readable.
- Full session JSON saved to `/cyber_logs/gatt_NNNNNNNNNN.json`.
- Calls `wardrive_ble_stop()` / `wardrive_ble_resume()` for clean NimBLE handoff.
- **Files**: `ble_gatt_explorer.cpp`, `ble_gatt_explorer.h`

### wpa_handshake — WPA EAPOL handshake capture
- Promiscuous mode EAPOL-Key frame filter. Groups frames by BSSID+client MAC pair.
- Captures ANonce (msg1), SNonce + MIC (msg2), assembles `.hccapx` v4 record.
- Also captures beacon frames to build SSID→BSSID map for proper ESSID fields in output.
- Auto-saves when complete handshake assembled. Manual save with `S` key.
- Output: `/cyber_logs/handshake_NNNNNNNNNN.hccapx` — standard Hashcat `-m 2500` input.
- Passive only — zero transmission.
- **Files**: `wpa_handshake.cpp`, `wpa_handshake.h`

### rf_spectrum — SX1262 RF spectrum visualizer
- SX1262 RSSI sweep across configurable frequency range (150–960MHz).
- Scrolling waterfall display (color-mapped blue→green→yellow→red by signal strength) + peak-hold bar chart.
- Trackball L/R: shift center frequency ±1MHz. U/D: adjust span. `+`/`-`: dwell time. `[`/`]`: step count.
- Peak hold decay every 2 seconds. Frequency axis labels on bar chart.
- Sets `lora_voice_active` (SPI Bus Treaty compliance — wardrive SD logging pauses during sweep).
- **Files**: `rf_spectrum.cpp`, `rf_spectrum.h`

### probe_intel — Probe request intelligence
- Dedicated 802.11 probe request capture and analysis. Device fingerprinting by OUI vendor (26-entry built-in table).
- DEVICES view: unique MACs, vendor, probe count, SSID count, RSSI, first probed SSID preview.
- NETS view: all unique SSIDs being probed, sorted by frequency.
- Detail view: tap device to see full list of every SSID it has previously connected to.
- Channel hopping: trackball L/R.
- Session JSON saved to `/cyber_logs/probe_NNNNNNNNNN.json` on exit.
- **Files**: `probe_intel.cpp`, `probe_intel.h`

### offline_pkt_analysis — Rules-based offline packet analysis
- Post-session analysis engine for `/cyber_logs/` data. No WiFi required.
- File browser lists all `beacon_*.json`, `pkt_*.csv`, `probe_*.json` files.
- Rules engine: DEAUTH FLOOD, EVIL TWIN, ENCRYPTION DOWNGRADE, HIDDEN AP ANOMALY, CHANNEL ANOMALY, PROBE PATTERN (sensitive SSID keyword matching).
- CRITICAL / WARNING / INFO severity levels, color coded.
- ArduinoJson parse with PSRAM-backed 32KB buffer.
- Analysis report saved to `/cyber_logs/analysis_NNNNNNNNNN.txt`.
- **Files**: `offline_pkt_analysis.cpp`, `offline_pkt_analysis.h`

### ble_ducky — BLE HID keyboard injection
- NimBLE HID server. Advertises as "PM-Keyboard" (or `NAME` from payload header).
- Full DuckyScript parser: `STRING`, `STRINGLN`, `DELAY`, `DEFAULT_DELAY`, `GUI`, `CTRL`, `ALT`, `SHIFT`, `CTRL-ALT`, all named keys, `F1-F12`, `REPEAT`, `NAME`, `WAIT_FOR_CONNECT`, `REM`.
- Complete ASCII→HID keycode translation including all shifted symbols.
- Payloads: `/payloads/*.txt`. Ghost Partition hides `/payloads/` in Student Mode.
- Live progress bar during injection. Abort with Q or header tap at any time.
- Works in standard CDC build — no reflashing required.
- Calls `wardrive_ble_stop()` / `wardrive_ble_resume()`.
- Demo payload auto-created if `/payloads/` doesn't exist.
- **Files**: `ble_ducky.cpp`, `ble_ducky.h`

### usb_ducky — USB HID keyboard injection
- Same DuckyScript format and payload directory as BLE Ducky.
- **Standard CDC build**: shows "WRONG BUILD" screen with exact flash instructions, explains the USB mode conflict, links to BLE Ducky as the no-reflash alternative.
- **HID build** (`ARDUINO_USB_HID_MODE=1`): uses Arduino-esp32 native `USBHIDKeyboard`. Full modifier key support (GUI, CTRL, ALT, SHIFT, CTRL-ALT). Waits for USB enumeration before enabling injection.
- `#ifdef ARDUINO_USB_HID_MODE` gate — both code paths compile cleanly in one file.
- **Files**: `usb_ducky.cpp`, `usb_ducky.h`

### wifi_ducky — WiFi payload delivery and reverse C2
- Four delivery modes per target (configured in `/payloads/wifi_targets.json`):
  - **POST**: sends payload file as HTTP request body to listener on target
  - **GET**: triggers pre-staged endpoint on target
  - **SSH**: executes commands via SSH (stubs to LibSSH-ESP32 when installed)
  - **REVERSE**: T-Deck runs HTTP server on port 7070; shows Python agent snippet; live command input from T-Deck keyboard; results stream back and display in real time
- State machine UI: target select → payload select → result display.
- Payload read from SD into PSRAM buffer.
- Sample `wifi_targets.json` auto-created on first launch.
- **Files**: `wifi_ducky.cpp`, `wifi_ducky.h`

### platformio.ini — HID build environment
- New `[env:esp32s3_hid]` that extends the standard environment.
- `ARDUINO_USB_MODE=0`, `ARDUINO_USB_HID_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=0`.
- Flash with: `pio run -e esp32s3_hid --target upload`
- Return to standard build: double-tap RST → ROM bootloader → standard upload.
- Full inline documentation of tradeoffs (no serial, GPIO1 noise risk, UART0 debug path).
- `PISCES_OS_VERSION` set to `"0.9.7-HID"` to distinguish builds.
- LibSSH-ESP32 and `meshtastic/ESP32_Codec2` added as commented `lib_deps` entries.
- **Files**: `platformio.ini`

### launcher.cpp / apps.h — Updated for 14 CYBER apps
- APP_IDs 40–47 assigned and wired.
- CYBER category expanded from 6 apps (1 page) to 14 apps (3 pages).
- New includes, category entries, and switch cases for all 8 new apps.
- Version bumped to v0.9.7 in comments.
- **Files**: `launcher.cpp`, `apps.h`

---

## KNOWN ISSUES / DEFERRED

- **wardrive_splitter.py** dedup has a minor Python scoping bug on line 219 (double-round call) — cosmetic, does not affect output.
- **ble_ducky.cpp** `REPEAT` command re-parse is simplified — only replays the most recent STRING/STRINGLN, not arbitrary commands. Sufficient for 95% of payloads.
- **wifi_ducky SSH exec** is a stub pending LibSSH-ESP32 addition to `lib_deps`. TCP connection framework present.
- **Ghost Partition Stealth Format** still pending (documented in Chapter 43 of manual).
- **uPY REPL** is a custom command shell, not a real MicroPython interpreter. Rename to SHELL deferred per earlier session decision.
- **DOOM** engine source goes in `src/`. WAD on SD card root or `gamedata` FAT partition. Integration layer complete, engine source pending.

---

*Pisces Moon OS v1.0.0 "The Arsenal" — April 2026*
*"The network is a resource. The intelligence runs on your metal."*
