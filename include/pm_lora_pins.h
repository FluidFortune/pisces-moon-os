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
#elif defined(DEVICE_TEMBED_CC1101)
  // The T-Embed's sub-GHz radio is a CC1101 (OOK), not an SX1262. These
  // SX1262-style macros exist only so the shared LoRa files (mesh_messenger,
  // lora_voice) still compile here; those code paths are never entered on
  // this device -- no Mesh/LoRa apps in the T-Embed launcher. The real
  // CC1101 pins live in hal_pins.h (CC1101_CS / CC1101_GDO0 / ...).
  #define PM_LORA_CS    -1
  #define PM_LORA_RST   -1
  #define PM_LORA_IRQ   -1
  #define PM_LORA_BUSY  -1
#else
  #error "No LoRa pin map for this device"
#endif

// SX1262 TCXO supply (DIO3) and regulator, passed as the last two
// arguments of SX1262::begin(). M5Stack's Cap LoRa868 / Cap LoRa-1262
// reference code uses radio.begin(..., 3.0, true): a 3.0 V TCXO and the
// LDO regulator. RadioLib's defaults (1.6 V, DC-DC) under-drive that
// TCXO, which can fail begin() or leave the receiver deaf / off
// frequency. Other boards keep RadioLib's defaults, unchanged.
#if defined(DEVICE_CARDPUTER_ADV)
  #define PM_LORA_TCXO_V   3.0f
  #define PM_LORA_USE_LDO  true
#else
  #define PM_LORA_TCXO_V   1.6f
  #define PM_LORA_USE_LDO  false
#endif

#endif // PM_LORA_PINS_H
