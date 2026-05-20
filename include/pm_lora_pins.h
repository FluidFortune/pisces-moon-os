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

#ifndef PM_LORA_PINS_H
#define PM_LORA_PINS_H

#if defined(PIN_LORA_CS) && defined(PIN_LORA_RST) && \
    defined(PIN_LORA_IRQ) && defined(PIN_LORA_BUSY)
  #define PM_LORA_CS    PIN_LORA_CS
  #define PM_LORA_RST   PIN_LORA_RST
  #define PM_LORA_IRQ   PIN_LORA_IRQ
  #define PM_LORA_BUSY  PIN_LORA_BUSY
#elif defined(DEVICE_TDECK_PLUS)
  #define PM_LORA_CS    9
  #define PM_LORA_RST   17
  #define PM_LORA_IRQ   45
  #define PM_LORA_BUSY  13
#else
  #error "No LoRa pin map for this device"
#endif

#endif // PM_LORA_PINS_H
