# lib/ — PlatformIO library directory

PlatformIO auto-discovers any directory under `lib/` that contains `.c` or
`.cpp` files and compiles them as part of the project.

## lib/lua/

This directory is **populated by `../fetch_lua.sh`**. It is not checked
into the repo (gitignored — see `.gitignore` at the repo root if your
clone preserves it).

After running `./fetch_lua.sh`, you should see ~33 `.c` files and ~7 `.h`
files here: `lapi.c`, `lauxlib.c`, `lbaselib.c`, ..., `lua.h`, `lauxlib.h`,
`lualib.h`, `luaconf.h`, etc.

If `lib/lua/` is empty when you run `pio run`, the build will fail at the
first `#include "lua.h"` in `src/pm_shell.cpp`. Run `fetch_lua.sh` and
retry.

## Why isn't Lua a `lib_dep` in `platformio.ini`?

Lua doesn't publish a `library.json` or `library.properties` manifest in
its tarball, so the PlatformIO Library Registry doesn't carry it as a
first-class package. The fetch script approach gives us the official
upstream tarball verbatim with no third-party wrapper.

## Adding other libraries

Drop them as subdirectories here (`lib/<name>/`) with their own `.c`/`.h`
files. PlatformIO will pick them up automatically. For libraries that DO
have a manifest, prefer `lib_deps` in `platformio.ini`.
