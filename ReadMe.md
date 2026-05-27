# Pisces Moon OS

**ESP32-S3 general-purpose handheld operating system**

A multi-device OS targeting the LilyGO T-Deck Plus, LilyGO T-LoRa
Pager, and M5Stack Cardputer ADV. Ships ~50 apps across seven
categories (Comms, Cyber, Tools, Games, Intel, Media, System).

**Author:** Eric Becker / Fluid Fortune
**License:** AGPL-3.0-or-later
**Site:** fluidfortune.com

---

## Supported devices

| Device | Display | Input | LoRa | GPS | Battery PMU |
|---|---|---|---|---|---|
| **LilyGO T-Deck Plus** | 320×240 ST7789, IPS | Touch + QWERTY + trackball | SX1262 | L76K | AXP2101 |
| **LilyGO T-LoRa Pager** | 480×222 ST7796U, IPS | QWERTY + rotary encoder | SX1262 | L76K | BQ27220 |
| **M5Stack Cardputer ADV** | 240×135 ST7789V2, IPS | 56-key QWERTY | SX1262 (Cap LoRa868) | AT6668 (Cap LoRa868) | BAT_ADC |

Building is per-device:
```
pio run -e tdeck_plus
pio run -e tlorapager
pio run -e cardputer_adv
```

---

## Core architecture

### Ghost Engine
A persistent Core-0 task that runs wardrive (WiFi + BLE +
optionally LoRa observation logging) continuously across app
launches. Apps that consume the radios coordinate via the SPI
Bus Treaty mutex; the engine never stops just because an app
exited.

### SPI Bus Treaty
A `spi_mutex` protects every SPI transaction. Each peripheral
(display, SD, LoRa, NFC where applicable) takes the mutex with
a named ownership tag, performs its transaction, and releases.
500 ms timeout, named ownership logging. Library-free — uses
our own mutex, not a wrapper around any third-party semaphore.

On the Cardputer specifically, the display is on a separate SPI
controller (HSPI) from the shared SD+LoRa bus (FSPI); the Treaty
only governs the shared bus.

### ELF loader
Apps can ship as `.elf` files loaded from SD. The loader maps the
ELF into PSRAM, resolves the small set of host symbols (gfx, sd,
pm_input, etc.), and hands control to the ELF's entry point.
Used by RetroPack for per-system emulators.

### Bridge
USB-CDC JSON protocol. The device acts as a sensor head for
desktop tooling — Gemini integration, the web emulator at
`piscesdemo.fluidfortune.com`, or any client that speaks the
protocol. ~30 event types covering wardrive, BLE, GPS, and
device telemetry.

### Width-aware rendering
Apps query `gfx->width()` / `gfx->height()` or per-device
constants. Headers span full width; game canvases (Snake,
Pac-Man, Galaga, Chess) keep fixed dimensions and center on
wider screens. The Cardputer adds a third device-specific layout
branch for the 240×135 form factor — labels become tighter,
font sizes mostly textSize=1, and several apps use detail
sub-screens instead of horizontally-scrolling tables.

---

## Project layout

```
PiscesMoon/
├── README.md                          ← you are here
├── platformio.ini                     ← three build envs
├── partitions.csv                     ← T-Deck Plus + T-LoRa Pager partition table
├── partitions_cardputer.csv           ← Cardputer 8MB partition table
├── secrets.h                          ← template, no keys (gitignored copy lives elsewhere)
├── secrets_h.example                  ← same template, distributed publicly
├── variants/
│   └── m5stack_cardputer_adv/
│       └── pins_arduino.h
├── include/
│   ├── theme.h                        ← color palette + per-device layout constants
│   ├── pm_input.h                     ← unified keyboard / trackball / gamepad input
│   ├── spi_treaty.h                   ← SPI Bus Treaty mutex
│   ├── text_buffer.h                  ← PSRAM-backed scrollback (Terminal, MicroPython, Notepad)
│   ├── game_input.h                   ← NES-layout input helper for games
│   └── pm_disp_tlorapager.h           ← T-LoRa Pager custom display driver
├── src/
│   ├── main.cpp                       ← setup() + Ghost Engine task + boot screens
│   ├── launcher.cpp                   ← T-Deck Plus + T-LoRa Pager launcher
│   ├── launcher_cardputer.cpp         ← Cardputer launcher (single-row side-scroll)
│   ├── about_app.cpp / system_app.cpp / etc.   ← ~50 apps
│   ├── wardrive.cpp                   ← Ghost Engine implementation
│   ├── bridge_app.cpp                 ← USB-CDC protocol
│   ├── elf_loader.cpp                 ← ELF runtime
│   ├── retro_elf_pack.cpp             ← RetroPack browser
│   └── atari7800/                     ← 7800 emulator (in development)
│       ├── README.md
│       └── a7800_main.cpp
└── docs/
    ├── CHANGELOG_v1_2_0.md            ← release notes
    ├── HANDOFF_SUMMARY.md             ← session log + integration notes
    └── (cardputer port docs)          ← phase plans, helper files, build instructions
```

---

## Building

Requires PlatformIO. From the project root:

```sh
# T-Deck Plus
pio run -e tdeck_plus -t upload

# T-LoRa Pager
pio run -e tlorapager -t upload

# M5Stack Cardputer ADV
pio run -e cardputer_adv -t upload
```

**First-time flash on each device:** flash the manufacturer's
factory firmware first to verify hardware, then flash Pisces Moon.

**Cardputer-specific build:** uses 8 MB flash with `qio_opi` octal
PSRAM memory mode. The `partitions_cardputer.csv` table is selected
automatically by the `[env:cardputer_adv]` block in `platformio.ini`.

---

## Key invariants

1. **Ghost Engine never stops.** Apps that interact with radios
   coordinate via the Treaty mutex, but the engine task itself
   persists.
2. **No exit on app crash.** App functions return to the launcher
   on Q/Esc; assertion failure or watchdog reset boots back to
   the launcher.
3. **Three devices, one codebase.** Every device-specific path is
   inside an `#ifdef DEVICE_*` block. Compiling for one device
   produces output byte-identical to a prior version unless that
   device's code path changed.

---

## Documents

- **`docs/CHANGELOG_v1_2_0.md`** — full v1.2.0 release notes
  including the Cardputer port
- **`docs/HANDOFF_SUMMARY.md`** — session integration notes
- **`docs/CARDPUTER_PORT_PLAN.md`** — Cardputer reflow master plan
  (all 48 apps with layout decisions)

---

*The Ghost Engine never stops. The SPI Bus Treaty is why.*

*Pisces Moon OS — Fluid Fortune — fluidfortune.com*
*AGPL-3.0-or-later*
