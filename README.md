# Pisces Moon OS — v1.1.1 "ELF Treaty"

A general-purpose operating system for the LilyGO T-Deck Plus
(ESP32-S3 handheld computer).

**Repo:** github.com/FluidFortune/PiscesMoon  
**Web:** fluidfortune.com  
**License:** AGPL-3.0-or-later

---

## What's New in v1.1

This release closes a foundational gap in the SPI Bus Treaty and ships
significant stability fixes for the wardriving subsystem.

**Headline changes:**
- **ELF API v1.1** — third-party ELF modules can now safely access the
  SD card without violating the SPI Bus Treaty
- **Random wardrive reboot fixed** — `String` allocation inside mutex
  was the culprit; replaced with stack-allocated `char[]` buffers
- **SPI Bus Treaty audit** — 7 apps had SD operations outside the
  mutex; all fixed (Galaga, Pac-Man, Snake, SimCity, Doom, Notepad,
  Filesystem)
- **Filesystem refactor** — file viewer loads to PSRAM under mutex,
  releases, then paginates without holding the bus
- **Header tap standardization** — all apps now use `ty < 40` (the
  v1.1 universal convention)

See `CHANGELOG_v1_1_0.md` for the full list.

---

## Hardware

- **Target device:** LilyGO T-Deck Plus
- **Chip:** ESP32-S3 (8MB PSRAM, 16MB flash)
- **Display:** 320×240 IPS via ST7789
- **Input:** GT911 capacitive touch + trackball + QWERTY keyboard
- **Radio:** SX1262 LoRa
- **GPS:** L76K via UART
- **Power:** AXP2101 PMU
- **Audio:** I2S DAC

---

## Build & Flash

This is a PlatformIO project. To build:

```bash
# Install PlatformIO if not already
pip install platformio

# Clone and build
git clone https://github.com/FluidFortune/PiscesMoon.git
cd PiscesMoon
pio run -e esp32s3

# Flash to T-Deck (USB-C connected)
pio run -e esp32s3 -t upload
```

For VS Code users: install the PlatformIO extension, open this folder,
click "Build" then "Upload" in the bottom toolbar.

### Required: secrets.h

Some apps (Gemini Terminal, Voice Terminal) need API keys. Copy the
example and fill in your keys:

```bash
cp include/secrets.h.example include/secrets.h
# Edit include/secrets.h to add your Gemini API key
```

`secrets.h` is gitignored — your keys never leave your machine.

If you don't add keys, the affected apps will gracefully show a
"no key configured" screen instead of crashing.

---

## Project Layout

```
PiscesMoon/
├── platformio.ini           ← Build configuration
├── partitions.csv           ← Flash partition table
├── README.md                ← This file
├── CHANGELOG_v1_1_0.md      ← v1.1 changes
├── CLA.md                   ← Contributor License Agreement
│
├── src/                     ← All .cpp implementation files (60 files)
│   ├── main.cpp             ← Setup, loop, dual-core task pinning
│   ├── launcher.cpp         ← Home screen, app dispatch
│   ├── elf_loader.cpp       ← v1.1 ELF runtime + SD helpers
│   ├── wardrive.cpp         ← Active scan with treaty compliance
│   └── ... (all apps)
│
├── include/                 ← All .h declarations (56 files)
│   ├── apps.h               ← Function declarations for every app
│   ├── elf_loader.h         ← ElfContext + ABI v1.1 surface
│   ├── theme.h              ← Color constants
│   ├── secrets.h.example    ← Template — copy to secrets.h
│   └── ... (all module headers)
│
└── docs/
    ├── CHANGELOG_v1_0_0.md
    ├── CHANGELOG_v1_0_1.md
    ├── ELF_v1_1_MIGRATION.md ← How to migrate ELF modules to v1.1
    └── fluid-fortune-bible.md ← Architecture notes
```

---

## SPI Bus Treaty — Quick Reference

The T-Deck Plus shares one SPI bus between SD card, LoRa radio, and
display. Every operation that touches SD or LoRa **must** take
`spi_mutex` first. This is enforced throughout the OS.

For app developers:

```cpp
extern SemaphoreHandle_t spi_mutex;

if (xSemaphoreTake(spi_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    FsFile f = sd.open("/my_data.txt", O_WRITE | O_CREAT);
    if (f) {
        f.print("hello");
        f.close();
    }
    xSemaphoreGive(spi_mutex);
}
```

Helper functions (`nosql_*`, `database_*`) handle this internally.

For ELF modules in v1.1+, use the `ctx->sd_*` helpers exposed via
`ElfContext` — they take the mutex internally so treaty violations
are impossible. See `docs/ELF_v1_1_MIGRATION.md`.

---

## Architecture Highlights

### Dual-Core Task Split
- **Core 0:** Ghost Engine (background wardrive task)
- **Core 1:** UI thread (apps, rendering, input)

### Memory Strategy
- **Heap:** ~280 KB for normal allocation
- **PSRAM:** 8 MB for buffers, scrollback, ELF modules
- All large allocations use `ps_malloc()` to target PSRAM
- No stack allocations >1 KB

### App Categories (7)
- **TOOLS** — Notepad, Calculator, Clock, Calendar, Etch
- **COMMS** — WiFi, GPS, Mesh Messenger, Voice Terminal, LoRa PTT
- **CYBER** — Wardrive, BT Radar, Packet Sniffer, BLE Ducky, etc.
- **GAMES** — Snake, Pac-Man, Galaga, Chess, Doom, SimCity, Retro ELFs
- **INTEL** — Gemini Terminal, Reference DBs, SSH, Trails, Baseball
- **MEDIA** — Audio Player, Audio Recorder
- **SYSTEM** — File Browser, WiFi File Manager, Bridge, About

### File Types (Pisces Moon Ecosystem)
- **`.cpp`** — Your app code (logic, drawing, input handling)
- **`.h`** — Declarations / shared interfaces
- **`.elf`** — Compiled, loadable third-party module (`/sd/apps/`)
- **`.bin`** — Full firmware image (the result of `pio run`)
- **`.json`** — ELF manifest (paired with each `.elf`)

---

## Companion Tools

These run alongside Pisces Moon:

- **Lety** — Browser-based IDE for Pisces Moon apps  
  Live: lety.fluidfortune.com  
  Repo: github.com/FluidFortune/lety

- **SDL2 Emulator** — Run Pisces Moon on Mac/Linux/Windows  
  Repo: github.com/FluidFortune/emulator

- **Bridge App** — USB Serial bridge for the web emulator  
  Built into Pisces Moon (SYSTEM → BRIDGE)

---

## License

GNU Affero General Public License v3.0 or later. See `CLA.md` for
contribution terms.

If you fork this project to run as a network service, AGPL requires
that you publish your modifications under the same license. This is
intentional — the OS exists to be improved by its users, not enclosed.

---

## Credits

- **Author:** Eric Becker / Fluid Fortune (fluidfortune.com)
- **Hardware platform:** LilyGO T-Deck Plus
- **Frameworks:** Arduino-ESP32, FreeRTOS, SdFat, Arduino_GFX,
  TinyGPSPlus, NimBLE, RadioLib, ESP32-audioI2S
- **AI assistance:** Claude (Anthropic) for development collaboration
- **Special thanks:** the ESP32 hobbyist community

---

*Pisces Moon OS · v1.1.1 "ELF Treaty" · May 2026*  
*fluidfortune.com · AGPL-3.0-or-later*
