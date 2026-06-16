# Pisces Moon OS — v1.2.1 Changelog

**Codename:** Court Jester
**Release date:** 2026-05-27
**For:** Jennifer Soto

---

## Overview

v1.2.1 extends the Multi-Device foundation laid in v1.2.0 with two new device targets, four new applications, the first multi-device voice terminal, and a body of stability fixes from real hardware bring-up.

The C28P (LCDwiki 2.8" ESP32-S3 Display) joins the supported device family as the first kiosk-class member — desk-fixture, touch-only, full-duplex audio. The Heltec V4 (Heltec WiFi LoRa 32 V4) is pinned to the v1.2.1 target list with hardware bring-up beginning next session; the codebase compiles for it pending hardware verification.

All four currently-shipping device builds (T-Deck Plus, T-LoRa Pager, Cardputer ADV, C28P) compile clean from a fresh tree.

---

## New device targets

### C28P (LCDwiki 2.8" ESP32-S3 Display)
- ESP32-S3R8 — 8MB octal PSRAM, 16MB flash
- ILI9341V 240×320 native portrait, FT6336G capacitive touch
- ES8311 codec + MEMS microphone + FM8002E audio amplifier
- MicroSD via SDIO 4-bit
- WiFi + BLE (no LoRa, no GPS, no keyboard, no buttons — touch only)
- Form factor: desk fixture / kiosk

### Heltec V4 (pinned, hardware verification pending)
- ESP32-S3R2 — 2MB PSRAM
- 320×240 capacitive touch (same form factor as C28P)
- SX1262 LoRa, L76K GNSS
- B2B expansion socket, pin-compatible with V3/V4 ecosystem
- Shares portrait layout code path with C28P throughout the codebase

---

## New applications

### Weather (`weather.cpp`)
Multi-device Open-Meteo client with per-device layouts:
- **Portrait (C28P, Heltec V4):** vertical card layout, current conditions + 3-day forecast, touch refresh
- **T-Deck Plus:** 2-column layout, keyboard `q`/`r` shortcuts
- **Cardputer ADV:** compact single-screen layout
- **T-LoRa Pager:** wide 3-column forecast strip

Default location Pasadena, CA. Persists user-set location in NoSQL "settings" category. No API key required (Open-Meteo is free for personal use).

### RSS reader (`rss.cpp`)
Multi-device RSS client with per-device layouts (same matrix as Weather). Default-seeds four feeds on first run: BBC World, NPR, AP Top, Hacker News. Tiny built-in parser handles `<item>`/`<title>`/`<description>` with CDATA and basic HTML stripping. Persists feed list in NoSQL "rss_feeds" category. Article detail view on selection.

### Media (C28P only — `c28p_media.cpp`)
Three-section media app:
- Voice recorder UI with WAV file creation (records silent WAV in v1.2.1; actual I2S microphone capture lands v1.3)
- Voice recordings library
- Photos placeholder section

### System settings (C28P only — `c28p_system.cpp`)
Real settings screen replacing the previous ABOUT-redirect:
- Backlight slider (persists to NoSQL; GPIO wiring pending pin identification)
- Sound toggle
- Storage statistics
- WiFi state display
- ABOUT button
- Factory reset (gated as "not yet implemented" pending `nosql_clear_category()`)

### Voice terminal — multi-device (`voice_terminal.cpp`)
First multi-device voice terminal with per-device input modalities and TTS playback wired:

- **C28P / Heltec V4:** Touch-and-hold red record button. Full pipeline: ES8311 mic → Google Speech-to-Text → Gemini → Google Text-to-Speech → speaker playback via ESP32-audioI2S
- **T-Deck Plus:** SPACE-to-record (preserves v1.2.0 behavior). Uses ES7210 mic chip path, hardcoded speaker pins matching `audio_player.cpp`
- **T-LoRa Pager:** Keyboard mode at startup. Gemini text input + TTS speaker output. Microphone capture deferred to v1.3.
- **Cardputer ADV:** Keyboard mode. Gemini text input only. Microphone TBD.

TTS playback (the missing piece from v1.2.0) is now wired via the ESP32-audioI2S `Audio` library on all speaker-equipped devices.

---

## C28P launcher

Eight-tile launcher, all functional:

| Tile | Function |
|---|---|
| AUDIO | MP3 player on `/audio/` from SD |
| AI | Gemini text terminal with on-screen QWERTY |
| WEATHER | Multi-device weather (this device: portrait layout) |
| RSS | Multi-device RSS reader |
| MEDIA | Voice recorder UI + library + photos stub |
| TOOLS | Sub-launcher: WARDRIVE, SURVIVAL, MEDICAL, HISTORY, WIFI, ABOUT |
| GAMES | Sub-launcher: Tetris, Snake, Pac-Man, Galaga |
| SYSTEM | Real settings (backlight cosmetic, factory reset stubbed) |

### C28P UX polish
- **Unified exit-bar pattern across all games.** Top 14px is universal quit zone painted by `c28p_dpad.cpp`; games paint score/HUD into the right side of the bar rather than a separate header that overpaints.
- **Sub-launcher pagination** (5 items per page with PREV/NEXT controls).
- **WiFi setup screen rewritten** with explicit BACK, real scan feedback, 5-step portal instruction screen, and FORGET button.
- **Wardrive UI shows current-scan counts** ("NOW SEEING") rather than lifetime cumulative totals.
- **Game-over screens updated with D-pad-appropriate prompts** (A=RETRY, B=QUIT).

---

## Architecture additions

### `c28p_wardrive_engine.cpp`
C28P-specific replacement for the shared mobile `wardrive.cpp`. Keeps the mobile engine clean and gives C28P a stationary-optimized scan loop tuned for desk-fixture usage patterns.

### JenEricFS / JenFS (v1.3 deliverable, design complete)
1,144-line implementation tutorial for a custom MicroSD filesystem providing access control through filesystem identity. Threat model: protection against casual consumer-OS adversaries who see the card as "unformatted, want to format?" Three-layer stack envisioned: Ghost Partition / JenFS / LUKS each addressing different adversary classes. Python format tool and inspection utilities specified. Implementation queued for v1.3.

### Pisces Moon Linux (Jennifer WebOS native variant)
Integration specification (697 lines) for the Pi Zero 2W + Argon POD Modular System touchscreen variant of Jennifer WebOS. Same backend software, dual-identity naming:
- **Jennifer WebOS** when accessed remotely (phone/tablet/laptop browser)
- **Pisces Moon Linux** when rendered natively on Argon POD Display Module

Spec defines Track A (serial logger backend extension covering universal raw capture, multi-device support, device identification cascade) and Track B (touchscreen UI layer with C28P design language ported to Chromium kiosk).

---

## Bug fixes

### Critical
- **`DEVICE_TDECK` vs `DEVICE_TDECK_PLUS` guard mismatch** in `weather.cpp` and `rss.cpp`. T-Deck Plus builds were falling through to the "not configured for this device" fallback message instead of rendering the real UI. Affected 7 occurrences across both files. Caught by code review (credit: Codex). All occurrences corrected.

- **`c28p_run_media()` undefined at link time**. New `c28p_media.cpp` had been added to project tree without the `.cpp` extension. File-naming verification step added to development workflow.

- **`PM_KEY_ENTER` undeclared constant** across 6 occurrences in `rss.cpp`. Replaced with literal character comparisons (`input.key == 13 || input.key == 10`).

- **Multi-device pin assumption errors** in `voice_terminal.cpp`:
  - Assumed `PIN_I2S_SCLK/LRCK/DOUT` macros exist on T-Deck Plus (they don't — T-Deck Plus speaker pins are private inside `audio_player.cpp`)
  - Assumed `c28p_audio_begin()` and `c28p_audio_stop()` are available on T-LoRa Pager (they're not — C28P-only)
  - Resolved by per-device branches with appropriate pin literals and codec-init paths.

### NimBLE / wardrive
- **Memory pressure defenses** unchanged from v1.2.0 (CP_SCAN_MAX_APS=24, CP_BLE_SCAN_MIN_FREE_HEAP=70000, NimBLE deinit recovery), but serial logger integration spec (in Jennifer WebOS) prepared for v1.3 diagnostic capture of the dense-RF BLE-drop pattern (>30 visible WiFi networks).

### Build system
- Pinned IRremote dependency disambiguation noted (7 packages match bare `IRremote @ *`; recommended explicit pin to `z3t0/IRremote@^4.7.1`).
- Voice terminal v1 → v2 migration: `voice_terminal_v2.cpp` superseded `voice_terminal.cpp`; both shipped briefly during development causing multiple-definition link error; resolved by overwriting v1 with v2 contents.

---

## Addendum — 2026-05-28 (post-release session)

Continuation work after the v1.2.1 release date. C28P focus: build-filter correction, audio-recording root cause, and a measurement-first microphone diagnostic. All changes verified on disk via MCP filesystem.

### C28P build filter corrected to multi-device sources
The C28P `build_src_filter` in `platformio.ini` still referenced the pre-multi-device `c28p_weather.cpp` and `c28p_rss.cpp`. Swapped to the shipping multi-device `weather.cpp` and `rss.cpp`, and added `voice_terminal.cpp` (previously absent from the C28P filter entirely). The multi-device files carry `c28p_run_weather()` / `c28p_run_rss()` aliases, so the existing C28P launcher dispatch resolves unchanged; the superseded `c28p_weather.cpp` / `c28p_rss.cpp` are now out of the build. Advances the "Cross-device launcher integration" limitation for C28P (Voice Terminal now reachable on this target).

### Audio recording — root cause identified (all devices)
The "records a half-second blip / silent WAV on every device" symptom is **not** a sample-rate or compression issue. Source review found the shared defect: every record path configures the ESP32 I2S engine but never performs a full microphone-codec ADC bring-up.
- **C28P:** `c28p_audio.cpp` installs I2S0 **TX-only** and initializes the ES8311 **DAC-only**; `voice_terminal.cpp`'s `es8311_adc_enable()` then writes only two ADC registers (0x14, 0x17), which is insufficient to power the analog input path.
- **T-Deck Plus:** `audio_recorder.cpp`'s `initI2SMic()` installs the I2S RX peripheral on the ES7210 pins but issues **no ES7210 I2C register sequence** — the codec is never told to produce data.
- A partial/absent codec sequence is indistinguishable from a correct one by symptom (flat, near-zero samples), so the fix is gated on measurement, not guessing.

This supersedes the v1.2.1 "voice recorder records silent WAV" software-gap entry with a concrete cause, and refines the "ES8311 ADC register sequence uses best-read-of-datasheet values" entry — see the verified sequence below.

### C28P microphone diagnostic (`c28p_mic_probe.cpp`, new)
Self-contained measurement tool, reachable from **TOOLS → MIC TEST** (sub-launcher now 7 items / 2 pages). No cloud, no STT, no touch-hold logic — one button runs a fixed 3-second capture and reports hard numbers to serial: `bytesRead`, min/max, RMS, zero-crossings, first-16 samples, plus a one-line VERDICT (NO DATA / FLAT / NEAR-SILENT / ENERGY PRESENT). Draws a live level meter and best-effort writes `/mic_probe.wav` to SD for off-device inspection.
- **Verified register provenance:** the ES8311 ADC bring-up is traced line-by-line to Espressif's hardware-tested `esp-bsp` driver (`components/es8311/es8311.c`) — `es8311_init()` (reg00 0x80 power-on, 0x0D/0x0E/0x12/0x13/0x1C, DAC 0x37) and `es8311_microphone_config()` (reg17 0xC8 ADC gain, reg14 0x1A analog-mic + PGA), with clock registers 0x01–0x08 from the `{4096000, 16000}` coefficient row. The deltas this exposes against C28P's current `es8311_init()`: the **0x80 power-on command, ADC 0x1C EQ/DC-offset, and ADC 0x17 gain are all absent**, and there is no RX path. These are the precise additions the real recorder/Voice Terminal fix will need once the probe confirms them on hardware.
- **I2S ownership caveat:** the probe owns I2S0 (uninstalls any prior driver first, since the ES8311 shares one serial port between ADC and DAC). `c28p_audio.cpp` keeps a static `i2s_initialized` flag the probe cannot reset, so game-tone audio will not re-init until reboot after a probe run. The clean resolution is a C28P audio HAL with explicit `idle/playback/record` modes (queued for v1.3).

### Record trigger UX (Codex)
Keyboard-equipped devices now use **`R`** to start/stop recording instead of SPACE, and the generic touch record-button toggle was removed from `audio_recorder.cpp`. The **C28P remains the only device that records via point-touch.** Rationale: on T-Deck Plus, GPIO0 (trackball click) is shared with the ES7210 microphone hardware and is unavailable while the I2S mic is active, so `R` sidesteps the conflict.

---

## Addendum — 2026-06-01 (session: games, e-reader, SD fixes, wardrive, UI hardening)

Large multi-topic session. Key themes: C28P MicroSD fully working, game suite expansion, e-reader shipped, Maxine chess corrected, wardrive CSV logging, backlight + factory reset wired, and audio codec fixes.

### Games — Maxine display passes (4 games)

- **Pole Position:** Maxine geometry (480×520, HORIZON=180, CAR_Y=460), grass pre-fill flicker fix extended from Cardputer to Maxine (~163k pixels saved per frame), `vy()` top-anchor, `maxine_dpad_render()`, 30fps cap.
- **Pac-Man:** Maxine geometry (TS=14, 480×520, HUD_H=22, MAZE_TOP=24), `viewY()` top-anchor, Maxine `fillRect` stage-clear guard, Maxine HUD (size-2 text, spread across 480px, pac-dot lives indicator), `maxine_dpad_render()`.
- **Galaga:** Maxine geometry (480×520, PLAY_W=440), pre-existing recursive bug fixed (`clearGameArea` called itself on non-C28P branch instead of `fillScreen`), Maxine `clearGameArea`, Maxine HUD (size-2, ship-triangle lives), `maxine_dpad_render()`.
- **Chess — Maxine port:** SQ=52, BOARD_X=32, BOARD_Y=30, horizontal panel below board (PANEL_Y/PANEL_H/PANEL_W=480), `maxine_touch_read` input path, `maxine_dpad_render()`, `drawPanel()` rewritten for Maxine layout, `drawFull()` fillScreen→fillRect (preserves dpad chrome), `showMessage()` scaled for Maxine, game-over wait uses `maxine_touch_read`.

### Chess — C28P port

- SQ=28, full 240×320 touch-only viewport (no dpad reservation).
- 14px quit strip at top, 224px board, 72px panel below.
- Panel: turn indicator + tappable AI-cycle button (cyan border, PANEL_Y+24..+45) + control hints.
- Exit: top 14px tap. Input: board tap = cursor+confirm. AI cycle: tap AI button area.
- Wired into `c28p_boot.cpp` GAMES sub-launcher (count 15→16).

### Chess — compile fixes

- **PS macro collision:** `xtensa/specreg.h` `#define PS 230` pulls in transitively via `Arduino.h → FreeRTOS → portmacro.h`. Renamed chess piece-scale constant to `CPS` (Chess Piece Scale), all ~50 references updated.
- **`bx`/`by` conflicting declarations** in `showMessage()`: Maxine and T-Deck paths declared same names in same scope. Fixed with `{ }` scope block around Maxine path.

### Chess — Maxine dpad removed (correction)

Chess on Maxine was incorrectly rendering the dpad chrome (reserved bottom strip + `maxine_dpad_render()` call). Chess is a touch-native app; the dpad reservation wastes screen real estate. Corrected:

- `maxine_dpad.h` include removed from `chess.cpp`.
- Maxine geometry updated: SCREEN_H_C 520→800, PANEL_H expands to fill 800px (338px panel, vs 72px before).
- `drawFull()` simplified to `fillScreen` + Maxine header strip ("< EXIT" left + "CHESS" title centered).
- `maxine_dpad_render()` call removed from `run_chess()`.
- Maxine tap-to-quit hot zone added (top 30px strip, fresh `mqx`/`mqy` locals to avoid collision with T-Deck `tx`/`ty`).
- T-Deck path restructured from `#elif !defined(DEVICE_MAXINE)` negative-conditional to explicit `#elif defined(DEVICE_MAXINE) … #else` chain.

### E-Reader — full implementation (`ereader.h` / `ereader.cpp`, ~32KB)

Complete e-reader for all 5 devices: T-Deck Plus, T-LoRa Pager, Cardputer ADV, C28P, Maxine.

- Forward-streaming paginator with 64-entry back-history stack (precise prev/next without re-reading from start).
- Never loads full file into RAM — 4KB chunk reads; all book sizes equal in memory cost.
- Auto-save offset on exit, explicit bookmark support. Per-book `.bm` files at `/books/.<filename>.bm`.
- Touch model (C28P/Maxine): tap top = exit, body left/right = prev/next, 3-button footer.
- Keyboard model: SPC/→ = next, BKSP/← = prev, B = bookmark, Q = exit.
- Scans `/books/`, `/Books/`, `/BOOKS/`, skips dotfiles.
- Launcher wiring for all 5 devices was pre-staged.

### E-Reader — C28P SD dual-backend fix

`ereader.cpp` used `extern SdFat sd` exclusively. On C28P, `sd` exists but is never `begin()`'d (SD_MMC is used instead). Added adapter layer at top of `ereader.cpp`:

- `ErFile`, `ER_OPEN_READ`, `ER_OPEN_WRITE`, `ER_EXISTS`, `er_open_next`, `er_is_dir`, `er_get_name`.
- `#ifdef DEVICE_C28P`: `fs::File` / `SD_MMC` / `FILE_READ` / `FILE_WRITE` / `openNextFile()` / `isDirectory()` / `f.name()` with VFS prefix stripping.
- `#else`: `FsFile` / `SdFat sd` / `O_READ` / `O_WRITE|O_CREAT|O_TRUNC` / `openNext()` / `isDir()` / `getName()`.
- All 6 SD touchpoints updated: `er_scan_folder`, `er_bookmark_load`, `er_bookmark_save`, `er_has_bookmark`, `layout_page` signature, `reader_run`.
- Compile fix: `er_bookmark_load()` — `fs::File::read()` requires `uint8_t*` strictly; cast added (`char*` was SdFat-tolerant but fails with `fs::FS`).

### C28P SD initialization — root cause resolved (`main.cpp`)

All C28P file writes were failing with `[NOSQL] Cannot create /data/` storms because `nosql_store.cpp`, `c28p_media.cpp`, and `c28p_audio_app.cpp` used `extern SdFat sd` which is never mounted on C28P (card wired to SDMMC peripheral, not SPI).

Architecture note: T-Deck Plus / T-LoRa Pager / Cardputer ADV / Maxine wire their MicroSD to SPI; SdFat speaks SPI. C28P wires to the ESP32-S3's dedicated SDMMC peripheral (CLK=38, CMD=40, D0-D3=39/41/48/47) at up to 24 Mbit/s. SdFat 2.2.3 has no SDIO mode for ESP32-S3. The only path is `SD_MMC`. Fix applied per-file using `#ifdef DEVICE_C28P` / `#else` split — working devices unchanged:

- **`nosql_store.cpp`:** adapter macros `NOSQL_FS` / `NOSQL_FILE` / `NOSQL_MODE_READ` / `NOSQL_MODE_WRITE` resolve to `SD_MMC` + `fs::File` on C28P, `sd` + `FsFile` on others. Ten direct `sd.xxx` calls replaced with `NOSQL_FS.xxx`.
- **`c28p_media.cpp`:** `<SD.h>` → `<SD_MMC.h>`, four `SD.xxx` → `SD_MMC.xxx`.
- **`c28p_audio_app.cpp`:** same swap; removed duplicate `SD.begin()` call (SD_MMC already mounted at boot); `connecttoFS(SD, path)` → `connecttoFS(SD_MMC, path)`.

### Wardrive — CSV logging added (C28P + Maxine)

Both kiosk wardrive engines previously logged JSON-only via NoSQL. CSV parity with mobile engine added:

- Filename rotates: `/wardrive_001.csv` → `/wardrive_002.csv` … up to 999.
- Header: `bssid,ssid,rssi,channel,encryption,t_ms,observed_uptime_s`.
- SSIDs RFC 4180-quoted (handles embedded commas and quotes).
- Flushes every 32 rows; opens at task start, closes cleanly at task exit.
- `wardrive_get_log_filename()` returns current session's CSV path for Bridge / file manager access.
- Separate `WD_`-prefixed macros keep SD backend self-contained (does not share with nosql macros).
- Both C28P (`SD_MMC`) and Maxine (`SdFat`) backends.

### Wardrive — BLE display fix (Cardputer + T-Deck)

`bt_found` is reset to 0 at the start of every BLE scan window (~every 4 seconds). The UI refreshes at 500ms intervals, so most refreshes caught `bt_found` at 0 — showing "0 BLE" even while data was flowing. Fix: added `bt_display_cached` (static, persists across redraws) that only updates when `bt_found > 0`. Display now holds the last non-zero per-window count rather than blinking to 0 between scan windows. Applied to both Cardputer and T-Deck/Pager paths.

### System app — backlight and factory reset wired (`c28p_system.cpp`)

Both were stubs in v1.2.1:

- **Backlight:** `c28p_set_backlight()` now drives LEDC PWM on `PIN_LCD_BL` (GPIO 45) at 5kHz, 8-bit resolution. Handles ESP32 Arduino 3.x (`ledcAttach`) and 2.x (`ledcSetup` + `ledcAttachPin`) via `ESP_ARDUINO_VERSION_MAJOR` guard. 16-PWM floor prevents fully-black screen at level 1.
- **Factory reset:** now wipes four NoSQL categories (`wardrive`, `ble_log`, `settings`, `anomaly_baseline`) with per-category visual feedback (OK/FAIL in green/red). In-memory backlight + sound preferences reset to defaults and reapplied after wipe. Does **not** touch user content (`/recordings`, `/audio`, game high-score files, e-Reader bookmarks). "True card wipe" remains a desktop reformat operation.
- **`nosql_clear_category(const char* category)`** added to `nosql_store.h` and implemented in `nosql_store.cpp` (both SdFat and SD_MMC backends): walks `/data/<category>/`, deletes every entry file, removes index, rmdirs category folder.

### Audio — ES8311 codec register fixes (`c28p_audio.cpp`)

Two register errors caused silent output and inoperative ADC:

- **`REG00` power-on command missing.** The ES8311 analog section never powered up. Added `0x80` write to `REG00` (power-on command) from Espressif's `esp-bsp` reference driver.
- **`CLK1 = 0x30` → `0x3F`.** Not all clock paths were enabled; ADC clock path was inactive.
- **Added missing ADC registers:** `REG1C = 0x6A` (ADC EQ bypass + DC-offset cancellation), `REG17 = 0xC8` (ADC PGA gain), `REG16 = 0x00` (ADC digital gain scale). These were entirely absent from the prior init sequence.
- **`SDP` registers confirmed at `0x0C`** (correct for 16-bit I2S per esp-bsp; prior attempted change to `0x00` was reverted — `0x0C` encodes bits[3:2]=11 = 16-bit in the ES8311 SDP format).
- **`DAC_REG37` confirmed at `0x08`** (also matches esp-bsp; prior attempted change to `0x48` was reverted).

### Media app — voice recording implemented (`c28p_media.cpp`)

`start_recording()` was a UI stub (created WAV header, no I2S capture). Now fully implemented:

- Initializes codec via `c28p_audio_begin()`, installs I2S in RX mode, discards DMA warmup garbage, opens WAV file.
- `run_media()` loop polls `i2s_read()` every 10ms, writes samples to disk.
- `stop_recording()` stops I2S, patches WAV header with real byte count.
- At 16kHz × 16-bit mono: ~960KB per 30-second recording.

### Media app — library playback implemented (`c28p_media.cpp`)

LIBRARY tab previously drew file list but had no tap handler. Now:

- Tap any row → starts playback; row highlights green with `»` indicator, header shows `» PLAYING`.
- Red STOP button appears at bottom during playback.
- Auto-stops at end of file and clears indicator.
- On exit (top strip tap): `stop_media_playback()` called before return — no dangling I2S state.

### PDF-to-text host script (`tools/pdf_to_text/`)

Three files for preparing books for the e-reader:

- **`pdf_to_text.py`:** three-tier extraction — PyMuPDF → pdfminer.six → Tesseract OCR. Fallback trigger: <50 chars/page average signals scan-only PDF. Preserves paragraph structure, no hard-wrap (e-reader reflows). OCR degrades gracefully if `pytesseract`/`pdf2image` not installed. FAT32-safe output filenames.
- **`requirements.txt`:** required + optional split.
- **`README.md`:** install per-OS, usage, troubleshooting, workflow with the e-Reader.

### Mario Bros — arcade original (new `src/mario_bros.cpp`, `include/mario_bros.h`)

Previous `mario_bros.cpp` was a Super Mario Bros side-scroller. Deprecated and replaced with the original 1983 arcade game (fixed screen, platform-punch mechanic).

**Deprecated side-scroller:**
- Moved `src/mario_bros.cpp` → `temp_hold/super_mario_deprecated.cpp`.
- Entire body wrapped in `#if 0 … #endif` (preprocessor-level disable).
- Deprecation banner added explaining what it was and how to revive.
- No launcher or build filter changes required (file is outside `src/`).

**New arcade implementation (`src/mario_bros.cpp`):**
- Fixed-screen layout. Per-device platform geometry (Cardputer: 4 platforms; others: 5; Maxine: generous).
- **Mechanics:** punch-from-below to flip enemies, kick flipped enemies off-screen before recovery.
- **Enemy state machine:** spawn → walk → flipped → recovering → angry → dead.
- **Three enemy kinds:** Shellcreeper (turtle, 1 hit), Sidestepper (crab, 2 hits), Fighter Fly (hops, only flippable mid-ground).
- **POW block:** 3 uses, visible crack progression, finishes crabs regardless of HP.
- **Phase table:** 10 entries cycling with +0.20 speed_mul per loop; ice rounds on phases 4 and 8; enemy type escalation.
- Side wraparound on every platform.
- High score persisted: `/mario_bros_hs.txt` (SD_MMC on C28P, SdFat + `spi_mutex` elsewhere).
- Title screen; game-over with "NEW HIGH SCORE!" badge.
- 60fps target except Cardputer (30fps for ST7789).
- **C28P input:** left/right dpad + A button to jump (correct mapping).

### New apps — soft keyboard, clock/timer, BLE tracker scanner

Three new apps wired into C28P and Maxine TOOLS sub-launchers:

#### Soft keyboard component (`pm_text_input.h` / `pm_text_input.cpp`)
Unified text entry API: `pm_text_input(prompt, out, outlen, initial)`.
- **Touch (C28P, Maxine):** renders on-screen QWERTY. Input field at top (~60px), 5-row keyboard filling bottom. Keys scale proportionally (Maxine gets larger keys).
- **Keyboard devices (T-Deck, Pager, Cardputer):** polls `get_keypress()` directly — no on-screen keyboard needed.
- Foundation for Notes, Contacts, and Calendar apps (all blocked on this component previously).

#### Clock / Timer / Stopwatch (`pm_clock.h` / `pm_clock.cpp`)
Three-tab app for C28P and Maxine:
- **Clock:** NTP sync over WiFi when available; falls back gracefully when offline (no RTC, drifts on reboot without WiFi).
- **Timer:** countdown with +/- adjusters, alarm on expiry.
- **Stopwatch:** start/stop/lap, up to 6 laps displayed, purely `millis()`-based.

#### BLE tracker scanner (`pm_tracker_scan.h` / `pm_tracker_scan.cpp`)
Passive BLE sweep detecting known tracker manufacturer signatures:
- **Apple FindMy** (manufacturer ID `0x004C`, payload type `0x12`).
- **Tile** (`0x00A8`).
- **Samsung SmartTag** (`0x0075`).
- Uses NimBLE polling pattern (`start(duration, false)` returning `NimBLEScanResults`) — compatible with pinned NimBLE 1.4.1. Does not use callback API.
- Results sorted closest-first by RSSI; RSSI bands displayed with distance estimates.
- 30-second sweep, STOP tap terminates early (up to 1s lag at scan window boundary).
- Wired into C28P TOOLS (now 10 items / 3 pages of 4) and Maxine TOOLS (now 6 items / 2 pages of 4).

### RSS — Fluid Fortune feed + SD headline cache

- **Fluid Fortune feed added** (`https://blog.fluidfortune.com/feed`) as first entry in default seed list for both C28P (`rss.cpp`) and Maxine (`maxine_apps.cpp`).
- **Idempotent seeding:** `seed_defaults_if_empty()` rewritten to check each default individually. Existing devices that already had the 4 old feeds pick up the new Fluid Fortune entry on next boot without wiping prior feed list.
- **SD headline cache (`pm_rss_cache.h` / `pm_rss_cache.cpp`):** one JSON file per feed at `/rss_cache/<sanitized_name>.json`, ≤6KB. Populated on every successful HTTP fetch; loaded as fallback on fetch failure. Device-aware backend (same macro pattern as `nosql_store`). Feeds show `(cached)` suffix in header when serving cached data. Allows offline reading of previously-fetched headlines.
- Added to C28P and Maxine `build_src_filter` in `platformio.ini`.

---

## Addendum — 2026-06-01 (session: Cardputer wardrive crash diagnosis + fixes)

Captured from Codex session. Cardputer wardrive crashing in high-RF environments (Downtown LA).

### Cardputer wardrive capture script (`tools/cardputer_wardrive_capture.sh`)

Hands-off crash capture tool for Mac:
- Wraps `pio device monitor` with `caffeinate` (runs with lid closed).
- Logs to `captures/cardputer-wardrive/latest/` — `serial_raw.log`, `events.log`, `summary.log`.
- Auto-reconnects across device resets.
- ESP32 exception decoder integration for file:line stack traces.
- `--upload` flag flashes before capture; `--list-ports` enumerates serial devices; `--tag` labels capture session.
- `bash -n` clean, smoke-tested.

### Cardputer — SPI bus Treaty fix (`src/cardputer_i2c_module.cpp`)

P4 bridge (line ~784) was calling `cardputerSdSPI.begin()` and writing CS pins **before** taking `spi_mutex`. This could race wardrive SD appends on the shared HSPI bus. Fixed: `spi_mutex` acquired before any shared SPI setup in the P4 bridge.

### Cardputer — SD SPI clock reduced (`src/ghost_partition.cpp`)

Cardputer SD SPI clock reduced from 10 MHz → 4 MHz (line 60). In high-RF environments, contention on the shared SPI bus at 10 MHz was causing `SdFat::waitReady()` to stall, which triggered the Task WDT against `WarDriveCore` on CPU0. 4 MHz is stable under load.

### Wardrive — BLE flush batching (`src/wardrive.cpp`)

Previous BLE flush: opened the CSV once **per BLE hit**, wrote one row, closed. In high-RF environments with 30+ BLE devices per window, this produced dozens of open/close cycles per scan, pushing `SdFat` into extended `waitReady()` calls.

Fixed: flush now opens the CSV **once per window**, writes up to 8 BLE rows in a batch, closes once, then yields. Reduces SD open/close rate by ~8–10×. Root cause confirmed via serial capture (TASK_WDT stack decoded to `SharedSpiCard::waitReady() → writeSector() → FsCache::sync() → FsVolume::open() → wardrive_task()`).

### Cardputer wardrive diagnostic improvements (`src/main.cpp`, `src/wardrive.cpp`)

- **Boot reset reason logged:** `[SYSTEM] Reset reason: POWERON (1)` etc. on every boot.
- **`-g3` debug symbols** added to `cardputer_adv` build for accurate exception decoding.
- **Wardrive session diagnostics:** every BLE window logged; WiFi scan lines now include `session`, `sd_ready`, `sd_busy` fields.
- **Throttled SD diagnostics:** session creation, append open failures, GPS-gated skips, SPI mutex timeouts all logged with rate limiting.
- **`/diag_NNNN.txt` on SD:** written on abnormal conditions for post-mortem recovery when USB is not connected.

---

## Known limitations (carried into v1.3)

### Hardware verification pending
- **Heltec V4** — compiles, hardware bring-up not yet attempted.
- **C28P backlight slider** — wired (LEDC PWM on GPIO 45) but `ledcAttach()` is currently called lazily on first slider touch. If flicker occurs on first touch, move `ledcAttach()` call to `c28p_setup()` at boot. *(Upgraded from "pin not identified" — pin is GPIO 45, wiring is functional.)*
- **Anomaly baseline persistence** — routes through NoSQL; with the SD fix in place, `[C28P-Anom] Baseline loaded: 0 entries` should reflect actual counts on second boot. Needs hardware verification.

### Voice terminal — feature gaps
- **T-LoRa Pager microphone capture** deferred to v1.3.
- **Cardputer ADV microphone capture** deferred indefinitely.
- **C28P Voice Terminal** — ADC register sequence now verified against esp-bsp; hardware test pending.

### Software gaps
- **Notes app** — `pm_text_input` component is ready; Notes UI not yet built. *(→ built in v1.3 cleanup; see Notes/Contacts/Calendar shipped.)*
- **Contacts app** — same; NoSQL backend ready, UI not built. *(→ built in v1.3 cleanup.)*
- **Calendar app** — wired for T-Deck/Pager/Cardputer; no C28P/Maxine touch layout. *(→ touch layout built in v1.3 cleanup.)*
- **Calculator** — wired for T-Deck/Pager/Cardputer; no C28P/Maxine layout.
- **Clock** (existing `clock.cpp`) — wired for T-Deck; no C28P/Maxine layout. *(Separate from new `pm_clock.cpp` above.)*
- **Cross-device launcher integration for Weather/RSS/Voice** — T-Deck Plus, Cardputer ADV, T-LoRa Pager launchers need tile additions.
- **Maxine** — Chess dpad removed, full-screen layout corrected. No other regressions known.
- **Cardputer warmup** — first wardrive session after flash may still exhibit 0 BLE display briefly before `bt_display_cached` populates.

### Architectural debt
- **C28P audio HAL:** `i2s_initialized` flag in `c28p_audio.cpp` cannot be reset by the mic probe; requires reboot to re-enable game tones after a probe run. Proper `idle/playback/record` state machine queued for v1.3. *(→ resolved in v1.3 cleanup; see addendum 2026-06-02 below.)*
- **`PIN_I2S_*` macro naming inconsistency** — three devices use `PIN_I2S_*` build flags; T-Deck Plus uses private constants inside `audio_player.cpp`. Standardize in v1.3. *(→ resolved in v1.3 cleanup; see addendum 2026-06-02 below.)*
- **NoSQL / SD_MMC dual-backend** — currently per-file (`nosql_store.cpp`, `ereader.cpp`, `c28p_media.cpp`, `c28p_audio_app.cpp`). A `pm_storage.h` HAL with a `SdFatFs : public fs::FS` adapter would eliminate per-file boilerplate. Queued for v1.3. *(→ resolved in v1.3 cleanup; see addendum 2026-06-02 below.)*
- **RSS `seed_defaults_if_empty` ordering** — Fluid Fortune appends at position 5 on existing devices (only first-boot devices see it at position 1). Feed management UI will allow reordering.

---

## Strategic monitoring

- **KodeDot community** (ESP32-S3 handheld, Kickstarter, August 2026 ship date) — planned outreach to founder Pablo Sax. Pisces Moon's SPI Bus Treaty and ELF loader are directly applicable to KodeDot's dual-chip architecture challenges. Kode OS is an app loader framework shipping without applications; Pisces Moon ships with a complete suite — key differentiator.
- **ELF loader / app store model** — architecture validated conceptually. ELF loader on ESP32 loads a relocatable binary at runtime, resolves symbols against a Pisces Moon HAL host table. Enables third-party apps compiled to the Pisces Moon HAL to run on any supported device without reflashing. P4/C6 multi-chip architecture provides natural partitioning: C6 could host ELF-loaded radio firmware (e.g., Meshtastic radio stack) while P4 HP cores handle application work. ISA mismatch (Xtensa vs RISC-V) means pre-compiled foreign binaries can't execute directly; apps must be compiled against Pisces Moon HAL headers for the target ISA.
- **Dual-band wardrive (C5 + C6):** ESP32-C5 (5GHz) + C6 (2.4GHz) simultaneous capture eliminates channel-hop blind spots. Each radio independently covers its band; P4 Ghost Engine correlates streams by BSSID/MAC. Cross-band timing, band-steering event detection, and dual-band device fingerprinting become possible — capabilities no single-radio wardrive tool can match.
- **ESP32-C5 board acquired** — 8MB PSRAM, 16MB flash, touch screen, single-core RISC-V at 240MHz. 5GHz WiFi is the differentiating feature. Single-core means dual-core Ghost Engine architecture does not apply directly; single-core cooperative task loop variant planned.

---

## Acknowledgments

- **Codex** caught the `DEVICE_TDECK` / `DEVICE_TDECK_PLUS` guard mismatch; contributed Cardputer wardrive capture script and crash diagnosis.
- **Lewis Brisbois IP infrastructure** backs the AGPL-3.0-or-later licensing strategy.
- **Parallel Claude instances** owned distinct scopes: implementation arm (this branch), JenFS design (separate), Jennifer WebOS backend (separate), architectural review (separate).
- **Local model collaboration** queued as v1.3+ architecture work — three-tier delegation pattern (Claude architecture → local model grunt work → MCP filesystem truth).

---

## Addendum 2026-06-02 — v1.3 architectural cleanup

Three v1.2.1 "architectural debt" items closed out, plus the three pm_text_input-blocked apps shipped. JenFS / JenEricFS is deliberately deferred (the current SdFat + SD_MMC dual-backend works fine in production; pm_storage HAL now hides the seam from callers).

### Touch-kiosk apps shipped (C28P + Maxine)

All three use `pm_text_input` for entry and a parallel `*_tombstones` NoSQL category for delete (workaround for NoSQL's append-only model in v1.2.1 — a real `nosql_remove_entry()` is queued for v1.3 proper).

- **Notes** (`include/pm_notes.h`, `src/pm_notes.cpp`) — LIST/VIEW/EDIT screens, NoSQL "notes" category, derived title from first body line, 1024-byte body cap, paged PREV/NEW/NEXT.
- **Contacts** (`include/pm_contacts.h`, `src/pm_contacts.cpp`) — four sequential `pm_text_input` prompts (name / phone / email / note), pipe-delimited record encoding with sanitization, case-insensitive alphabetical sort, 200-entry session cap.
- **Calendar** (`include/pm_calendar.h`, `src/pm_calendar.cpp`) — month grid with Zeller-formula day-of-week, NTP detection via `time(nullptr) > 1700000000`, 6×7 cells with green indicator dots on days with notes, amber "today" cell, prev/next chevrons in title bar, ADD / EDIT / DELETE per day.

C28P TOOLS sub-launcher: 10 → 13 items. Maxine TOOLS sub-launcher: 6 → 9 items. All three files added to both kiosk `build_src_filter` blocks in `platformio.ini`.

### C28P audio HAL with explicit modes

`include/c28p_audio.h` (new) + `src/c28p_audio.cpp` (rewritten) replace the static `i2s_initialized` flag with an explicit mode state machine:

```
C28P_AUDIO_IDLE              no driver installed, codec quiet, amp off
C28P_AUDIO_PLAYBACK_TONE     I2S TX driver owned by HAL, DAC on, amp on
C28P_AUDIO_PLAYBACK_AUDIO    codec + amp on, caller owns I2S (Audio lib)
C28P_AUDIO_RECORD            I2S RX driver owned by HAL, ADC on, amp off
```

Transitions go through `c28p_audio_enter(mode)`; any non-idle to non-idle path is internally serialized through IDLE so driver lifecycle stays consistent. Codec init is one-shot (cached behind `_codec_initialized`); only the I2S driver and amp gate flap between modes. `c28p_audio_release_driver()` is the explicit handoff for cases where the caller (ESP32-audioI2S Audio library) installed its own driver and is about to uninstall it.

Backwards-compat: `c28p_audio_begin() / c28p_audio_stop() / c28p_audio_tone()` are preserved as thin wrappers routing through the mode machine. `game_audio.cpp` works without changes.

The stale-flag bug that required a reboot between mic-probe runs is fixed. Exit log message updated from `"reboot before using game tones"` to `"safe to use game tones"`.

Refactored call sites:
- `src/c28p_mic_probe.cpp` — ~120 lines of duplicated codec sequence + I2S RX install deleted; now calls `c28p_audio_enter(RECORD)` / `c28p_audio_release()`. Provenance comments preserved (the source-of-truth register values live in `c28p_audio.cpp::es8311_init()`).
- `src/c28p_audio_app.cpp` — MP3 player uses `c28p_audio_enter(PLAYBACK_AUDIO)` on track start and `c28p_audio_release_driver()` on BACK. Kept direct `SD_MMC` for `Audio::connecttoFS` (third-party library requires `fs::FS&`).
- `src/c28p_media.cpp` — voice recorder and library playback both route through the HAL; ~50 lines of duplicated I2S setup deleted. Recording side now uses `c28p_audio_enter(RECORD)`; library playback uses `c28p_audio_enter(PLAYBACK_AUDIO)` + `c28p_audio_release_driver()`.

### pm_storage HAL

`include/pm_storage.h` + `src/pm_storage.cpp` (new) provide a unified SD storage HAL. `pm_storage::open()`, `pm_storage::exists()`, `pm_storage::openDir()`, and the move-only `pm_storage::File` handle hide the SD_MMC (C28P, fs::FS) vs SdFat (everyone else) split that v1.2.x carried as per-file macro adapters.

Design choice: chose a wrapper API over `SdFatFs : public fs::FS` inheritance. The `fs::FS / FSImpl / FileImpl` interfaces are Arduino-ESP32 internals subject to upstream churn; the Pisces Moon SD surface is small enough (~12 unique call sites) that a leaner custom API ships cleaner than a full FSImpl adapter would. Files that explicitly need `fs::FS&` for third-party libraries (`Audio::connecttoFS` in `c28p_audio_app.cpp`) are already device-gated and keep direct `SD_MMC` access — documented inline.

Refactored call sites:
- `src/nosql_store.cpp` — deleted the `NOSQL_FS / NOSQL_FILE / NOSQL_MODE_*` macro block (~30 lines) and the `#ifdef DEVICE_C28P` branch in `nosql_clear_category()`. ArduinoJson 7 reads/writes go through a String intermediary (`pm_storage::File` is intentionally NOT derived from Stream).
- `src/ereader.cpp` — deleted the `ErFile / ER_OPEN_READ / er_open_next / er_get_name` adapter block; `using ErFile = pm_storage::File` keeps the loop bodies' local type name readable. `pm_storage::File::name()` returns the full path on both backends, so the basename-extraction case is now one `strrchr` instead of two backend-specific helpers.
- `src/c28p_wardrive_engine.cpp` CSV writer — deleted the `WD_FS / WD_FILE / WD_OPEN_WRITE` macro block; the CSV file handle is now a `pm_storage::File` static. `wd_csv_open / write_row / close` all call through the HAL.

`pm_storage.cpp` added to `build_src_filter` for `[env:c28p]` and `[env:maxine]`. SPI devices (T-Deck Plus, T-LoRa Pager, Cardputer ADV) pick it up via the default `+<*>` filter.

### PIN_I2S_* macro standardization

T-Deck Plus is the only device with TWO I2S buses (MAX98357A amp on `I2S_NUM_0`, ES7210 mic codec on `I2S_NUM_1`). v1.2.x left the pin values hardcoded as private `#define I2S_BCLK=7 / I2S_LRC=5 / I2S_DOUT=6` inside `audio_player.cpp` and `ES7210_MCLK=48 / LRCK=21 / SCK=47 / DIN=14` inside `audio_recorder.cpp` — three naming conventions across one device.

v1.3 standardizes on:
- `PIN_I2S_OUT_SCLK / _LRCK / _DOUT` for the output bus
- `PIN_I2S_IN_MCLK / _SCLK / _LRCK / _DIN` for the input bus

All values now live in `[env:tdeck_plus]` build_flags in `platformio.ini` with inline documentation of the dual-bus topology. `audio_player.cpp` and `audio_recorder.cpp` keep their historical local `#define`s (`I2S_BCLK`, `ES7210_MCLK`, etc.) but they're now aliases over the build-flag macros — the call sites read with the familiar BCLK/LRC/MCLK spellings the chip docs use, while pin values are centrally managed.

`include/hal_pins.h` T-Deck Plus block replaced its stale `I2S_*` (no PIN_ prefix) entries — which had pin numbers that didn't match what the driver files actually used — with `I2S_OUT_*` and `I2S_IN_*` aliases over the new build flags. A cross-device convention note at the bottom of `hal_pins.h` documents the single-bus (`PIN_I2S_*`) vs dual-bus (`PIN_I2S_OUT_*` / `PIN_I2S_IN_*`) split for any future device added to the family.

Single-bus devices (C28P, T-LoRa Pager, Cardputer ADV, Maxine) are unchanged — their existing `PIN_I2S_*` convention already matched the standard.

### Carried forward to v1.3 proper

- **JenFS / JenEricFS** — design tutorial exists (1144 lines) but implementation deferred per user direction. The existing SdFat + SD_MMC dual-backend works fine in production and `pm_storage` now hides the seam from callers, so the urgency dropped.
- **`nosql_remove_entry()`** — the Notes/Contacts/Calendar delete model uses a parallel `*_tombstones` category as a workaround for NoSQL's append-only API. A real per-entry remove would retire the tombstone pattern entirely.
- **Factory reset wipe list** — the new `notes_tombstones / contacts_tombstones / calendar_tombstones` categories should be added to `nosql_clear_category()`'s wipe list so leftover tombstones don't survive a reset and confuse fresh entries that land on the same abs_idx.
- **Heltec V4** — still pending hardware bring-up.
- **ESP32-C5 board** — acquired, single-core RISC-V variant planning queued. *(→ brought up in the 2026-06-09 addendum below; this item is now closed.)*

---

## Addendum — 2026-06-09 (session: NM-CYD-C5 bring-up + fleet-wide WiFi UX)

Full bring-up of the NM-CYD-C5 (RockBase "Colorful") as a fleet-class kiosk target, plus a cross-device WiFi UX overhaul that closes two long-standing gaps in the OS: touch kiosks couldn't enter WiFi credentials without a phone, and keyboard devices couldn't get past captive portals because they have no browser. Both problems are now solved in v1.2.1.

The C5 is the first RISC-V board in the fleet (single-core 240 MHz, contrasted with the dual-core Xtensa S3 family on T-Deck Plus / T-LoRa Pager / Cardputer ADV / C28P / Maxine) and the first board with dual-band Wi-Fi 6 — the rest of the fleet's radios are 2.4 GHz only. That 5 GHz capability is exposed as a CYBER → 5G SCAN sub-tile, giving the operator visibility into the AP population every other Pisces Moon device is blind to.

### New device target — NM-CYD-C5 (`[env:c5]`)

- **SoC:** ESP32-C5-WROOM-1 (single-core RISC-V HP core, 240 MHz), 16 MB flash, 8 MB PSRAM.
- **Radio:** Wi-Fi 6 dual-band (2.4 + 5 GHz), Bluetooth LE 5.3, IEEE 802.15.4.
- **Display:** 2.8" ST7789 240×320 portrait IPS panel, shared SPI bus.
- **Touch:** XPT2046 resistive controller on SPI (CS=1, IRQ pulled up).
- **Storage:** MicroSD on shared SPI (CS=10) via SdFat.
- **Form factor:** desk-fixture / kiosk, identical shell silhouette to the C28P. Hosts the C5 chip set including a CN1 "extend" connector for I2C peripherals on GPIO 9/8.
- **Single-core implication:** the Ghost Engine's dual-core wardrive architecture (radio task on CPU0, UI on CPU1) does not apply directly. The C5 wardrive engine (`c5_wardrive_engine.cpp`) is a cooperative single-task loop that yields between scan windows.
- **PSRAM build flag handling:** RISC-V toolchain rejects `-mfix-esp32-psram-cache-issue` (Xtensa erratum, doesn't apply on RISC-V). New `build_flags_psram_riscv` block in `[common]` provides the same `BOARD_HAS_PSRAM` / `CONFIG_SPIRAM_USE_MALLOC=1` flags minus the erratum flag. Documented inline in `platformio.ini`.

### Touch driver (`src/c5_boot.cpp`)

XPT2046 resistive touch via shared SPI. Every read takes the SPI Treaty mutex, defends against stale LCD / SD CS lines, and validates with a two-read stability gate (±20 ADC counts on each axis) before returning.

Calibration is **edge-based, not range-based.** Earlier prototype used `RAW_X_MIN / RAW_X_MAX` plus a separate `ROTATE_TO_PORTRAIT` flag — a representation that couldn't cleanly express the NM-CYD-C5's inverted X axis (raw ADC counts DOWN as you move right, not up). The new representation stores four constants — `RAW_X_AT_LEFT / _AT_RIGHT / _AT_TOP / _AT_BOTTOM` — the raw ADC value at each physical screen edge. The mapping in `c5_touch_read()` is then a single signed linear interpolation per axis: if `RAW_X_AT_LEFT > RAW_X_AT_RIGHT` (as it is on this panel), the denominator is negative and the integer math gives the right answer without a separate rotation flag.

Hardcoded defaults are from corner-tap calibration on Eric's panel (TL: x=3533 y=339 / TR: x=496 y=351 / BL: x=3592 y=3607 / BR: x=515 y=3676 averaged per edge):

```
RAW_X_AT_LEFT   = 3562    // raw_x at pixel_x = 0
RAW_X_AT_RIGHT  =  505    // raw_x at pixel_x = SCREEN_W-1
RAW_Y_AT_TOP    =  345    // raw_y at pixel_y = 0
RAW_Y_AT_BOTTOM = 3641    // raw_y at pixel_y = SCREEN_H-1
```

Other C5 units may need their own corner-tap calibration via SYSTEM → CALIBRATE — these values are a reasonable starting point but every resistive panel is slightly different.

### Launcher (`src/c5_boot.cpp`)

12-tile 3×4 portrait grid, fleet-parity with the C28P + Maxine layouts:

```
Row 0: SYSTEM   | CYBER     | WEATHER
Row 1: UTILITIES| PERSONAL  | REFERENCE
Row 2: GPS      | RSS       | CLASSICS
Row 3: ARCADE   | PUZZLES   | TETRIS   (HERO)
```

C5-specific substitutions from the C28P template: slot 6 is GPS (C28P has MEDIA — no audio hardware on the C5), slot 7 is RSS (C28P has WEATHER, which moved to slot 2). 5G SCAN — the C5's signature feature — is a sub-item under CYBER alongside WARDRIVE since both are WiFi-scanning workflows.

### 5G SCAN (`src/c5_boot.cpp::c5_run_5g_scan`)

File-static helper under CYBER. Saves current WiFi mode + band mode on entry, sets `WIFI_BAND_MODE_5G_ONLY` via `esp_wifi_set_band_mode`, runs a blocking `WiFi.scanNetworks(false, true)` (typically 2-3 s), renders results sorted by RSSI desc with the same color-coded RSSI bands as wardrive (>-55 green, >-70 yellow, >-82 amber, else red). REFRESH re-scans; EXIT restores the prior mode and band setting before returning to the launcher.

This is intentionally a simpler UI than the full async wardrive engine — it answers the single question *"what 5 GHz networks are around me right now"*. The continuous-log + PMU1-fan-out flow stays in `c5_wardrive_engine.cpp`.

### All 16 games — C5 touch-dpad ports

Every game in the fleet now has a C5 branch alongside its C28P branch. Pattern is the same across all 16: extend the existing `#elif defined(DEVICE_C28P)` viewport block to `|| defined(DEVICE_C5)` (same 240×320 panel + 240×200 game viewport + 240×120 dpad strip), include `c5_dpad.h` alongside `c28p_dpad.h`, swap `c28p_dpad_render()` for `c5_dpad_render()` under `#ifdef DEVICE_C5`, and where the game needs direct touch (paddle drag, swipe, long-press, etc.) dispatch the touch reader via `#ifdef`:

- Touch-needing games (paddle/swipe/tap): breakout, 2048, minesweeper, connect4, simon, solitaire, chess.
- Dpad-only games: snake, asteroids, space invaders, frogger, pacman, galaga, pole position, mario bros, donkey kong.

**Chess** got a small helper refactor (`kiosk_touch()` dispatching C28P ↔ C5) to avoid ~120 lines of `#ifdef` duplication across the chess UI's many tap-handlers.

**Mario Bros + Donkey Kong** use SD_MMC on C28P (its SD wire is on the dedicated SDIO peripheral) but the rest of the fleet — including C5 — uses SdFat on shared SPI. Both files keep the existing `#ifdef DEVICE_C28P → SD_MMC` / `#else → SdFat` split. The C5 falls into the `#else` branch automatically because its SD is on the same SPI bus as the LCD and touch controller.

### Touch keyboard WiFi setup — cross-kiosk (`src/kiosk_wifi_setup.cpp`)

Started as a C5-specific `c5_wifi_setup.cpp` (the SYSTEM → WIFI stub needed to do something useful), then promoted to a fleet-wide kiosk helper covering C5, C28P, and Maxine. The original `c5_wifi_setup.cpp` is archived at `docs/archive/c5_wifi_setup.cpp.v1.2.1-pre-generalize` for posterity.

Device differences are absorbed by two dispatch blocks at the top of the file:

1. **Touch reader dispatch.** `kiosk_touch_read()` is a static inline that resolves to `c5_touch_read` / `c28p_touch_read` / `maxine_touch_read` per `#ifdef`. All three drivers return portrait pixel coordinates via the same `(int16_t*, int16_t*) → bool` signature, so the inline is trivially uniform.
2. **`K_SCALE` multiplier.** Set to `1` on C5 and C28P (both 240×320 panels), `2` on Maxine (480×800). Every geometry constant and every `gfx->setTextSize()` call multiplies by `K_SCALE`, so Maxine renders the same 5-row keyboard at doubled key size + doubled text size + doubled gaps. No separate layout pass for Maxine.

**Two screens, same on every device:**

*Screen 1 — Scan list.* Active-connection banner at the top. Dual-band scan on C5 (2.4 + 5 GHz in one list), single-band 2.4 GHz on C28P + Maxine. Results sorted by RSSI desc, asterisk (`*`) prefix on SSIDs the `/wifi.cfg` keyring already knows the password for. SCAN AGAIN refreshes; DISCONNECT / FORGET appear when associated. Open networks tapped from the list skip the keyboard and connect directly. Known-password networks pre-fill the password entry so reconnect is one tap of DONE.

*Screen 2 — Password entry with on-screen keyboard.* Five rows: numbers (10×24), top letters (10×24), middle letters (9×24 indented), SHIFT + 7 mid keys + BKSP (40+7×22+46), and SYM-toggle + `.` + SPACE + `@` + DONE (40+24+104+24+48). SHIFT is one-shot (auto-releases after one character). SYM swaps the alphabet rows for symbol pages (`! @ # $ % ^ & * ( )` / `- _ = + ; : ' " ?` / `\ | / { } [ ]`); the numbers row stays put across both pages. Password field defaults to masked dots with a SHOW/HIDE toggle on the right side of the input box — changes color when toggled so shoulder-surfers can't read what was typed if the user briefly peeked.

DONE calls `WiFi.begin()` and polls `WiFi.status()` for up to 12 seconds with a dot-spinner animation. On success, `save_wifi_config()` persists the credential via the device-agnostic `wifi_manager` API and the flow returns to the scan list with the new network active. On failure, the user is bounced back into the password screen with their typed password preserved — a single-character typo is one tap away from fixed, not a full retype.

**Wiring:**
- C5 SYSTEM submenu's `WIFI` item now calls `kiosk_run_wifi_setup()` directly. A backwards-compat alias `void c5_run_wifi_setup() { kiosk_run_wifi_setup(); }` preserves the existing forward decl in `c5_boot.cpp` so v1.3 can drop the alias without urgency.
- C28P `c28p_run_wifi_setup()` (in `c28p_apps.cpp`) gained a MANUAL button (cyan) between CONNECT and PORTAL, re-spacing the four buttons to 32px height with 6px gaps. MANUAL routes to `kiosk_run_wifi_setup()`.
- Maxine `maxine_run_wifi_setup()` (in `maxine_apps.cpp`) gained a 4th MANUAL button alongside CONNECT / PORTAL / FORGET, re-spaced to 60px height with 10px gaps. MANUAL routes through the same shared file with `K_SCALE=2` selected automatically.

All three kiosks now have first-class home-network setup that doesn't require a phone. The existing PORTAL (WiFiManager phone-driven AP config) is preserved on C28P + Maxine as an alternative for users who prefer it.

### QR phone bridge for captive portals (`src/wifi_share_qr.cpp`)

New app for the three keyboard-equipped devices (T-Deck Plus, T-LoRa Pager, Cardputer ADV). Solves the *Starbucks WiFi* problem: the device can JOIN an open SSID just fine from its physical keyboard via `wifi_connect.cpp`, but it can't authenticate a captive-portal browser-bridge because it has no browser.

**The fix:** generate a STANDARD WiFi-config QR code from the live connection (falling back to the keyring), display it on-screen, let the user scan it on their phone. The phone joins the same network independently, handles the captive portal in its own browser, and from there the user can either just use the phone for whatever they actually needed the internet for, or turn on the phone's hotspot and re-point the device at the phone's tethered SSID (no captive portal in the way of that).

**QR payload format** (industry-standard, recognized by iOS Camera and Android Settings):

```
WIFI:S:<ssid>;T:<WPA|WEP|nopass>;P:<password>;;
```

`T:WPA` covers WPA / WPA2 / WPA3 — phones figure out which actual protocol to use during the join. `T:nopass` marks open networks.

**Library:** `ricmoo/QRCode @ ^0.0.1` (header-only C library, ~106 bytes per v3 QR). Added to `lib_deps` for `[env:tdeck_plus]`, `[env:tlorapager]`, and `[env:cardputer_adv]`. Touch-kiosk builds don't compile `wifi_share_qr.cpp` (it's outside their `build_src_filter` and gated by `#if defined(DEVICE_TDECK_PLUS) || defined(DEVICE_TLORAPAGER) || defined(DEVICE_CARDPUTER_ADV)` anyway), so the QRCode library is never linked on C5/C28P/Maxine.

**Version 4 QR (33×33 modules)** at ECC_LOW gives ~114-char alphanumeric capacity — comfortable headroom for a 32-char SSID + 63-char WPA password + format overhead (~110 chars). Per-device module size: 5 px on T-Deck Plus (320×240) and T-LoRa Pager (480×222), 3 px on Cardputer ADV (240×135 — tight but still scannable from a phone held ~6 inches away). 4-module quiet-zone padding on a white background per QR spec.

**Discovery:** the WIFI SCANNER's header bar now reads `WIFI SCANNER | R:RESCAN S:QR | <exit-key>`. Press `S` (or `s`) from the scanner to invoke `run_wifi_share_qr()`. Touch-kiosks haven't gotten the QR bridge yet — deferred until someone actually hits a captive portal on a C5 / C28P / Maxine in the wild.

### `platformio.ini` summary of touched envs

- **New** `[env:c5]` with full src_filter (12-tile launcher app list), `build_flags_psram_riscv`, custom platform URL (`pioarduino/platform-espressif32 v55.03.36`) for ESP32-C5 support, and `+<kiosk_wifi_setup.cpp>` in the filter.
- `[env:c28p]` gained `+<kiosk_wifi_setup.cpp>`.
- `[env:maxine]` gained `+<kiosk_wifi_setup.cpp>`.
- `[env:tdeck_plus]` lib_deps gained `ricmoo/QRCode @ ^0.0.1`.
- `[env:tlorapager]` now carries its own `lib_deps` block (`${common.lib_deps}` + `ricmoo/QRCode @ ^0.0.1`) where previously it inherited common implicitly via `extends = common`. The local block continues to chain through `${common.lib_deps}` so nothing is dropped.
- `[env:cardputer_adv]` lib_deps gained `ricmoo/QRCode @ ^0.0.1`.

### Carried forward to v1.2.2 (or v1.3 proper)

- **C5↔Elecrow PMU1 physical cable** — waiting on P5 connector identification (JST-GH 1.25mm vs JST-PH 2.0mm). Wiring map known (C5 GPIO5→Elecrow white/RX, C5 GPIO4←Elecrow yellow/TX, GND straight, never bridge VCC).
- **`c5_data_reader.cpp`** to unstub SURVIVAL / MEDICAL / HISTORY under REFERENCE. `data_reader.cpp` uses `get_touch / get_keypress / update_trackball` unconditionally; needs a touch-only port.
- **WEATHER location-picker UI** — currently hardcoded Pasadena, CA in `weather.cpp::DEFAULT_LAT/LON`. The on-screen keyboard component shipped in this addendum makes a proper picker straightforward in v1.2.2.
- **Calibration persistence to NVS** on C5 — currently the user must paste serial `[CAL]` output back into `c5_boot.cpp` source. A Settings tile that writes the four edge constants to NVS would mean no source edits on inter-board swaps.
- **Touch-kiosk QR bridge** — the captive-portal QR feature is keyboard-device-only in v1.2.1. Adding it to C5 / C28P / Maxine is a small new file + a menu item once a user hits a real captive portal on a touch kiosk.
- **v1.3 cleanup:** drop the `c5_run_wifi_setup` backward-compat alias in `kiosk_wifi_setup.cpp` once the C5 launcher calls `kiosk_run_wifi_setup` directly.

### Fleet link + parity audit (same session)

A static cross-build audit (every launcher-referenced symbol mapped to its defining file, checked against each env's `build_src_filter`) closed out three gaps:

- **Maxine missing `ereader.cpp`** — the E-READER entry was pre-staged in `maxine_open_tools` on 2026-06-01 but the src_filter entry never landed, producing the `undefined reference to run_ereader()` link error on the next fresh Maxine build. Fixed: `+<ereader.cpp>` added to the Maxine filter.
- **`c5_run_5g_scan` extern/static mismatch** in `c5_boot.cpp` — forward-declared `extern`, defined `static` in the same TU. Compile error on C5. Fixed by removing the bogus forward decl (the static definition precedes all its callers).
- **Maxine missing DONKEY KONG** — `donkey_kong.cpp` has carried full Maxine support (480×520 viewport, `maxine_dpad_render`, Maxine game-over paths) since the 2026-06-01 arcade rewrite, but was never added to the Maxine filter or GAMES menu. Fixed: filter entry + menu row + header include. Maxine GAMES is now 17 titles, matching C28P/C5 coverage.

Parity additions for the C5 (both reuse existing portrait code, same pattern as weather.cpp):

- **RSS is now real on C5.** `rss.cpp`'s portrait branch extended from `C28P || HELTEC_V4` to include `DEVICE_C5`, with a `p_touch()` inline dispatching `c5_touch_read` vs `c28p_touch_read` (mirroring weather.cpp's approach). `+<rss.cpp>` added to the C5 filter (`pm_rss_cache.cpp` was already there, so offline headline caching works day one). Launcher slot 7 now calls `run_rss()`; the RSS stub is deleted.
- **ABOUT is now real on C5.** Compact 240×320 device-info screen (`c5_run_about` in c5_boot.cpp) showing version, hardware summary, the 5 GHz fleet-first line, and authorship — parity with the C28P/Maxine ABOUT screens. Stub deleted.

Remaining intentional per-device exclusions, confirmed as agreed (not gaps):

| App | Excluded on | Reason |
|---|---|---|
| MEDIA (recorder + library) | Maxine, C5 | No microphone (Maxine), no audio path at all (C5: PM_NO_AUDIO_IN/OUT) |
| AUDIO player | Maxine, C5 | Maxine is tone-out only; C5 has no audio hardware |
| AI voice terminal | Maxine, C5 | Requires mic + speaker + Gemini client wiring |
| MIC TEST | Maxine, C5 | No microphone |
| CALIBRATE | C28P, Maxine | Capacitive touch — no calibration needed (resistive-only tool) |
| 5G SCAN | everything except C5 | 2.4 GHz-only radios |
| GPS tile | — | C5-only tile, stubbed pending PMU1/P4 link hardware (weekend item) |
| WARDRIVE on C5 | — | Stub pending single-core engine UI; `c5_wardrive_engine.cpp` scaffold exists |
| SURVIVAL/MEDICAL/HISTORY on C5 | — | Pending `c5_data_reader.cpp` touch port (documented in platformio.ini DEFERRED block) |

Keyboard devices (T-Deck Plus, T-LoRa Pager, Cardputer ADV) compile all of `src/` with no filter, so they are structurally immune to the missing-filter-entry failure mode that bit Maxine.

### Closing v1.2.1

v1.2.1 ships with every touch kiosk (C5, C28P, Maxine) having a real on-screen keyboard for WiFi setup, every keyboard device (T-Deck Plus, T-LoRa Pager, Cardputer ADV) having a captive-portal answer, and the fleet at full parity on the core WiFi setup story across six supported devices. The C5 brings RISC-V and dual-band Wi-Fi 6 into the family as net-new capabilities no other Pisces Moon device has.

This is the natural endpoint for v1.2.1. Feature work for v1.2.2 starts from the carried-forward list above; bug fixes that turn up during field testing of this addendum's changes land as point releases in the 1.2.1.x lane if needed.

---

*For Jennifer.*
*Court Jester of Vibe Code, signing off on v1.2.1.*

🎺
