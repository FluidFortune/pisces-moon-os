// pm_shell — Pisces Moon Sovereign Shell Firmware
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_shell.cpp — REPL core
//
//  Owns:
//    - the single global lua_State for this firmware's lifetime
//    - the FreeRTOS task that reads USB-CDC bytes and dispatches lines
//    - the print() override that routes Lua output through Serial
//    - the loadlibs selection (base/string/table/math/utf8 only)
//
//  Does NOT own:
//    - any pm.* binding implementations (those live in pm_shell_bindings.cpp)
//    - hardware-specific setup (the task body never touches PIN_*)
//
//  Concurrency:
//    Single-threaded with respect to Lua. The shell task is the only
//    code that touches L. Bindings called from Lua scripts are
//    therefore implicitly serialized.
// ─────────────────────────────────────────────

#include <Arduino.h>
#include "pm_shell.h"

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// Implemented in pm_shell_bindings.cpp — registers everything under
// the global "pm" table.
extern void pm_shell_register_bindings(lua_State* L);

// ─── Module state ────────────────────────────────────────
static lua_State*       L         = nullptr;
static TaskHandle_t     s_task    = nullptr;
static volatile bool    s_ready   = false;

// ─── Lua print() override ────────────────────────────────
//
// Lua's stock print() in lbaselib.c writes via the lua_writestring
// macro, which by default expands to fwrite(stdout). On Arduino-ESP32
// that path eventually reaches USB-CDC anyway, but going through
// Serial.write() directly gives us control over byte ordering and
// matches everything else in this firmware.
//
// Implementation: walk the Lua stack, convert each arg to string via
// luaL_tolstring (which respects __tostring metamethods), write the
// bytes through Serial, separate args with a tab, terminate with \n.
//
// Errors during conversion bubble up as Lua errors via luaL_tolstring;
// our caller (lua_pcall in the REPL loop) catches them.
static int lua_pm_print(lua_State* L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        Serial.write(reinterpret_cast<const uint8_t*>(s), len);
        lua_pop(L, 1);   // pop the string luaL_tolstring pushed
        if (i < n) Serial.write('\t');
    }
    Serial.write('\n');
    return 0;
}

// ─── Stdlib opener ───────────────────────────────────────
//
// Opens only the safe subset of the Lua standard library:
//   - base    (print, type, pairs, ipairs, tostring, error, pcall, ...)
//   - string  (string.format, string.sub, gmatch, ...)
//   - table   (table.insert, table.concat, ...)
//   - math    (math.floor, math.sin, math.random, ...)
//   - utf8    (utf8.char, utf8.codepoint, ...)
//
// Excluded:
//   - io      — would expose host-side fopen. Users want pm.storage.
//   - os      — os.exit() would call exit() and reboot the chip.
//   - package — require() needs a custom loader path-aware of SD.
//   - debug   — large, not needed at this level.
//   - coroutine — Phase 2 candidate when someone asks.
static void open_safe_libs(lua_State* L) {
    luaL_requiref(L, "_G",     luaopen_base,    1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string,  1); lua_pop(L, 1);
    luaL_requiref(L, "table",  luaopen_table,   1); lua_pop(L, 1);
    luaL_requiref(L, "math",   luaopen_math,    1); lua_pop(L, 1);
    luaL_requiref(L, "utf8",   luaopen_utf8,    1); lua_pop(L, 1);
}

