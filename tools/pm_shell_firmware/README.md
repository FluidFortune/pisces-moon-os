# pm_shell — Pisces Moon Sovereign Shell Firmware

**An independent firmware. Not part of Pisces Moon OS.**

A standalone PlatformIO firmware that turns any Pisces Moon target device
into an interactive Lua REPL over USB serial, with bindings for the
device's hardware (GPIO, I2C, SD storage, WiFi, audio tone generation).

This is a *tool firmware*, like flashing a memory tester or a JTAG dongle.
You flash it onto a board when you want a sovereign hardware shell;
you re-flash Pisces Moon when you want the OS back.

---

## What it is

- Lua 5.4 interpreter running in its own FreeRTOS task
- 256-byte line-buffered REPL over USB-CDC `Serial`
- A `pm.*` namespace of ~17 functions covering common hardware ops
- Script files load from `/scripts/*.lua` on the device's SD card
- Builds for **all 5 Pisces Moon device targets**, including Cardputer ADV
  (the no-PSRAM device — possible here because we don't ship Pisces Moon's
  WiFi+BLE+wardrive baggage alongside)

## What it is NOT

- Not the Pisces Moon launcher. No screen UI, no apps, no NoSQL, no wardrive.
- Not a terminal emulator. No tab completion, no command history, no ANSI
  escape codes. Just plain line input. Run it through `screen`, `minicom`,
  PuTTY, or your editor's serial pane — those handle terminal niceties.
- Not coordinated with Pisces Moon's SPI mutex (there is no Pisces Moon
  running). The shell owns the device.
- Not an ELF loader. That comes later.

---

## Build prerequisites

