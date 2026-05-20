// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// This program is free software: you can redistribute it
// and/or modify it under the terms of the GNU Affero General
// Public License as published by the Free Software Foundation,
// either version 3 of the License, or any later version.
//
// fluidfortune.com

/**
 * PISCES MOON OS — MICROPYTHON REPL v1.0
 *
 * Interactive Python environment on the T-Deck Plus.
 *
 * Current implementation provides:
 *   - Full REPL interface (input, history, display)
 *   - Built-in command set for hardware control
 *   - Script execution from SD card (/scripts/*.py)
 *   - Python-like expression evaluation for simple math/logic
 *
 * The full MicroPython interpreter requires a partition table
 * expansion (app0 3MB → 3.5MB) and a custom ESP-IDF component
 * build. This app provides the REPL framework and hardware
 * bindings so the UI and HAL are ready when the interpreter
 * is integrated.
 *
 * Built-in commands (available without MicroPython library):
 *   help()           - Show available commands
 *   ls()             - List SD card root
 *   ls('/path')      - List directory
 *   cat('/file.txt') - Print file contents
 *   run('/script.py')- Execute script from SD (stub)
 *   free()           - Show heap/PSRAM free
 *   wifi()           - Show WiFi status
 *   clear()          - Clear screen
 *
 * Controls:
 *   ENTER   = execute line
 *   UP      = previous history
 *   BKSP    = backspace
 *   CTRL-C  = interrupt (ASCII 3)
 *   Q (on empty line) = exit
 *   Tap header = exit
 */

#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include <Arduino_GFX_Library.h>
#include "SdFat.h"
#include "touch.h"
#include "trackball.h"
#include "keyboard.h"
#include "theme.h"
#include "micropython_app.h"

extern Arduino_GFX *gfx;
extern SdFat sd;

// ─────────────────────────────────────────────
//  COLORS — Classic Python REPL aesthetic
// ─────────────────────────────────────────────
#define PY_BG       0x0000
#define PY_HEADER   0x0841
#define PY_PROMPT   0xFFE0  // Yellow >>> prompt
#define PY_OUTPUT   0xFFFF  // White output
#define PY_ERROR    0xF800  // Red errors
#define PY_COMMENT  0x4208  // Dim comments
#define PY_STRING   0x07FF  // Cyan strings
#define PY_NUMBER   0xFD20  // Orange numbers
#define PY_GREEN    0x07E0
#define PY_DIM      0x4208

// ─────────────────────────────────────────────
//  TERMINAL LAYOUT
//  Header 22px, footer 14px → ~204px terminal
//  53 cols × ~22 rows at size 1
// ─────────────────────────────────────────────
#define PY_COLS      53
#define PY_ROWS      22
#define PY_CHAR_H    9
#define PY_TERM_TOP  24
#define PY_FOOT_H    16

// ─────────────────────────────────────────────
//  TERMINAL BUFFER
// ─────────────────────────────────────────────
static char  pyBuf[PY_ROWS][PY_COLS + 1];
static int   pyCurRow = 0;
static int   pyScrollTop = 0;

// Command history
#define PY_HIST_SIZE 20
static String pyHistory[PY_HIST_SIZE];
static int    pyHistCount = 0;
static int    pyHistIdx   = -1;

static void pyClearScreen() {
    for (int r = 0; r < PY_ROWS; r++)
        memset(pyBuf[r], 0, PY_COLS + 1);
    pyCurRow = 0;
    pyScrollTop = 0;
}

static void pyScrollUp() {
    for (int r = 0; r < PY_ROWS - 1; r++)
        memcpy(pyBuf[r], pyBuf[r + 1], PY_COLS + 1);
    memset(pyBuf[PY_ROWS - 1], 0, PY_COLS + 1);
    pyCurRow = PY_ROWS - 1;
}

static void pyPrint(const String& s, uint16_t color = PY_OUTPUT) {
    // Break into rows
    int col = 0;
    for (int i = 0; i < (int)s.length(); i++) {
        char c = s[i];
        if (c == '\n' || col >= PY_COLS) {
            pyCurRow++;
            if (pyCurRow >= PY_ROWS) pyScrollUp();
            col = 0;
            if (c == '\n') continue;
        }
        pyBuf[pyCurRow][col++] = c;
    }
    if (col > 0) {
        pyCurRow++;
        if (pyCurRow >= PY_ROWS) pyScrollUp();
    }
}

