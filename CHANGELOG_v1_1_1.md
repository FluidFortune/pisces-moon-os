# Pisces Moon OS — Changelog v1.1.1

**Release:** v1.1.1 "ELF Treaty Patch"  
**Date:** 2026-05-07  
**Base:** v1.1.0

---

## What Changed

### Bug Fixes — ZIP Completeness
v1.1.0 shipped with several files that were updated during development
but not correctly included in the release package. This patch corrects that.

**Files updated from v1.1.0:**
- `src/wardrive.cpp` — includes WARDRIVE_MODE_PROMISCUOUS, wardrive_set_mode(),
  pm_promiscuous integration, _wardrive_on_pkt() streaming callback
- `src/bridge_app.cpp` — includes promiscuous_start/stop/lock/unlock/filter
  Bridge commands, updated visualizer with promiscuous stats display
- `src/launcher.cpp` — v1.1.1 version string
- `src/main.cpp` — v1.1.1 BIOS and splash strings
- `src/about_app.cpp` — v1.1.1 version display
- `src/doom_app.cpp` — SPI Bus Treaty fix (ty<40 header tap)
- `src/filesystem.cpp` — SPI Bus Treaty fix (PSRAM-buffered file viewer)
- `src/galaga.cpp` — SPI Bus Treaty fix (high score mutex)
- `src/notepad.cpp` — SPI Bus Treaty fix (mkdir+save mutex, ty<40)
- `src/pacman.cpp` — SPI Bus Treaty fix (high score mutex)
- `src/simcity.cpp` — SPI Bus Treaty fix (save mutex 1000ms)
- `src/snake.cpp` — SPI Bus Treaty fix (high score mutex)
- `src/elf_loader.cpp` — ELF API v1.1 SD helper function bodies
- `src/lora_voice.cpp` — full implementation (was zero bytes in v1.1.0)

**Files added (were missing from v1.1.0 ZIP entirely):**
- `include/pm_promiscuous.h` — ISR-safe 802.11 promiscuous mode engine.
  Hard dependency of wardrive.cpp. Build fails without it.
- `include/probe_intel.h` — Probe Intel app header
- `src/probe_intel.cpp` — RF Intel app (Scan Mode + Promiscuous Mode,
  user-selectable on launch). Not yet wired into launcher — see v1.2.

**Headers updated:**
- `include/wardrive.h` — wardrive_mode_t typedef, wardrive_set_mode() declaration
- `include/elf_loader.h` — ELF API v1.1 helper function pointer declarations
- `include/apps.h` — run_probe_intel() declaration (wiring pending v1.2)

---

## Notes

### probe_intel — not yet in launcher
`probe_intel.cpp` compiles but is not wired into the launcher menu yet.
It will appear in CYBER category in v1.2. The file is included now so the
build is complete and the code can be reviewed.

### pm_promiscuous.h — header-only
This is a header-only library (all inline functions). It has no corresponding
.cpp file. Include it once — wardrive.cpp already does this. Do not include
it in any other .cpp or you will get duplicate symbol errors.

### lora_voice.cpp
The full LoRa Voice implementation is now included. If you previously added
your own lora_voice.cpp to work around the missing file, replace it with this
version to get the complete implementation.

---

*Pisces Moon OS · Copyright (C) 2026 Eric Becker / Fluid Fortune · fluidfortune.com*  
*AGPL-3.0-or-later*
