# Pisces Moon OS — v1.2.0 "Multi-Device" Inventory

**Build date:** 2026-05-16
**Author:** Eric Becker / Fluid Fortune
**License:** AGPL-3.0-or-later
**Site:** fluidfortune.com

This file lists every path in the ZIP. If your unzipping tool
shows a different layout (e.g. macOS Archive Utility replacing
directories instead of merging), use this as the ground truth.

## Top-level

```
.gitignore                          — Git ignore rules
CHANGELOG_v1_1_0.md                 — v1.1.0 release notes
CHANGELOG_v1_1_1.md                 — v1.1.1 release notes
CHANGELOG_v1_2_0.md                 — v1.2.0 release notes (UPDATED)
CLA.md                              — Contributor License Agreement
CNAME                               — GitHub Pages CNAME
INVENTORY.md                        — This file (NEW)
INVENTORY.txt                       — Flat file listing for diffing
PiscesMoon.code-workspace           — VS Code workspace
README.md                           — Repo readme
ghost_partition_gui.py              — Partition tool (GUI)
ghost_partition_tool.py             — Partition tool (CLI)
index.html                          — GitHub Pages landing page
partitions.csv                      — Partition table (T-Deck Plus / Pager)
partitions_cardputer.csv            — Partition table (Cardputer ADV)
platformio.ini                      — Main PlatformIO config
platformio_cardputer_block.ini      — Cardputer env stanza
secrets.h                           — API keys placeholder (no real keys)
secrets.h.example                   — Identical to secrets.h, for new clones
user_setup.h                        — TFT_eSPI config (legacy)
wardrive_splitter.py                — Utility for splitting WiGLE CSVs
```

## `src/` — 70 .cpp files

Source files (`.cpp`). The Cardputer build pulls every `.cpp` in
`src/` because PlatformIO's default `build_src_filter` is `+<*>`.
Per-device behavior is gated with `#ifdef DEVICE_*` blocks.

**NEW in this build:**
- `src/wardrive_inspect.cpp` — Ghost Ride The Whip diagnostic app

**MODIFIED this build:**
- `src/main.cpp` — Cardputer splash + BIOS auto-scroll
- `src/wardrive.cpp` — v2.7 lazy session-file creation (Pager fix)
- `src/launcher_cardputer.cpp` — Arrow key + Enter handling
- `src/keyboard.cpp` — Diagnostic instrumentation
- 16 other `.cpp` files — AGPL header added

## `include/` — 71 .h files

Headers (`.h`). Includes the `pm_input.h`, `hal_pins.h`,
`spi_treaty.h` foundation, app interfaces, and SDK shims.

**NEW in this build:**
- `include/wardrive_inspect.h` — Ghost Ride The Whip interface

**MODIFIED this build:**
- 47 `.h` files — AGPL header added

## `docs/` — 12 markdown documents

Reference documentation, hardware notes, design rationales.

## `variants/`

Per-device PlatformIO variant headers:
- `variants/lilygo_tlora_pager/pins_arduino.h` — Verbatim LilyGo
- `variants/m5stack_cardputer_adv/pins_arduino.h` — M5Stack Cardputer ADV

## `temp_hold/`

Scratch/staging area. Not built. Not referenced from `platformio.ini`.

## `markdown/`

Long-form whitepaper drafts and Lety IDE export staging.

---

## Critical: Unzip Safety

**macOS Archive Utility** has a known behavior where extracting a
ZIP into a directory containing the same name **replaces** the
existing directory wholesale instead of merging. If you extract
this ZIP into an existing checkout, **back up `src/` and `include/`
first** or extract into a fresh directory and rsync the result.

Recommended extraction:

```sh
unzip PiscesMoon_v1.2.0.zip -d /tmp/pisces-extract/
rsync -av /tmp/pisces-extract/PiscesMoon_build/ ~/workspace/PiscesMoon/
```

Or use `unzip -o` which merges instead of replaces.

---

*Pisces Moon OS — Fluid Fortune — fluidfortune.com*
*The Ghost Engine never stops. The SPI Bus Treaty is why.*
