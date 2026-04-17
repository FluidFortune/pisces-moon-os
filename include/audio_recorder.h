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

#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include "hal.h"

#ifdef AUDIO_DISABLED
  // FrankenDot has no audio hardware — stub prevents linker errors
  inline void run_audio_recorder() {
    // No-op: audio hardware not present on this target
    // Launcher should hide this app when AUDIO_DISABLED is defined
  }
#else
  // T-Deck Plus: ES7210 via I2S
  // CRASH FIX: Ensure GPIO0 (TRK_CLICK, shared with ES7210) is pulled HIGH
  // before I2S init. Call init_trackball() in setup() BEFORE this app runs.
  // Stack size: allocate recorder task on internal SRAM, not PSRAM —
  // I2S DMA buffers must not land in external RAM.
  void run_audio_recorder();
#endif

#endif