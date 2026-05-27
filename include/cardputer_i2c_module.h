// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com

#ifndef CARDPUTER_I2C_MODULE_H
#define CARDPUTER_I2C_MODULE_H

#include <Arduino.h>

#ifdef DEVICE_CARDPUTER_ADV
void cardputer_i2c_module_begin();
void cardputer_i2c_module_offer_key(char c);
#else
static inline void cardputer_i2c_module_begin() {}
static inline void cardputer_i2c_module_offer_key(char c) { (void)c; }
#endif

#endif // CARDPUTER_I2C_MODULE_H
