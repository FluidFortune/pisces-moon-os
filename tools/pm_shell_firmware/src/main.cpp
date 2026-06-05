// pm_shell — Pisces Moon Sovereign Shell Firmware
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  main.cpp — Entry point for the pm_shell firmware
//
//  This firmware exists to provide a sovereign Lua REPL over USB-CDC
//  on Pisces Moon target devices. There is no launcher, no display
//  rendering, no app dispatch. The shell task runs on core 0; loop()
//  on core 1 just sleeps. Everything interesting happens in the
//  shell task body inside pm_shell.cpp.
//
//  USB-CDC enumeration on ESP32-S3 typically completes within ~1-2s
//  after reset. We wait up to 3 seconds for Serial to become true,
//  but proceed regardless so a board reflashed by a build server
//  doesn't hang at boot.
// ─────────────────────────────────────────────

#include <Arduino.h>
#include "pm_shell.h"

#ifndef PM_SHELL_TARGET
#define PM_SHELL_TARGET "unknown"
#endif

#ifndef PM_SHELL_VERSION
#define PM_SHELL_VERSION "0.0.0-dev"
#endif

void setup() {
    Serial.begin(115200);

    // Wait for USB-CDC to enumerate, but don't hang forever — a
    // board flashed and immediately power-cycled without a host
    // connection should still boot the shell so it's running for
    // the moment USB does get connected.
    uint32_t deadline = millis() + 3000;
    while (!Serial && (int32_t)(deadline - millis()) > 0) {
        delay(10);
    }
    delay(300);   // small additional settle so the banner is intact

    Serial.println();
    Serial.println();
    Serial.println("===========================================");
    Serial.println("  pm_shell - Pisces Moon Sovereign Shell");
    Serial.print  ("  v");
    Serial.print  (PM_SHELL_VERSION);
    Serial.print  (" - target: ");
    Serial.println(PM_SHELL_TARGET);
    Serial.println("===========================================");
    Serial.println();

    pm_shell_setup();
}

void loop() {
    // The shell runs in its own task. The Arduino main loop has
    // nothing to do. Sleep deeply to keep CPU usage low and let
    // the shell task have the full slice of core 1 when the user
    // is typing.
    vTaskDelay(pdMS_TO_TICKS(1000));
}