// ─── REPL line evaluation ────────────────────────────────
//
// Classic Lua REPL pattern: first try the input as if the user typed
// "return <expr>". If that compiles, evaluate it and print results.
// If it fails to compile (because they typed a statement, not an
// expression — e.g. "x = 1" or "for i=1,3 do print(i) end"), fall
// back to compiling the line verbatim as a statement chunk.
//
// Either way, any error message is printed to Serial and the lua_State
// is left clean for the next prompt.
static void eval_line(lua_State* L, const char* src, size_t len) {
    if (len == 0) return;

    // Attempt 1: wrap in "return ... " and try as expression
    String expr = String("return ") + src;
    int status = luaL_loadbuffer(L, expr.c_str(), expr.length(), "=stdin");

    if (status != LUA_OK) {
        // Discard the load error and try again as a statement
        lua_pop(L, 1);
        status = luaL_loadbuffer(L, src, len, "=stdin");
        if (status != LUA_OK) {
            Serial.print("[parse error] ");
            Serial.println(lua_tostring(L, -1));
            lua_pop(L, 1);
            return;
        }
    }

    // Capture top-of-stack before call so we know how many results
    // came back.
    int top_before = lua_gettop(L) - 1;  // -1 for the function itself
    status = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (status != LUA_OK) {
        Serial.print("[runtime error] ");
        Serial.println(lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }

    // Print any return values — only relevant for the "return ..."
    // expression path. Statement chunks return nothing.
    int n_results = lua_gettop(L) - top_before;
    if (n_results > 0) {
        // Call print() with the results. lua_pm_print is on the global
        // table so we fetch it explicitly rather than relying on the
        // user-overridable print.
        lua_pushcfunction(L, lua_pm_print);
        lua_insert(L, -n_results - 1);   // move print under the results
        // pcall the print itself so a __tostring that throws doesn't
        // poison the REPL.
        if (lua_pcall(L, n_results, 0, 0) != LUA_OK) {
            Serial.print("[print error] ");
            Serial.println(lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

// ─── Task body ───────────────────────────────────────────
//
// Reads USB-CDC bytes into a fixed-size line buffer. Echoes printable
// characters back so the user sees what they type. Handles backspace
// (0x08 or 0x7F) by erasing the last buffered byte and emitting BS-SP-BS
// to the terminal. Dispatches the line to eval_line() on \n or \r.
//
// 256 bytes is enough for typical REPL input. Lines longer than that
// truncate silently — documented limitation. For longer programs, use
// pm.dofile("/scripts/foo.lua").
static void shell_task(void* /*pv*/) {
    L = luaL_newstate();
    if (!L) {
        Serial.println("[SHELL] FATAL: lua_newstate failed (out of memory)");
        s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    open_safe_libs(L);

    // Override the global print() with our Serial-routed version.
    // This MUST happen after openlibs (which installs the default).
    lua_pushcfunction(L, lua_pm_print);
    lua_setglobal(L, "print");

    pm_shell_register_bindings(L);

    s_ready = true;
    Serial.println("[SHELL] Lua state ready. Type pm.version() or "
                   "pm.dofile(\"/scripts/blink.lua\")");
    Serial.print("pm> ");

    static char buf[256];
    size_t pos = 0;

    while (true) {
        int avail = Serial.available();
        while (avail-- > 0) {
            int c = Serial.read();
            if (c < 0) break;

            // ── Newline → dispatch line ─────────────────
            if (c == '\r' || c == '\n') {
                Serial.println();
                buf[pos] = 0;
                if (pos > 0) {
                    eval_line(L, buf, pos);
                }
                pos = 0;
                Serial.print("pm> ");
                continue;
            }

            // ── Backspace / DEL → erase one char ────────
            if (c == 0x08 || c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    // VT100-friendly erase: back up, write space, back up.
                    // Most terminal emulators understand this.
                    Serial.write("\b \b", 3);
                }
                continue;
            }

            // ── Printable ASCII → buffer + echo ─────────
            if (c >= 0x20 && c < 0x7F) {
                if (pos < sizeof(buf) - 1) {
                    buf[pos++] = (char)c;
                    Serial.write((uint8_t)c);
                }
                // Silent truncation if buffer full. The line will
                // still dispatch on the next newline.
                continue;
            }

            // ── Ctrl-C → abandon current line ───────────
            if (c == 0x03) {
                Serial.println("^C");
                pos = 0;
                Serial.print("pm> ");
                continue;
            }

            // Other control chars (tab, ESC, etc.) — silently ignore
            // for Phase 1. Phase 2 with linenoise will handle them.
        }

        // Yield. 20 ms gives a smooth-feeling type-and-echo without
        // burning CPU on an idle prompt. lua_pcall executions are
        // synchronous and may take longer than 20 ms; that's fine.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ─── Public API ──────────────────────────────────────────
void pm_shell_setup() {
    if (s_task != nullptr) {
        Serial.println("[SHELL] pm_shell_setup() called twice — ignored");
        return;
    }
    // 12 KB stack: lua_pcall recursion + string operations + bindings
    // that touch Wire/SD have observable depth. Internal DRAM only —
    // FreeRTOS task stacks cannot live in PSRAM.
    //
    // Pinned to core 0. On Pisces Moon devices core 0 hosts Ghost
    // Engine when wardrive is running, but in this firmware nothing
    // else runs there — full core, full Lua-VM-go.
    BaseType_t result = xTaskCreatePinnedToCore(
        shell_task,
        "PmShell",
        12288,
        nullptr,
        1,           // priority — same as Arduino main task
        &s_task,
        0            // core 0
    );

    if (result != pdPASS) {
        Serial.println("[SHELL] FATAL: xTaskCreatePinnedToCore failed");
        s_task = nullptr;
    }
}

bool pm_shell_eval(const char* lua_source) {
    if (!L || !lua_source) return false;
    int status = luaL_loadstring(L, lua_source);
    if (status != LUA_OK) {
        Serial.print("[eval load error] ");
        Serial.println(lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        Serial.print("[eval runtime error] ");
        Serial.println(lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool pm_shell_ready() {
    return s_ready;
}
