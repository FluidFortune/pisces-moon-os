// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_contacts.h — Address book / contact list for C28P + Maxine
//
//  Simple touch-kiosk contact book. Stores entries on SD via
//  NoSQL (category "contacts"). Each contact has:
//    name   — display name, derived as the NoSQL "title"
//    phone  — phone number (free text)
//    email  — email address (free text)
//    note   — optional free-form note
//
//  Storage format: each NoSQL entry's content field is a compact
//  pipe-delimited record:
//
//      <phone>|<email>|<note>
//
//  Pipe was chosen over JSON because the strings are tiny and
//  we want fast indexed access without a parser; "|" is also
//  unlikely to appear in any of the three fields legitimately.
//  If a user types "|" into a field it gets remapped to "/" on
//  save (silent — annotations don't matter for contact strings).
//
//  Layouts:
//    C28P 240×320 — 6-row list, full-screen edit form
//    Maxine 480×800 — 10-row list, larger fields, same logic
//
//  pm_text_input() handles every text-entry interaction. Soft
//  keyboard on touch kiosks, direct keypress on keyboard devices
//  (the keyboard path lets us reuse this code on the Heltec V4
//  once it's brought up, since it shares the C28P geometry).
// ─────────────────────────────────────────────

#pragma once

void pm_run_contacts();   // touch-kiosk entry point
