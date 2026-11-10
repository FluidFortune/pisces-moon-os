// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// pm_lora_lease.cpp — see pm_lora_lease.h

#include "pm_lora_lease.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static portMUX_TYPE     s_mux    = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t s_holder = PM_LORA_NONE;
static volatile uint8_t s_wanted = PM_LORA_NONE;   // highest owner waiting

bool pm_lora_lease_request(PmLoraOwner who) {
    if (who == PM_LORA_NONE) return false;
    bool got = false;
    portENTER_CRITICAL(&s_mux);
    if (s_holder == who) {
        got = true;
    } else if (s_wanted > who) {
        // Someone more important is waiting; don't take it from under them.
    } else if (s_holder == PM_LORA_NONE) {
        s_holder = who;
        if (s_wanted == who) s_wanted = PM_LORA_NONE;
        got = true;
    } else if (s_holder < who) {
        s_wanted = who;               // ask the holder to yield
    }
    // else: held by an equal or higher owner; nothing to do.
    portEXIT_CRITICAL(&s_mux);
    return got;
}

bool pm_lora_lease_take(PmLoraOwner who, uint32_t timeout_ms) {
    uint32_t start = millis();
    for (;;) {
        if (pm_lora_lease_request(who)) return true;
        if (millis() - start >= timeout_ms) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    portENTER_CRITICAL(&s_mux);
    if (s_wanted == who) s_wanted = PM_LORA_NONE;
    portEXIT_CRITICAL(&s_mux);
    return false;
}

void pm_lora_lease_release(PmLoraOwner who) {
    portENTER_CRITICAL(&s_mux);
    if (s_holder == who) s_holder = PM_LORA_NONE;
    if (s_wanted == who) s_wanted = PM_LORA_NONE;
    portEXIT_CRITICAL(&s_mux);
}

bool pm_lora_lease_should_yield(PmLoraOwner who) {
    // Single byte reads; no lock needed for a poll.
    return s_holder == who && s_wanted > who;
}

PmLoraOwner pm_lora_lease_holder() {
    return (PmLoraOwner)s_holder;
}

const char* pm_lora_owner_name(PmLoraOwner who) {
    switch (who) {
        case PM_LORA_TETHER:   return "tether";
        case PM_LORA_WARDRIVE: return "wardrive";
        case PM_LORA_MESH:     return "mesh";
        default:               return "none";
    }
}