1. **PlatformIO Core** installed (CLI or VS Code extension)
2. **One of the supported boards** physically connected via USB:
   - LilyGO T-Deck Plus
   - LilyGO T-LoRa Pager
   - M5Stack Cardputer ADV
   - LCDwiki C28P
   - Sunton Maxine (ESP32-8048S050C 5" 800×480)
3. **`curl` and `tar`** on your build host (for `fetch_lua.sh`)

---

## Build steps

### 1. Fetch Lua sources

```bash
cd tools/pm_shell_firmware
./fetch_lua.sh
```

This downloads `lua-5.4.7.tar.gz` from `lua.org`, extracts it, copies the
relevant `.c` and `.h` files into `lib/lua/`, and removes the CLI entry
points (`lua.c`, `luac.c`) we don't need. Run once; results are
gitignored.

If your build host can't reach `lua.org`, manually download
`https://www.lua.org/ftp/lua-5.4.7.tar.gz`, extract it, and copy
`lua-5.4.7/src/*.c` and `lua-5.4.7/src/*.h` to `lib/lua/`, then delete
`lib/lua/lua.c` and `lib/lua/luac.c`.

### 2. Build for your target

```bash
pio run -e tdeck_plus
pio run -e tlorapager
pio run -e cardputer_adv
pio run -e c28p
pio run -e maxine
```

### 3. Flash

```bash
pio run -e tdeck_plus --target upload
```

### 4. Connect

```bash
pio device monitor -e tdeck_plus
```

You should see the banner and a `pm>` prompt within ~3 seconds of flash
complete. Type `pm.version()` to verify the Lua state is alive.

---

## Quick reference — the `pm.*` namespace

| Function | Description |
| --- | --- |
| `pm.version()` | Firmware version string |
| `pm.uptime()` | Seconds since boot |
| `pm.millis()` | Milliseconds since boot |
| `pm.heap()` | Returns `{internal=N, psram=N, total=N}` byte counts |
| `pm.delay(ms)` | Non-busy delay, yields to FreeRTOS |
| `pm.reboot()` | `esp_restart()` after a 100ms flush |
| `pm.gpio.mode(pin, mode)` | mode: `"input"` / `"output"` / `"input_pullup"` |
| `pm.gpio.read(pin)` | Returns 0 or 1 |
| `pm.gpio.write(pin, level)` | level: 0 or 1 |
| `pm.i2c.scan()` | Returns array of present 7-bit addresses |
| `pm.i2c.read(addr, reg)` | Reads one byte from register, returns int or nil |
| `pm.i2c.write(addr, reg, val)` | Writes one byte, returns bool |
| `pm.storage.exists(path)` | Returns bool |
| `pm.storage.read(path)` | Returns file contents as string, or nil |
| `pm.storage.write(path, text)` | Overwrites file, returns bool |
| `pm.storage.list(dir)` | Returns array of entry names |
| `pm.storage.mkdir(path)` | Returns bool |
| `pm.storage.remove(path)` | Returns bool |
| `pm.wifi.scan()` | Returns array of `{ssid, bssid, rssi, channel, encryption}` |
| `pm.wifi.connect(ssid, password)` | Returns bool after ~6s connection attempt |
| `pm.wifi.disconnect()` | Drops the current connection |
| `pm.wifi.status()` | Returns `{connected=bool, ip=string, rssi=N}` |
| `pm.audio.tone(pin, freq, ms)` | LEDC-based square-wave tone — works on every device, no codec assumptions |
| `pm.dofile(path)` | Loads + runs `/scripts/*.lua` (or any path) from SD |

Plus standard Lua libraries: `string`, `table`, `math`, `utf8`. The
following stdlib modules are **excluded** for safety: `io` (use
`pm.storage` instead), `os` (`os.exit` would call `exit()` and reboot
the chip), `package`, `debug`, `coroutine`.

---

## Per-device notes

### T-Deck Plus
SD: SPI bus on `MOSI=41 / MISO=38 / SCK=40 / CS=39`. Pisces Moon ships
SdFat here; pm_shell uses Arduino `<SD.h>` (simpler API, returns `fs::File`
to match the C28P backend). 4 MHz clock.

### T-LoRa Pager
SD: SPI bus on `MOSI=34 / MISO=33 / SCK=35 / CS=21`. Shared with LoRa
and NFC on the same bus, but pm_shell doesn't touch those — SD owns the
bus exclusively in this firmware.

### Cardputer ADV
8MB flash, no PSRAM. The Lua VM lives in internal DRAM here.
Reduced-binding mode is automatic: `pm.wifi.*` is still available
(WiFi STA mode works without PSRAM), but heavy script use will tighten
the heap. Typical scripts run fine; deep recursion may not.
SD: SPI on `MOSI=14 / MISO=39 / SCK=40 / CS=12`.

### C28P
SD on SDIO 4-bit (`PIN_SD_CLK=38 / CMD=40 / D0=39 / D1=41 / D2=48 / D3=47`)
via Arduino's `SD_MMC`. Touch panel is initialized but not exposed to
Lua in Phase 1.

### Maxine (ESP32-8048S050C 5″ 800×480)
SD on SPI (`MOSI=11 / SCK=12 / MISO=13 / CS=10`). RGB parallel display
is not initialized in pm_shell — the framebuffer would eat 768KB of
PSRAM that we don't need to allocate for a serial shell. Touch GT911
not exposed in Phase 1.

---

## Sample session

```
===========================================
  pm_shell — Pisces Moon Sovereign Shell
  v0.1.0 — target: tdeck_plus
===========================================
[SHELL] Lua state ready. Type pm.version() or pm.dofile("/scripts/blink.lua")
pm> pm.version()
0.1.0
pm> pm.heap()
table: 0x3fcb1234
pm> for k,v in pairs(pm.heap()) do print(k,v) end
internal    240128
psram       8154628
total       8394756
pm> pm.uptime()
12
pm> pm.i2c.scan()
table: 0x3fcb5678
pm> for _, a in ipairs(pm.i2c.scan()) do print(string.format("0x%02X", a)) end
0x18
0x38
0x71
pm> pm.dofile("/scripts/wifi_scan.lua")
[WiFi] Scanning...
[WiFi] 12 networks found:
  HomeNet         -53dBm  ch6   WPA2
  Hidden          -71dBm  ch11  WPA2
  ...
pm> pm.reboot()
[SHELL] Rebooting in 100ms...
```

(`pm.heap()` returns a table; the REPL prints it as a Lua table reference.
Use `pairs(pm.heap())` or destructure it to see the values.)

---

## Files in this tree

```
tools/pm_shell_firmware/
├── README.md                     ← this file
├── platformio.ini                ← multi-target build config (5 envs)
├── partitions.csv                ← 16MB partition table
├── partitions_cardputer.csv      ← 8MB partition table
├── fetch_lua.sh                  ← downloads + installs Lua 5.4.7
├── lib/
│   ├── README.md                 ← (lib/lua/ populated by fetch_lua.sh)
│   └── lua/                      ← Lua 5.4.7 source (after fetch)
├── include/
│   └── pm_shell.h                ← public API
├── src/
│   ├── main.cpp                  ← entry: Serial.begin + pm_shell_setup
│   ├── pm_shell.cpp              ← REPL task body + Lua state lifecycle
│   └── pm_shell_bindings.cpp     ← all the pm.* C functions
└── scripts/
    ├── README.md                 ← copy these to your SD card's /scripts/
    ├── blink.lua                 ← GPIO toggle demo
    ├── wifi_scan.lua             ← formatted scan dump
    ├── i2c_scan.lua              ← bus enumeration
    └── audio_test.lua            ← tone sweep (device-agnostic via LEDC)
```

---

## Risks and limitations

1. **Lua errors use longjmp.** Bindings deliberately don't hold mutexes
   or OS resources across `lua_error`. If you write your own binding,
   follow the pattern: acquire → do work → release → push error → return.

2. **Concurrent USB-CDC writes.** Only the shell task writes to Serial
   in this firmware (no background tasks), so there's no interleaving
   risk like Pisces Moon's wardrive logs cause.

3. **`pm.reboot()` mid-write corrupts the file.** Same as pulling power.

4. **No memory protection between scripts.** A script can call
   `pm.gpio.write(VBUS_PIN, 0)` and brick the session if you wire to
   the wrong pin. This is a sovereign tool. Be careful.

5. **WiFi cannot run concurrently with BLE on this firmware.** BLE
   support isn't compiled in for Phase 1; if you need both, that's
   Phase 2.

6. **Cardputer ADV without PSRAM** has ~50KB free heap after Lua state
   creation. Scripts allocating large tables (`pm.wifi.scan()` of 100
   networks at once) may hit `not enough memory` Lua errors. Solution:
   call scan, iterate, discard.

---

## Versioning

`pm_shell` versions independently of Pisces Moon. Current: **0.1.0**.

This firmware is Phase 1 (REPL + core bindings). Roadmap:

- **0.2.x** — Optional `linenoise` integration for line editing + history
  over USB-CDC. Tab completion of `pm.*` namespace.
- **0.3.x** — BLE scan binding, LoRa binding on Pager/T-Deck, NFC binding
  on Pager.
- **0.4.x** — On-device terminal UI for T-Deck Plus / Pager / Cardputer
  (gfx + keyboard, not just USB-CDC).
- **1.0.0** — Stable API. ELF loader integration arrives separately
  (potentially as `pm_shell` 2.x or as a sister tool).

---

## License

AGPL-3.0-or-later, matching Pisces Moon OS.
Lua itself is MIT — see `lib/lua/` after fetch.