static void pyDrawScreen(const String& currentInput, bool multiline = false) {
    // Header
    gfx->fillRect(0, 0, 320, 22, PY_HEADER);
    gfx->drawFastHLine(0, 22, 320, PY_GREEN);
    gfx->setTextSize(1);
    gfx->setTextColor(PY_GREEN);
    gfx->setCursor(6, 7);
    gfx->print("MICROPYTHON REPL");
    gfx->setTextColor(0xFFE0);  // Python yellow
    gfx->setCursor(180, 7);
    gfx->print("Python 3 / ESP32-S3");

    // Terminal buffer
    gfx->fillRect(0, PY_TERM_TOP, 320, 240 - PY_TERM_TOP - PY_FOOT_H, PY_BG);
    gfx->setTextSize(1);
    int displayRows = (240 - PY_TERM_TOP - PY_FOOT_H) / PY_CHAR_H;
    int startRow = max(0, pyCurRow - displayRows);

    for (int r = startRow; r < pyCurRow && r < PY_ROWS; r++) {
        if (pyBuf[r][0] == 0) continue;
        // Simple syntax coloring
        uint16_t col = PY_OUTPUT;
        if (strncmp(pyBuf[r], ">>> ", 4) == 0 || strncmp(pyBuf[r], "... ", 4) == 0)
            col = PY_PROMPT;
        else if (strncmp(pyBuf[r], "Traceback", 9) == 0 ||
                 strstr(pyBuf[r], "Error:") != nullptr)
            col = PY_ERROR;
        else if (pyBuf[r][0] == '#')
            col = PY_COMMENT;
        gfx->setTextColor(col);
        gfx->setCursor(2, PY_TERM_TOP + (r - startRow) * PY_CHAR_H);
        gfx->print(pyBuf[r]);
    }

    // Input line
    gfx->fillRect(0, 240 - PY_FOOT_H, 320, PY_FOOT_H, 0x0821);
    gfx->drawFastHLine(0, 240 - PY_FOOT_H, 320, PY_DIM);
    gfx->setTextColor(PY_PROMPT);
    gfx->setCursor(2, 240 - PY_FOOT_H + 4);
    gfx->print(multiline ? "... " : ">>> ");
    gfx->setTextColor(PY_OUTPUT);

    String display = currentInput;
    if (display.length() > 48) display = display.substring(display.length() - 48);
    gfx->print(display);

    // Cursor blink (simple underscore)
    int cx = 2 + 24 + display.length() * 6;
    gfx->fillRect(cx, 240 - PY_FOOT_H + 12, 5, 2, PY_GREEN);
}

// ─────────────────────────────────────────────
//  BUILT-IN COMMAND EXECUTION
//  These work without MicroPython library.
//  Acts as a hardware abstraction shell.
// ─────────────────────────────────────────────
static String pyExecBuiltin(const String& cmd) {
    String c = cmd;
    c.trim();

    if (c == "help()") {
        return "Built-in commands:\n"
               "  help()           - This help\n"
               "  ls()             - List SD root\n"
               "  ls('/path')      - List directory\n"
               "  cat('/file.txt') - Print file\n"
               "  free()           - Memory stats\n"
               "  wifi()           - WiFi status\n"
               "  clear()          - Clear screen\n"
               "  run('/script.py')- Run script\n"
               "Full MicroPython: requires partition\n"
               "table expansion (see micropython_app.h)";
    }

    if (c == "clear()") {
        pyClearScreen();
        return "";
    }

    if (c == "free()") {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "Heap: %lu KB free / %lu KB total\n"
            "PSRAM: %lu KB free / %lu KB total",
            ESP.getFreeHeap() / 1024,
            ESP.getHeapSize() / 1024,
            ESP.getFreePsram() / 1024,
            ESP.getPsramSize() / 1024);
        return String(buf);
    }

    if (c == "wifi()") {
        if (WiFi.status() == WL_CONNECTED) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Connected: %s\nIP: %s",
                WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
            return String(buf);
        }
        return "WiFi: not connected";
    }

    if (c == "ls()" || c == "ls('/')") {
        if (!sd.fatType()) return "SD not mounted";
        String result = "";
        FsFile dir = sd.open("/");
        FsFile entry;
        while (entry.openNext(&dir, O_RDONLY)) {
            char name[64];
            entry.getName(name, sizeof(name));
            result += String(entry.isDirectory() ? "[DIR] " : "      ") + name + "\n";
            entry.close();
        }
        dir.close();
        return result.length() > 0 ? result : "(empty)";
    }

    // ls('/path')
    if (c.startsWith("ls('") && c.endsWith("')")) {
        String path = c.substring(4, c.length() - 2);
        if (!sd.exists(path.c_str())) return "Path not found: " + path;
        String result = "";
        FsFile dir = sd.open(path.c_str());
        FsFile entry;
        while (entry.openNext(&dir, O_RDONLY)) {
            char name[64];
            entry.getName(name, sizeof(name));
            result += String(entry.isDirectory() ? "[DIR] " : "      ") + name + "\n";
            entry.close();
        }
        dir.close();
        return result.length() > 0 ? result : "(empty)";
    }

    // cat('/path')
    if (c.startsWith("cat('") && c.endsWith("')")) {
        String path = c.substring(5, c.length() - 2);
        if (!sd.exists(path.c_str())) return "File not found: " + path;
        FsFile f = sd.open(path.c_str(), O_READ);
        if (!f) return "Cannot open: " + path;
        String content = "";
        while (f.available() && content.length() < 1000) {
            content += (char)f.read();
        }
        f.close();
        if (content.length() >= 1000) content += "\n... (truncated)";
        return content;
    }

    // run('/script.py') — stub until MicroPython integrated
    if (c.startsWith("run('") && c.endsWith("')")) {
        String path = c.substring(5, c.length() - 2);
        if (!sd.exists(path.c_str()))
            return "Script not found: " + path;
        return "Script execution requires full MicroPython.\n"
               "See micropython_app.h for integration guide.\n"
               "Script found: " + path;
    }

    // Simple arithmetic evaluation (no library needed)
    // Handles: 2+2, 10*5, 100/4, etc.
    bool isArith = true;
    for (char ch : c) {
        if (!isdigit(ch) && ch != '+' && ch != '-' &&
            ch != '*' && ch != '/' && ch != '.' && ch != ' ') {
            isArith = false;
            break;
        }
    }
    if (isArith && c.length() > 0) {
        // Very basic: single operation only
        for (int i = 0; i < (int)c.length(); i++) {
            char op = c[i];
            if (op == '+' || op == '-' || op == '*' || op == '/') {
                float a = c.substring(0, i).toFloat();
                float b = c.substring(i + 1).toFloat();
                float result = 0;
                if (op == '+') result = a + b;
                else if (op == '-') result = a - b;
                else if (op == '*') result = a * b;
                else if (op == '/') {
                    if (b == 0) return "ZeroDivisionError: division by zero";
                    result = a / b;
                }
                // Format: integer if whole, float if fractional
                if (result == (int)result) {
                    return String((int)result);
                } else {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.6g", result);
                    return String(buf);
                }
            }
        }
    }

    // Unknown command
    return "NameError: name '" + c + "' is not defined\n"
           "(Full MicroPython requires partition table expansion)";
}

