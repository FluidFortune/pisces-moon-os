# Atari 7800 emulator — Pisces Moon OS

Original engineering work. No port of ProSystem exists for ESP32-S3.
Building from scratch for v1.2.x.

## Roadmap

**Phase 1 — 6502 CPU core (~2 weeks)**
- All documented opcodes
- Cycle counting (rough — refine later)
- Reference: easy6502, py65 for cross-checking traces
- Output: passes Klaus Dormann's 6502 functional test

**Phase 2 — MARIA display list (~3 weeks)**
- Display list parser
- Scanline rendering
- DMA timing (this is where most accuracy issues will live)
- Reference: ProSystem's `Maria.cpp` — STUDY but don't copy (license)
- Output: renders the Atari logo from a 7800 BIOS dump

**Phase 3 — Cartridge support (~1 week)**
- .a78 header parsing
- Bank switching for common cartridges (Super Game, Activision)
- Output: boots Asteroids, Pole Position II, Dig Dug

**Phase 4 — Audio (~1 week, lower priority)**
- TIA audio (shared with 2600)
- POKEY for cartridges that use it
- Output: sound effects in Pole Position II

**Phase 5 — Polish and accuracy (~ongoing)**
- Fix bugs caught by additional test ROMs
- Target: 60% game compatibility by v1.3.0

## Build target

`a7800.elf` placed in `/sd/elf/retro/a7800.elf` on the device.
The RetroPack launcher dispatches to it via the existing ELF loader.

## Reference materials

- 7800 hardware reference: https://www.atari7800.org/programming/
- ProSystem source (for STUDY only, NOT direct copy — GPL v2,
  Pisces Moon is AGPL-3.0): https://github.com/raz0red/js7800
- TIA documentation (shared with 2600): http://www.atarihq.com/danb/files/TIA_HW_Notes.txt

## License

AGPL-3.0-or-later (matches the parent project).
