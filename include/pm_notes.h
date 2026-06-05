// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  pm_notes.h — Quick notes app for C28P + Maxine
//
//  Touch-kiosk notes app. Stores text notes on SD via NoSQL
//  (category "notes"). One screen lists existing notes; tap a
//  note to read, tap NEW to create one. Editing reopens the
//  soft keyboard preseeded with the current content.
//
//  Each note has a one-line title (first line of body, derived)
//  and a free-form body. Body is limited to 1024 chars to match
//  the soft keyboard's typical buffer size; longer notes can be
//  broken into multiple entries.
//
//  Persistence backend: nosql_store "notes" category. Title is
//  derived from the body's first line so the list view always
//  shows something meaningful without a separate title field.
//
//  C28P (240×320) and Maxine (480×800) get tuned layouts off
//  the same geometry block. T-Deck Plus, Pager, and Cardputer
//  continue to use notepad.cpp's keyboard-driven UI; this file
//  is gated to the two touch kiosks.
// ─────────────────────────────────────────────

#pragma once

void pm_run_notes();   // touch-kiosk entry point
