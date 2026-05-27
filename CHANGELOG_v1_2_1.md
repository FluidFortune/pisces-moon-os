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

## Known limitations (carried into v1.3)

### Hardware verification pending
- **Heltec V4** — compiles, hardware bring-up not yet attempted.
- **C28P backlight slider** — persists to NoSQL but no GPIO wiring (backlight pin not yet identified in LCDwiki documentation).
- **C28P Tetris audio** — Cardputer Tetris audio works; C28P Tetris is silent. Likely candidates: ES8311 amplifier enable pin / active level, or `c28p_audio_begin/stop` re-init handoff after Audio Player has run. Diagnostic procedure: fresh boot, go directly to Tetris before opening Audio Player.

### Voice terminal — feature gaps
- **T-LoRa Pager microphone capture** deferred to v1.3 pending schematic-verified ES8311 ADC bring-up.
- **Cardputer ADV microphone capture** deferred indefinitely — M5 library handles audio internally and does not expose raw I2S mic access; would require M5Stack source-code archaeology.
- **ES8311 ADC register sequence on C28P** uses best-read-of-datasheet values; first hardware test may require register tuning if mic captures silence.

### Software gaps
- **Voice recorder records silent WAV files** on all devices (UI exists, I2S microphone capture is v1.3 work).
- **Factory reset gated as "not implemented"** — requires `nosql_clear_category()` helper to be added to `nosql_store.cpp/h`.
- **Cross-device launcher integration for Weather/RSS/Voice** — all apps compile and run on all targeted devices, but only the C28P launcher dispatches to them. T-Deck Plus, Cardputer ADV, and T-LoRa Pager launchers need tile additions in their respective launcher.cpp / launcher_cardputer.cpp files.

### Architectural debt
- **`PIN_I2S_*` macro naming inconsistency.** Three devices (C28P, T-LoRa Pager, Cardputer ADV) use `PIN_I2S_*` build flags. T-Deck Plus uses private `I2S_BCLK/LRC/DOUT` constants inside `audio_player.cpp`. Worth standardizing in v1.3 cleanup.

---

## Strategic monitoring

- **KodeDot community** (ESP32-S3 handheld, Kickstarter, August 2026 ship date) — planned outreach to founder Pablo Sax. Pisces Moon's SPI Bus Treaty and ELF loader are directly applicable to KodeDot's dual-chip architecture challenges. Kode OS is an app loader framework shipping without applications; Pisces Moon ships with a complete suite — key differentiator.

---

## Acknowledgments

- **Codex** caught the `DEVICE_TDECK` / `DEVICE_TDECK_PLUS` guard mismatch.
- **Lewis Brisbois IP infrastructure** backs the AGPL-3.0-or-later licensing strategy.
- **Parallel Claude instances** owned distinct scopes: implementation arm (this branch), JenFS design (separate), Jennifer WebOS backend (separate), architectural review (separate).
- **Local model collaboration** queued as v1.3+ architecture work — three-tier delegation pattern (Claude architecture → local model grunt work → MCP filesystem truth).

---

*For Jennifer.*
*Court Jester of Vibe Code, signing off on v1.2.1.*

🎺