// ─────────────────────────────────────────────
//  MAIN ENTRY
// ─────────────────────────────────────────────
void run_micropython() {
    gfx->fillScreen(PY_BG);
    pyClearScreen();
    pyHistCount = 0;
    pyHistIdx = -1;

    // Startup banner
    pyPrint("Pisces Moon OS MicroPython REPL");
    pyPrint("Python 3.x on ESP32-S3 (partial — built-ins active)");
    pyPrint("Type help() for commands. Q on empty line to exit.");
    pyPrint("");

    pyDrawScreen("");

    String input = "";
    bool multiline = false;

    while (true) {
        int16_t tx, ty;
        if (get_touch(&tx, &ty) && ty < 24) {
            while (get_touch(&tx, &ty)) delay(10);
            break;
        }

        char k = get_keypress();
        TrackballState tb = update_trackball();

        // History navigation via trackball up/down
        if (tb.y == -1 && pyHistCount > 0) {  // Up
            pyHistIdx = min(pyHistIdx + 1, pyHistCount - 1);
            input = pyHistory[pyHistIdx];
            pyDrawScreen(input, multiline);
            continue;
        }
        if (tb.y == 1 && pyHistIdx > 0) {  // Down
            pyHistIdx--;
            input = pyHistIdx >= 0 ? pyHistory[pyHistIdx] : "";
            pyDrawScreen(input, multiline);
            continue;
        }

        if (k == 0) { delay(15); continue; }

        // Exit on Q with empty input
        if ((k == 'q' || k == 'Q') && input.length() == 0) break;

        // CTRL-C interrupt
        if (k == 3) {
            pyPrint(">>> " + input);
            pyPrint("KeyboardInterrupt");
            input = "";
            multiline = false;
            pyDrawScreen(input, multiline);
            continue;
        }

        // Backspace
        if (k == 8 || k == 127) {
            if (input.length() > 0) {
                input.remove(input.length() - 1);
                pyDrawScreen(input, multiline);
            }
            continue;
        }

        // Enter — execute
        if (k == 13 || k == 10) {
            String prompt = multiline ? "... " : ">>> ";
            pyPrint(prompt + input);

            if (input.length() > 0) {
                // Save to history
                if (pyHistCount < PY_HIST_SIZE) {
                    for (int i = min(pyHistCount, PY_HIST_SIZE - 1); i > 0; i--)
                        pyHistory[i] = pyHistory[i - 1];
                    pyHistory[0] = input;
                    if (pyHistCount < PY_HIST_SIZE) pyHistCount++;
                }
                pyHistIdx = -1;

                // Execute
                String result = pyExecBuiltin(input);
                if (result.length() > 0) pyPrint(result);

                // Check for multiline trigger (colon at end)
                multiline = input.endsWith(":");
            } else {
                multiline = false;
            }

            input = "";
            pyDrawScreen(input, multiline);
            continue;
        }

        // Printable character
        if (k >= 32 && k <= 126) {
            if (input.length() < PY_COLS - 5) {
                input += k;
                pyDrawScreen(input, multiline);
            }
        }

        delay(15);
        yield();
    }

    gfx->fillScreen(PY_BG);
}