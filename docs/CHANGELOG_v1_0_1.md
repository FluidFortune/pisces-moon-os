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

## v1.0.1 — May 2026

A maintenance release that closes the gap on real-world testing of v1.0.0 "The Arsenal." The biggest change is bringing the Mesh Messenger into the SPI Bus Treaty — it had been independently accessing the LoRa radio without coordinating with the rest of the system, which broke transmission whenever wardrive was running on Core 0. Several gameplay and UX issues are also fixed.

### Fixed — Mesh Messenger SPI Coordination

The Mesh Messenger was bypassing the SPI Bus Treaty entirely. Every other LoRa-using app on the device (Wardrive, RF Spectrum) takes the `spi_mutex` semaphore before issuing radio commands, because the SX1262 LoRa chip, the SD card, and the display all share the same physical SPI bus. Mesh Messenger was hammering the bus directly.

In quiet conditions this worked. But in any realistic use case — wardrive running on Core 0, display redrawing the cursor blink every 500ms, SD card writes from the Ghost Engine — the LoRa transmission collided with other SPI traffic and the packet went out malformed.

This was the underlying cause of:
- "Failed to send to #general" — TX collided with display or SD writes
- Inability to type after switching to a new channel — radio left in a stuck state from a corrupted `setFrequency()` call
- Intermittent success / failure with no apparent pattern

All radio operations (`begin`, `transmit`, `receive`, `setFrequency`, `startReceive`) now take `spi_mutex` with appropriate timeouts:
- 3000ms for radio init (full chip configuration sequence)
- 2000ms for transmit (TX at SF11 takes ~1 second)
- 500ms for channel switches and frequency tuning
- 20ms non-blocking for receive polling (skip cycle if bus is busy)

The result: **wardrive and mesh messaging now run simultaneously** without interfering with each other. This is the dual-core architecture working as designed — Core 0 logging networks while Core 1 sends mesh messages, both coordinated through the SPI Bus Treaty.

### Fixed — Mesh Messenger Header Tap Zone

The exit zone (top half of the header) was 11 pixels tall, which was nearly impossible to hit reliably given GT911 touchscreen calibration variance. Expanded the total header tap zone to 40px and split it 20/20 between exit and channel-switch with clearer boundaries.

### Fixed — Galaga Frame Stuttering and Freezes

Two related issues with the same root cause: blocking `delay()` calls inside the game loop.

**Stutter on enemy hit:** `drawExplosion()` was called inline during bullet-collision detection. The animation contained 4× `delay(25)` plus `delay(80)` — a total of 180ms blocked per hit. With multiple bullets hitting in rapid succession, the freezes compounded into half-second-plus stalls that looked like the game was crashing.

**Freezes after 10-15 seconds of play:** As the dive AI engaged and more enemies were destroyed, multiple explosion calls stacked up in the same frame. Combined with the 500ms `delay()` after tractor beam capture, this produced the appearance of the game freezing entirely.

Replaced the blocking explosion with a non-blocking explosion queue: up to 6 simultaneous explosions, each with a 5-frame animation that advances one tick per game frame. The visual effect is identical — concentric expanding circles in red→orange→yellow→white→grey — but the game loop never blocks. Removed the `delay(500)` after tractor beam capture for the same reason.

### Fixed — Chess Piece Movement

Chess was unplayable. Two issues:
- The trackball was using `update_trackball()` (250ms lockout) instead of `update_trackball_game()` (80ms lockout), making cursor movement feel sluggish
- The only way to confirm a selection was the trackball click button, which is a small physical button sharing GPIO0 with the ES7210 microphone — frequently unreliable

Selection can now be confirmed by trackball click, gamepad A button, **SPACE**, **ENTER**, or a direct touch on the destination square. Touch on board moves the cursor and confirms in one action. Added ESC for deselect.

The header tap quit zone was `ty < 8`, which was the same y-coordinate as `BOARD_Y` — meaning taps at the top of the board were quitting the game instead of selecting pieces. Fixed to `ty < 40`.

### Fixed — Global Header Tap Tolerance

A pattern emerged across the OS: the GT911 touchscreen's calibration variance was causing taps near the header boundary to land at y=24-26 pixels and miss the `ty < 24` check used by every app's exit handler. Updated 29 files globally to use `ty < 40` for header taps. This affects: BT Radar, Wardrive, GPS, Terminal, Voice Terminal, Mesh Messenger, Hash Tool, Beacon Spotter, Net Scanner, Packet Sniffer, Beacon Spotter, RF Spectrum, Probe Intel, BLE Ducky, USB Ducky, WiFi Ducky, GATT Explorer, WPA Handshake, Offline Packet Analysis, Calculator, Calendar, Clock, Notepad, Etch, Audio Recorder, Audio Player, MicroPython, SSH Client, Trails, Baseball, About, System, File Manager.

### Fixed — Bridge App Compile Errors

The `bridge_app.cpp` file added in v1.0.0 outputs was missing a direct `#include <TinyGPSPlus.h>` and was referencing the (non-existent) `C_DIM` color constant. Both fixed — bridge app now compiles clean alongside the rest of the OS. Bridge App still requires manual wiring into `apps.h` and `launcher.cpp` to be reachable from the OS UI.

### Verified — Audio Recorder Stability

Confirmed that v1.0.0's audio recorder DMA buffer fix (4×512 in PSRAM, was 8×1024 in BSS) is working correctly. Earlier crash reports from "entering a settings app" did not reproduce in this round of testing.

### Architectural Notes

This release reinforces a principle that should be documented for any future contributor: **any code that touches the SPI bus must participate in the SPI Bus Treaty**. The bus is shared between three peripherals on the T-Deck Plus and any app that talks directly to one of them without taking `spi_mutex` first will eventually corrupt traffic to another peripheral.

The Mesh Messenger oversight in v1.0.0 was discovered only because user testing finally exercised the wardrive-plus-mesh use case. This is the kind of bug that doesn't appear in development (where you test apps in isolation) but emerges immediately when real users run the device the way it's actually meant to be used.

For v1.0.x and beyond, every new LoRa, SD, or display-coordinated feature will be reviewed against the SPI Bus Treaty before being merged.

---

## v1.0.0 "The Arsenal" — April 2026

The first public release. See `CHANGELOG_v1_0_0.md` for full details. Highlights:
- Public open-source release under AGPL-3.0
- Migration from OAuth+VM-proxy to direct API key authentication for Gemini AI
- 8 new CYBER tools (BLE GATT Explorer, WPA Handshake, RF Spectrum, Probe Intelligence, Offline Packet Analysis, BLE Ducky, USB Ducky, WiFi Ducky)
- Per-session wardrive logging with `sd_in_use` traffic flag
- Web emulator at piscesdemo.fluidfortune.com
- Bridge App firmware (USB serial JSON protocol for any ESP32)
- Major bug fixes across Mesh Messenger exit, BT Radar exit, Clock NTP sync, Calendar, Galaga HUD stutter, Pac-Man Y-axis, Snake border artifacts, Wardrive BLE counter, WiFi File Manager uploads, Audio Recorder SRAM pressure

---

*Pisces Moon OS — fluidfortune.com*
