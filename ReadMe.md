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

# Pisces Moon OS

**A general-purpose operating system for the LilyGO T-Deck Plus**

Version 1.0.0 "The Arsenal" — April 2026
License: AGPL-3.0-or-later
Copyright (C) 2026 Eric Becker / Fluid Fortune — fluidfortune.com

---

> *"The network is a resource. The intelligence runs on your metal."*

---

## What Is This

Pisces Moon OS is the first true general-purpose operating system for the ESP32-S3 class of hardware. It runs on the **LilyGO T-Deck Plus** — a $50 handheld with a physical QWERTY keyboard, touchscreen, trackball, WiFi, Bluetooth LE, LoRa radio, GPS, and I2S audio.

This is not a single-function firmware. It is not a menu system bolted onto one application. It is a real OS: a launcher, a dual-core architecture, a security system, and 47+ applications — all running simultaneously on a $50 device that fits in a jacket pocket.

**This is real. The hardware exists. The software runs on it.**

---

## Hardware Required

| Item | Details |
|------|---------|
| **Device** | LilyGO T-Deck Plus |
| **Purchase** | ~$50 USD on LilyGO's AliExpress store or lilygo.cc |
| **MCU** | ESP32-S3 dual-core @ 240MHz |
| **RAM** | 320KB SRAM + 8MB OPI PSRAM |
| **Flash** | 16MB |
| **Radios** | WiFi, BLE 5.0, LoRa SX1262, GPS |
| **Audio** | ES7210 microphone, I2S speaker |
| **Storage** | MicroSD card (FAT32, any size) |

A MicroSD card is required. The OS stores all user data, wardrive logs, AI conversation history, and payloads on the SD card.

---

## Application Categories

### COMMS
WiFi network join, GPS display, LoRa mesh messenger, Voice Terminal (AI + STT + TTS), LoRa push-to-talk

### CYBER *(14 apps across 3 pages)*
Wardrive (WiFi/BLE mapping), BT Radar, Packet Sniffer, Beacon Spotter, Net Scanner, Hash Tool, BLE GATT Explorer, WPA Handshake Capture, RF Spectrum Visualizer, Probe Request Intelligence, Offline Packet Analysis, BLE Ducky, USB Ducky, WiFi Ducky

> All CYBER tools are for **authorized security research and education only**. Use only on networks and systems you own or have explicit written authorization to test.

### TOOLS
Notepad/Journal, Calculator, Clock, Calendar, Etch (drawing canvas)

### GAMES
Snake, Pac-Man, Galaga, Chess (with AI), SimCity Classic, Doom (framework), Retro ROM launcher (NES/GB/Atari via ELF)

### INTEL
Gemini AI Terminal, AI conversation log browser, SSH client, Sports reference, Trail database, Medical reference

### MEDIA
Audio player (MP3/FLAC/AAC/OGG), Audio recorder (16kHz WAV)

### SYSTEM
File browser, WiFi File Manager (SD card over HTTP), System stats, About, MicroPython shell, ELF app loader, Gamepad setup

---

## Architecture Highlights

### Dual-Core Design
- **Core 1**: UI, all applications, touch/keyboard/trackball
- **Core 0**: Ghost Engine — continuous background wardriving, GPS logging, BLE scanning. Runs silently while you do anything else.

### The SPI Bus Treaty
The SD card and LoRa radio share the SPI bus. Pisces Moon OS solved the resulting hardware contention problem through a formal architectural protocol — the first documented solution for this hardware platform. Every component follows it. Result: stable concurrent operation of WiFi, BLE, LoRa, GPS, SD, and display simultaneously.

### Ghost Partition (Security System)
Two-partition SD card architecture. Student Mode shows a normal consumer device. Tactical Mode (primary PIN + unlock key) mounts the second partition and reveals the full OS. The wardrive data, AI logs, and CYBER payloads are only visible in Tactical Mode. Nuke function wipes index files in milliseconds.

### ELF Application Engine
Third-party applications load at runtime from `/apps/` on the SD card as ELF binaries into PSRAM. No reflashing required to add or update apps. Retro emulators (NES, Game Boy, Atari) load this way.

### Memory Architecture
8MB PSRAM with `CONFIG_SPIRAM_USE_MALLOC=1` — heap allocations automatically route to PSRAM. Large working buffers (RF spectrum waterfall, BLE scan results, analysis tables) are allocated on app launch and freed on exit. Internal SRAM reserved for ISR-critical operations.

---

## Getting Started

