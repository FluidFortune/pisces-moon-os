// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_lora_lease.h — who may drive the SX1262 right now
//
//  One radio, several drivers: the Wardrive passive receiver, Mesh
//  Messenger, and (Cardputer) the P4 tether bridge each build their own
//  RadioLib SX1262 object. Before this lease, whichever called begin()
//  last silently took the chip and its DIO1 interrupt, and the others
//  kept reporting "ready" while hearing nothing. After Mesh Messenger
//  closed, the Wardrive LoRa receiver stayed dead until reboot.
//
//  The rule (Bus Treaty, radio edition): only the lease holder touches
//  the radio. A higher-priority owner can ask a lower one to give it up;
//  the holder does that from its OWN task loop (standby, delete its
//  driver, release), so no driver is ever deleted under another task.
//
//  Priority, low to high:
//    TETHER    Cardputer P4 bridge. Background; yields to anyone.
//    WARDRIVE  Passive receiver. Background; yields to Mesh Messenger.
//    MESH      Mesh Messenger, while the user is in the app.
//
//  The SPI bus itself is still the spi_mutex; this only decides which
//  driver owns the chip between transactions.
// ─────────────────────────────────────────────

#pragma once

#include <stdint.h>

enum PmLoraOwner : uint8_t {
    PM_LORA_NONE     = 0,
    PM_LORA_TETHER   = 1,
    PM_LORA_WARDRIVE = 2,
    PM_LORA_MESH     = 3,
};

// Non-blocking. Takes the radio if it is free and no higher owner is
// waiting. If a LOWER owner holds it, records that `who` wants it (the
// holder sees pm_lora_lease_should_yield) and returns false; call again
// later. Returns true if `who` already holds it.
bool pm_lora_lease_request(PmLoraOwner who);

// Blocking form of request() for foreground apps: waits up to timeout_ms
// for a lower holder to yield. On timeout the request is withdrawn.
bool pm_lora_lease_take(PmLoraOwner who, uint32_t timeout_ms);

// Give the radio back (if held) and withdraw any pending request.
void pm_lora_lease_release(PmLoraOwner who);

// Polled by the holder from its own loop: true when a higher owner is
// waiting. Put the radio in standby, delete the driver, then release.
bool pm_lora_lease_should_yield(PmLoraOwner who);

PmLoraOwner pm_lora_lease_holder();
const char* pm_lora_owner_name(PmLoraOwner who);
