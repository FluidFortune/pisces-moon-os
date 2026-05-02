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

# Pisces Moon OS — Web Demo

**Live demo of Pisces Moon OS v1.0.0 "The Arsenal"**

🌐 **[piscesdemo.fluidfortune.com](https://piscesdemo.fluidfortune.com)**

---

## What Is This

An interactive browser-based emulator for [Pisces Moon OS](https://github.com/FluidFortune/PiscesMoon) — a general-purpose operating system for the LilyGO T-Deck Plus handheld computer (ESP32-S3).

This demo lets you explore the OS interface, navigation model, and key applications without owning the hardware. The Ghost Engine simulation shows the dual-core architecture in action — a background task running independently while you use other apps, exactly as it works on the real device.

---

## What's Included

- **Full DOS-style boot sequence** — authentic BIOS scroll
- **7 app categories, 47 apps** — the complete Pisces Moon launcher
- **Ghost Engine simulation** — Core 0 background wardrive running while you use Core 1
- **Gemini AI Terminal** — live AI using your own free Google API key
- **Wardrive demo** — simulated WiFi/BLE scanning with GPS drift
- **GPS view** — animated position tracking
- **Clock** — real UTC time with working stopwatch
- **ESP32 Device Bridge** — connect a real ESP32 via USB for live data *(coming May 5, 2026)*

---

## Gemini AI Terminal

The AI Terminal is fully functional with a free Google Gemini API key.

1. Go to **[aistudio.google.com/apikey](https://aistudio.google.com/apikey)**
2. Sign in with any Google account
3. Click "Create API Key" — free, no credit card required
4. Open **INTEL → TERMINAL** in the demo and follow the instructions

Your key is stored in your browser only. It goes directly to Google — never through our servers. This is the same API key the real Pisces Moon device uses.

---

## ESP32 Device Bridge *(Coming May 5, 2026)*

Connect any ESP32-S3 running Pisces Moon OS via USB to replace simulated data with live device data. Requires Chrome or Edge (Web Serial API).

When connected, these apps use real hardware data:
- WarDrive (live WiFi/BLE scans + GPS coordinates)
- GPS (real satellite fix)
- BT Radar, Net Scanner, RF Spectrum, Packet Sniffer, Hash Tool, Beacon Spotter
- AI Terminal (prompts sent to device's Gemini client)

Bridge App firmware: [github.com/FluidFortune/PiscesMoon](https://github.com/FluidFortune/PiscesMoon)

---

## Get the Real Hardware

The full OS runs on the **LilyGO T-Deck Plus** — approximately $50 USD.

- Available on LilyGO's AliExpress store or [lilygo.cc](https://lilygo.cc)
- ESP32-S3 dual-core @ 240MHz · 8MB PSRAM · 16MB Flash
- Physical QWERTY keyboard · Touchscreen · Trackball
- WiFi · BLE 5.0 · LoRa SX1262 · GPS · I2S Audio · MicroSD

Flash Pisces Moon OS: [github.com/FluidFortune/PiscesMoon](https://github.com/FluidFortune/PiscesMoon)

---

## Repository Structure

```
pisces-moon-demo/
├── index.html          # Complete self-contained demo (single file)
├── CNAME               # piscesdemo.fluidfortune.com
├── README.md           # This file
└── CLA.md              # Contributor License Agreement
```

---

## License

```
Pisces Moon OS Web Demo
Copyright (C) 2026 Eric Becker / Fluid Fortune
SPDX-License-Identifier: AGPL-3.0-or-later
```

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
any later version.

Full license text: [gnu.org/licenses/agpl-3.0](https://www.gnu.org/licenses/agpl-3.0)

---

*Pisces Moon OS · [fluidfortune.com](https://fluidfortune.com)*