### 1. Install PlatformIO
Install [PlatformIO](https://platformio.org/) for VS Code or use the CLI.

### 2. Clone the Repository
```bash
git clone https://github.com/FluidFortune/PiscesMoon.git
cd PiscesMoon
```

### 3. Configure Secrets
```bash
cp include/secrets.h.example include/secrets.h
```
Edit `include/secrets.h` and add your API key:
```c
#define GEMINI_API_KEY  "your-key-here"
```
Get a free key at [aistudio.google.com/apikey](https://aistudio.google.com/apikey). See `docs/GETTING_YOUR_API_KEY.md` for step-by-step instructions.

`secrets.h` is in `.gitignore` and will never be committed.

### 4. Flash
Insert a FAT32-formatted MicroSD card into the T-Deck. Then:
```bash
# Standard build (normal operation)
pio run -e esp32s3 --target upload

# HID build (USB keyboard injection testing)
pio run -e esp32s3_hid --target upload
```

### 5. First Boot
The device boots to a PIN screen. Default PIN: **1234** (change in `include/security_config.h` before flashing for production use).

After authentication, the launcher grid appears. Navigate with the trackball or touchscreen. Tap any category to open it. Tap an app to launch it. Tap the top bar of any screen to exit back to the launcher.

---

## Gemini AI Terminal

The Gemini Terminal requires a free API key from Google AI Studio. No account billing required for the free tier (15 requests/minute, 1M tokens/day).

See **`docs/GETTING_YOUR_API_KEY.md`** for full setup instructions.

Once configured, open **INTEL → GEMINI** on the device. The terminal maintains a rolling 10-exchange conversation history. Type your prompt and press Enter.

The Voice Terminal (**COMMS → VOICE**) uses the same key for AI responses. Voice input/output additionally requires a Google Cloud API key with Speech-to-Text and Text-to-Speech APIs enabled — see the API key guide for details.

---

## WiFi File Manager

The easiest way to transfer files to/from the SD card without removing it.

1. Connect T-Deck to WiFi via **COMMS → WIFI JOIN**
2. Open **SYSTEM → SD FILES**
3. The T-Deck displays its IP address
4. On your computer, open a browser and navigate to `http://[IP-ADDRESS]/`
5. Browse, upload, download, and delete files
6. `/backup.zip` downloads the entire SD card as a ZIP archive
7. `/ping` is a lightweight test — if this works, the server is running

**If the browser can't connect:** type the full `http://` prefix. Many browsers auto-upgrade to HTTPS. If `/ping` also fails, check your router's AP isolation / client isolation setting — this prevents WiFi devices from talking to each other and is on by default on many mesh networks.

---

## Wardrive Data Pipeline

Pisces Moon OS is designed to feed the **Fluid Fortune** intelligence stack:

1. **Collect**: Ghost Engine logs WiFi APs + BLE devices with GPS coordinates to `/wardrive_NNNN.csv` continuously in the background
2. **Export**: Use WiFi File Manager `/backup.zip` or the multi-file `/select` page to pull session files to your computer
3. **Split**: Run `wardrive_splitter.py` to break large sessions into Smelter-ready chunks
4. **Analyze**: Load into [Spadra Smelter](https://github.com/FluidFortune/Smelter) for interactive heatmaps, anomaly detection, and device persistence tracking

Everything local. No cloud. No subscription.

---

## CYBER Tools

All CYBER tools require authorization. They are educational instruments for understanding wireless security. Most are passive — they only observe. The active tools (Ducky Suite) are for authorized penetration testing only.

### Passive Tools
- **Wardrive** — GPS-tagged WiFi + BLE survey
- **BT Radar** — BLE device scanner
- **Packet Sniffer** — 802.11 promiscuous frame capture
- **Beacon Spotter** — Management frame analysis, deauth detection
- **Net Scanner** — TCP host and port discovery
- **BLE GATT Explorer** — BLE service/characteristic enumeration
- **WPA Handshake Capture** — EAPOL 4-way handshake capture (→ .hccapx for Hashcat)
- **RF Spectrum Visualizer** — SX1262 RSSI sweep 150–960MHz, scrolling waterfall
- **Probe Request Intelligence** — Device fingerprinting via probe request analysis
- **Offline Packet Analysis** — Rules-based post-session analysis of saved logs

### Active Tools (Authorized Testing Only)
- **BLE Ducky** — Wireless HID keyboard injection over Bluetooth. No reflash required. Payload files in `/payloads/*.txt` (DuckyScript format)
- **USB Ducky** — Wired USB HID keyboard injection. Requires HID build: `pio run -e esp32s3_hid`
- **WiFi Ducky** — Network payload delivery: HTTP POST/GET to listeners, SSH exec, reverse C2 channel demo

### Payload Format
All three Ducky tools share the same `/payloads/` directory and DuckyScript `.txt` format. A payload written for BLE Ducky works unchanged in USB Ducky.

---

## Building the HID Variant

The standard build uses USB CDC (serial console + USB flash). The HID build converts the USB port to a keyboard for USB Ducky testing:

```bash
pio run -e esp32s3_hid --target upload
```

**HID build tradeoffs:**
- No USB serial console
- No USB-based flashing after this point
- To return to standard build: double-tap RST button → ROM bootloader → `pio run -e esp32s3 --target upload`
- Or: use a USB-serial adapter on GPIO43 (TX) / GPIO44 (RX) at 115200 baud

---

## Project Structure

```
PiscesMoon/
├── src/                    # All application source files
├── include/                # Headers + secrets.h (gitignored)
│   ├── secrets.h.example   # Copy to secrets.h and add your API key
│   ├── security_config.h   # PIN configuration, Ghost Partition paths
│   ├── hal.h               # Hardware pin definitions
│   └── theme.h             # Color constants
├── partitions.csv          # Custom 16MB partition table
├── platformio.ini          # Build config (esp32s3 + esp32s3_hid environments)
├── wardrive_splitter.py    # Wardrive CSV processing tool
├── docs/
│   ├── GETTING_YOUR_API_KEY.md
│   ├── CHANGELOG_v1_0_0.md
│   └── MANUAL_ADDENDUM_v1_0_0.md
└── PISCES_MOON_MANUAL.md   # Full technical and philosophical documentation
```

---

## SD Card Structure

```
/                           # Root (public partition)
├── wardrive_0001.csv       # Wardrive session logs
├── wifi.cfg                # WiFi credentials (auto-created)
├── cyber_logs/             # CYBER tool session outputs
│   ├── beacon_*.json
│   ├── pkt_*.csv
│   ├── handshake_*.hccapx  # WPA handshakes for Hashcat
│   └── analysis_*.txt
├── payloads/               # Ducky Suite scripts
│   ├── wifi_targets.json   # WiFi Ducky target config
│   └── *.txt               # DuckyScript payloads
├── apps/                   # ELF application modules
├── roms/                   # Retro emulator ROMs
│   ├── nes/
│   ├── gb/
│   └── atari/
└── recordings/             # Audio recorder output
```

---

## Dependencies

All managed by PlatformIO. No manual installation required.

| Library | Version | Purpose |
|---------|---------|---------|
| GFX Library for Arduino | 1.4.7 | Display driver |
| XPowersLib | 0.2.1 | AXP2101 PMU |
| TAMC_GT911 | 1.0.2 | Touchscreen |
| SdFat | 2.2.3 | SD card |
| TinyGPSPlus | 1.0.3 | GPS parsing |
| ArduinoJson | 7.4.3 | JSON |
| NimBLE-Arduino | 1.4.1 | Bluetooth LE |
| ESP32-audioI2S | 2.1.0 | Audio playback |
| WiFiManager | 2.0.17 | WiFi provisioning |
| RadioLib | 6.6.0 | SX1262 LoRa |

---

## Contributing

Pisces Moon OS is AGPL-3.0 licensed. Contributions are welcome.

Before your first pull request is merged, you will be asked to sign a Contributor License Agreement (CLA). This protects both you and the project.

**Before contributing:**
1. Fork the repository
2. Create a feature branch
3. Follow the existing code style (see any `.cpp` file for conventions)
4. Respect the SPI Bus Treaty (documented in the manual)
5. Test on real hardware if possible
6. Submit a pull request with a clear description

**Do not submit:**
- Offensive payloads in the `/payloads/` examples
- Tools that transmit unauthorized frames
- Code that circumvents the authorization requirements documented in the CYBER tools

---

## License

```
Pisces Moon OS
Copyright (C) 2026 Eric Becker / Fluid Fortune
SPDX-License-Identifier: AGPL-3.0-or-later

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
any later version.

fluidfortune.com
```

The full license text is in the `LICENSE` file.

**Third-party components with separate licenses:**
- Pac-Man engine — original third-party code, not covered by this license
- SimCity engine — original third-party code, not covered by this license
- Doom engine — original third-party code, not covered by this license
- Galaga engine — original third-party code, not covered by this license

---

## The Fluid Fortune Stack

Pisces Moon OS is one component of the Fluid Fortune intelligence platform:

| Project | Description | License |
|---------|-------------|---------|
| **Pisces Moon OS** | This project | AGPL-3.0 |
| **Spadra Smelter** | Wardrive analysis tool | AGPL-3.0 |
| **The Phantom** | Local AI assistant | AGPL-3.0 |

*The network is a resource. Not a dependency.*

---

*Pisces Moon OS v1.0.0 "The Arsenal" — April 2026*
*For Jennifer Soto. The ocean and the fire both.*
