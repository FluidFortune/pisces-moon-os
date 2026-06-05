# pm_shell — Phase 1 scope

**Codename:** Sovereign Terminal
**Target:** v1.3.x feature, not v1.3 architectural cleanup
**Status:** scoped, not started

---

## What Phase 1 is

A Lua REPL pinned to a FreeRTOS task, listening on USB-CDC `Serial`, with a small set of bindings (`pm.*`) into the parts of Pisces Moon that are useful for hardware bring-up and live debugging. Scripts can be loaded from SD via `pm.dofile("/scripts/foo.lua")`. No on-device terminal UI yet — that's Phase 3.

## What Phase 1 is NOT

- Not a `linenoise`/VT100 terminal emulator. Tab completion, history, escape codes — all Phase 3.
- Not a touch-kiosk shell. The C28P/Maxine REPL UX is Phase 4 (or never; a "script picker" is more honest for those devices).
- Not a replacement for `voice_terminal.cpp` or `terminal_app.cpp` — those stay where they are.
- Not interactive on T-LoRa Pager's physical keyboard yet (the TCA8418 keyboard scan integrates in Phase 3).
- Not coordinated with the SPI mutex. Heavy SD work from the shell while wardrive is scanning will produce errors. Documented limitation.

## Architecture decisions

### Language: Lua 5.4

Decision over PikaScript: maturer C API, stable for 20+ years, well-trodden binding patterns from NodeMCU/OpenWRT/game engines, smaller surface for what we need. PikaScript's "4KB RAM" claim is real for the empty interpreter but vanishes once you bind GPIO/SD/audio/wifi.

Source acquired as: drop `lua-5.4.7/src/*.c, *.h` into `lib/lua/` and let PlatformIO auto-discover. No build system changes needed. Skip `lua.c` and `luac.c` (they're the standalone tools — we embed, not link to an exe).

### Concurrency model: dedicated FreeRTOS task

The launcher functions (`c28p_launcher`, `maxine_launcher`, `run_launcher`) all own `loop()` permanently and never return. Cooperative polling from the main loop is not an option. The shell runs on its own pinned task:

- **C28P / Maxine:** pin to Core 0 (Ghost Engine doesn't run on these, Core 0 is otherwise idle).
- **T-Deck Plus / T-LoRa Pager / Cardputer ADV:** pin to Core 1 (Ghost Engine owns Core 0 for wardrive).

Stack: 12 KB. The Lua VM lives on the heap, not the stack, but `lua_pcall` recursion needs headroom — 8 KB minimum, 12 KB safe.

Priority: 1 (same as Ghost Engine). Lower than wardrive scan task on Pager (priority 2 there) — wardrive wins SPI contention.

### Persistence model: one Lua state, lives forever

Single `lua_State*` created once at setup, never destroyed. Lua's GC keeps it bounded. If a script blows up, `lua_pcall` catches and prints; the state survives.

### I/O model

- **stdin:** poll `Serial.available()` in the task, accumulate into a 256-byte line buffer until `\n` or `\r\n`. Echo back per byte so the user sees what they type.
- **stdout:** Lua's `print()` writes through `lua_writestring` macro — point that at `Serial.write()` via `LUAI_USER_CONFIG` in `luaconf.h`. (One-line override.)
- **stderr:** same as stdout for Phase 1.

### Build flag

Whole thing gated behind `-DPM_SHELL_ENABLE=1`. Default on for PSRAM devices; off on Cardputer ADV.

## File layout

```
lib/lua/                          (drop-in Lua 5.4.7 source)
  lapi.c, lauxlib.c, lbaselib.c, lcode.c, lcorolib.c,
  lctype.c, ldblib.c, ldebug.c, ldo.c, ldump.c, lfunc.c,
  lgc.c, linit.c, liolib.c, llex.c, lmathlib.c, lmem.c,
  loadlib.c, lobject.c, lopcodes.c, loslib.c, lparser.c,
  lstate.c, lstring.c, lstrlib.c, ltable.c, ltablib.c,
  ltm.c, lundump.c, lutf8lib.c, lvm.c, lzio.c,
  lua.h, lauxlib.h, lualib.h, luaconf.h
  (NOT lua.c, NOT luac.c — those are CLI tools)
include/pm_shell.h                (public API surface)
src/pm_shell.cpp                  (task body, state lifecycle, line reader)
src/pm_shell_bindings.cpp         (the pm.* C functions registered to Lua)
docs/PM_SHELL_PHASE1_SCOPE.md     (this file)
docs/SHELL.md                     (user-facing reference, written during build)
```

## Public API

`include/pm_shell.h`:

```cpp
#pragma once
#ifdef PM_SHELL_ENABLE

// Called once from main.cpp setup(), after Serial.begin() and after
// pm_storage / SD mount have completed. Creates the Lua state,
// registers bindings, spawns the shell task. Idempotent.
void pm_shell_setup();

// Manual eval — runs a string through the Lua state from C code.
// Useful for boot-time autorun (e.g. /scripts/init.lua) and for
// other apps that want to fire one-shot scripts.
// Returns true if the line evaluated without error.
bool pm_shell_eval(const char* lua_source);

// True once the task has finished initializing and is reading input.
// Apps that want to wait before printing to Serial can poll this.
bool pm_shell_ready();

#endif
```

Three functions total. The actual REPL loop is private inside `pm_shell.cpp`.

## Binding surface — `pm.*` namespace

Phase 1 minimum useful set. All bindings live in `src/pm_shell_bindings.cpp` and register at task start.

### `pm.heap()`
Returns a table `{internal=N, psram=N, total=N}` of byte counts. Useful for "did my last script leak."

### `pm.millis()`
Returns `millis()` as a Lua integer.

### `pm.delay(ms)`
`vTaskDelay(pdMS_TO_TICKS(ms))`. Yields to other tasks — does NOT busy-wait.

### `pm.gpio.mode(pin, mode)` / `pm.gpio.read(pin)` / `pm.gpio.write(pin, level)`
Direct GPIO. `mode` is a string: `"input"`, `"output"`, `"input_pullup"`. Returns/accepts 0 or 1. No fancy alternate-function handling — that's what the script's job is.

### `pm.i2c.scan()`
Returns a table of integer addresses present on the I2C bus. Reuses the existing `Wire` instance. Used during board bring-up.

### `pm.storage.exists(path)` → bool
### `pm.storage.read(path)` → string (or nil)
### `pm.storage.write(path, text)` → bool
### `pm.storage.list(dir)` → table of strings
### `pm.storage.mkdir(path)` → bool
### `pm.storage.remove(path)` → bool

All wrap `pm_storage::*`. The shell intentionally does NOT acquire the SPI mutex — that's a Phase 2 concern. Docs warn: "don't write large files while wardrive scans."

### `pm.nosql.count(category)` → integer
### `pm.nosql.get(category, idx)` → (title, content) two values
### `pm.nosql.save(category, title, content)` → bool

Wraps `nosql_store.*`. Same SPI-contention caveat.

### `pm.wifi.scan()` → table of `{ssid, bssid, rssi, channel, encryption}`
Returns the current `WiFi.scanNetworks` result. Does not start a scan if one isn't already running — it's a snapshot.

### `pm.audio.tone(freq, ms)` *(C28P only — `#ifdef DEVICE_C28P`)*
Wraps `c28p_audio_tone()`. Errors if not in `PLAYBACK_TONE` mode.

### `pm.audio.mode()` *(C28P only)* → string
Returns `"idle"` / `"tone"` / `"audio"` / `"record"`.

### `pm.audio.enter(mode)` *(C28P only)* → bool
Wraps `c28p_audio_enter()`. `mode` is one of the four strings above.

### `pm.audio.release()` *(C28P only)*
Wraps `c28p_audio_release()`.

### `pm.dofile(path)` → bool
Loads a script from SD via `pm_storage::open`, evaluates it through the same `lua_State`. Replaces stdlib `dofile`/`loadfile` which doesn't know about SdFat/SD_MMC.

### `pm.reboot()`
`esp_restart()`. Comes with a 100ms delay so the closing `print()` actually flushes.

### `pm.version()` → string
Returns `PISCES_OS_VERSION`.

That's the Phase 1 surface. ~17 functions. Estimated 400 lines of binding code.

## main.cpp integration

Insertion point: after Ghost Engine spawn, before the splash screen. Two lines:

```cpp
#ifdef PM_SHELL_ENABLE
    pm_shell_setup();
    drawBootLine("00:14", "PM_SHELL",              nullptr, 1, "ACTIVE");
#endif
```

The task spawns inside `pm_shell_setup()`; main code continues immediately. The task itself doesn't try to print anything until the launcher has booted (small initial `vTaskDelay(2000)` at task start, to keep boot-log output coherent on USB).

## Per-device matrix

| Device           | PM_SHELL_ENABLE | Core | Stack  | Rationale                            |
| ---------------- | --------------- | ---- | ------ | ------------------------------------ |
| T-Deck Plus      | yes             | 1    | 12 KB  | PSRAM, plenty of headroom            |
| T-LoRa Pager     | yes             | 1    | 12 KB  | PSRAM, plenty of headroom            |
| C28P             | yes             | 0    | 12 KB  | PSRAM, Core 0 free                   |
| Maxine           | yes             | 0    | 12 KB  | PSRAM, Core 0 free                   |
| Cardputer ADV    | **no**          | —    | —      | No PSRAM. Lua + bindings would eat the wardrive+NimBLE budget. Revisit if needed; could be enabled with reduced binding surface. |

## platformio.ini changes

```ini
[common]
; existing entries ...

build_flags_common =
    ; existing entries ...
    -DLUA_32BITS                    ; lua integer/number on a 32-bit MCU
    -DLUA_USE_C89                   ; portable build, no POSIX-isms

; Per-device opt-in:
[env:tdeck_plus]
build_flags =
    ${common.build_flags_common}
    ${common.build_flags_psram}
    -DDEVICE_TDECK_PLUS
    -DPM_SHELL_ENABLE=1             ; ← add

[env:tlorapager]
build_flags =
    ${common.build_flags_common}
    ${common.build_flags_psram}
    -DDEVICE_TLORAPAGER
    -DPM_SHELL_ENABLE=1             ; ← add
    ; ... rest of pager flags

[env:c28p]
build_flags =
    ${common.build_flags_common}
    ${common.build_flags_psram}
    -DDEVICE_C28P
    -DPM_SHELL_ENABLE=1             ; ← add
    ; ... rest of c28p flags

[env:maxine]
build_flags =
    ${common.build_flags_common}
    ${common.build_flags_psram}
    -DDEVICE_MAXINE
    -DPM_SHELL_ENABLE=1             ; ← add
    ; ... rest of maxine flags

; Cardputer ADV does NOT define PM_SHELL_ENABLE.
```

And `pm_shell.cpp`, `pm_shell_bindings.cpp` need adding to the C28P and Maxine `build_src_filter` blocks (SPI devices pick them up via `+<*>`).

## luaconf.h customization

One file in `lib/lua/luaconf.h` needs a small edit to route `print()` through `Serial.write`. Lua's default is `fputs(stdout, s)`. Override the macro at the top:

```c
#define LUAI_USER_CONFIG
#include "../../include/pm_shell_lua_config.h"
```

And create `include/pm_shell_lua_config.h`:

```c
#pragma once
#include <Arduino.h>
#define lua_writestring(s,l)   Serial.write((const uint8_t*)(s), (l))
#define lua_writeline()        Serial.write('\n')
#define lua_writestringerror(s,p)  Serial.printf((s), (p))
```

This is the single integration point with the host. Three macros. Lua's stdlib `print` now Just Works over USB-CDC.

## Stdlib pruning

Lua's default stdlib opens five things via `luaL_openlibs`: `base`, `coroutine`, `string`, `table`, `math`, `io`, `os`, `package`, `debug`, `utf8`.

Phase 1 opens: **base, string, table, math, utf8**.

Skipped:
- **io** — would expose `fopen` to host filesystem; we want `pm.storage` instead.
- **os** — `os.exit()` would call `exit()` and reset the chip. `os.time()` etc. work but the safer path is `pm.millis()`.
- **package** — `require` needs a custom loader anyway. Defer to Phase 2.
- **debug** — large, rarely needed at this level. Defer.
- **coroutine** — Phase 2 if anyone asks. Useful for scripted state machines.

Pruning is two lines in `pm_shell.cpp` — call `luaL_requiref` selectively instead of `luaL_openlibs`.

## Memory budget

Rough estimates from the Lua 5.4 reference + measurements from NodeMCU:

- Lua VM heap at startup: ~16 KB
- Bindings + globals registered: +8 KB
- Idle (no script running): ~24 KB heap usage
- Typical script (say, `pm.wifi.scan()` returning 20 networks): +4 KB transient

Comfortable on PSRAM. The task stack itself (12 KB) lives in internal DRAM. Total internal DRAM footprint at idle: ~16 KB stack + ~4 KB FreeRTOS task overhead.

Cardputer ADV: would steal ~30 KB from the WiFi/BLE budget. Not worth it for Phase 1.

## Risk list

1. **Lua's longjmp error model.** Lua throws errors via `setjmp/longjmp`. ESP-IDF + FreeRTOS support this on Xtensa, but bindings must NOT hold OS resources across `lua_error`. Pattern: acquire mutex → do work → release mutex BEFORE pushing error onto the stack and calling `lua_error`. Phase 1 bindings don't acquire mutexes (deliberate — the SPI mutex contention is a known limitation), so this risk is bounded.

2. **`Serial.available()` polling latency.** USB-CDC bytes can arrive in bursts of 64. Our line reader needs to drain them all per task tick, not one byte per tick. Implementation: `while (Serial.available() > 0) { read one char; if (newline) eval line; }`, then `vTaskDelay(20)`.

3. **Concurrent `Serial.print` from other tasks.** Ghost Engine prints to Serial. So does wardrive. So does the launcher. The shell's REPL output can interleave with those. Phase 1 acceptable — Serial in Arduino-ESP32 is already thread-safe at the byte level (USB-CDC ring buffer). Bytes don't corrupt, but lines do interleave. Phase 3 could add a `Serial.print` mutex.

4. **Script files on SD during wardrive.** `pm.dofile("/scripts/foo.lua")` opens a file via `pm_storage::open` which on SPI devices touches the SdFat instance. The wardrive task also touches SdFat. Without the mutex, this can corrupt either operation. Phase 1 doc warns user; Phase 2 adds mutex coordination.

5. **`pm.reboot()` mid-write.** If someone calls `pm.reboot()` while an SD write is in flight, the file is corrupted. Document as expected behavior — same as pulling power.

6. **Lua 5.4 → ESP32 floating point.** Lua 5.4 has integer + float types separately. ESP32-S3 has hardware single-precision float. `LUA_FLOAT_TYPE` defaults to double, which is software emulated. Define `LUA_FLOAT_TYPE=LUA_FLOAT_FLOAT` to use float32 — same precision as the FPU, ~2× faster numerics. (Build flag: `-DLUA_FLOAT_TYPE=2` per `luaconf.h`.)

## Test plan (post-build)

1. **Smoke:** USB-connect T-Deck Plus, see boot log line `[PM_SHELL] ACTIVE`, see prompt `pm> `, type `pm.version()` → returns version string.
2. **Heap:** `pm.heap()` returns sensible numbers; `pm.heap().psram > 1000000` on PSRAM devices.
3. **GPIO:** `pm.gpio.mode(42, "output"); pm.gpio.write(42, 1)` should flip the backlight on if 42 is a backlight pin. (Reuse a known pin from the device under test.)
4. **NoSQL:** `pm.nosql.count("notes")` returns the number of saved notes. After `pm.nosql.save("notes", "test", "hi")`, count goes up by 1.
5. **Audio (C28P):** `pm.audio.enter("tone"); pm.audio.tone(440, 200); pm.audio.release()` produces a 200ms tone, leaves HAL in IDLE.
6. **WiFi:** `pm.wifi.scan()` returns a table; iterating prints each network.
7. **Script:** create `/scripts/blink.lua` on SD with a loop that toggles a GPIO 5 times; run `pm.dofile("/scripts/blink.lua")`; observe.
8. **Error survival:** `pm.gpio.read("not a pin")` produces a Lua error, prompt returns, next line works.
9. **Coexistence:** start wardrive on Pager; shell still prints `pm.heap()` cleanly (interleaves with `[Wardrive]` lines but doesn't crash).
10. **Reboot:** `pm.reboot()` resets the device; on reboot the shell comes up again.

## What gets shipped at end of Phase 1

- `lib/lua/` populated with Lua 5.4.7 source
- `include/pm_shell.h` + `include/pm_shell_lua_config.h`
- `src/pm_shell.cpp` (~250 lines: task body, line reader, REPL loop)
- `src/pm_shell_bindings.cpp` (~400 lines: 17 `pm.*` functions)
- `platformio.ini`: build flags added per-device
- `main.cpp`: 4-line integration in `setup()`
- `docs/SHELL.md`: user-facing reference (every `pm.*` function with examples)
- CHANGELOG entry under "v1.3 features"

## Estimated effort

- Lua source drop-in + PlatformIO build verification on one device: **1 session**
- `pm_shell.cpp` task body + line reader + lua_State lifecycle: **1 session**
- `pm_shell_bindings.cpp` core bindings (heap, gpio, storage, nosql, wifi): **1 session**
- C28P audio bindings + cross-device build verification: **1 session**
- `docs/SHELL.md` + smoke tests on all enabled devices: **1 session**

**Total: ~5 focused sessions**, plus 1 buffer for issues that surface during cross-device testing.

## Phase 2 preview (NOT in this scope)

- Mutex coordination with wardrive task (`pm.storage.*` acquires SPI mutex)
- `linenoise` integration for line editing, history, tab completion (still over USB-CDC)
- `coroutine` and `package`/`require` from Lua stdlib
- `pm.lora.*` bindings on Pager / T-Deck Plus
- `pm.ble.*` for ad-hoc BLE scanning from script
- Script autoload at boot (`/scripts/init.lua` if present)

## Phase 3+ preview (further out)

- On-device terminal UI on T-Deck Plus / Pager / Cardputer (VFS shim routing `linenoise` to gfx + keyboard)
- Touch-kiosk "script picker" on C28P / Maxine (tap a `.lua`, output to scrollable text view — NOT a REPL)
- ELF loader integration: Lua scripts that load + invoke ELF modules
- Sandboxed execution policy for untrusted scripts (sandbox tables, no `pm.gpio`, etc.)

---

## Open questions for the user

1. **Cardputer ADV inclusion.** Phase 1 default is "no shell on Cardputer ADV." Acceptable, or do you want a reduced-binding version (skip wifi/nosql, keep gpio/storage) to fit?
2. **Boot autorun.** Should `/scripts/init.lua` (if present on SD) be evaluated automatically at boot, or strictly opt-in via `pm.dofile` from a typed line? Default: opt-in. Phase 2 adds autorun.
3. **Prompt customization.** Default prompt `pm> `. Worth making device-aware (`tdeck> `, `c28p> ` etc.) for multi-device serial captures? Trivial change.
4. **`print` to gfx too.** Some users will want shell output mirrored to a small region on the display (corner debug overlay). Worth scoping for Phase 1 as `pm.gfx.tail(N)` returning the last N print lines, or defer entirely to Phase 3?
