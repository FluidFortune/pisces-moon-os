// Pisces Moon OS
// Copyright (C) 2026 Eric Becker / Fluid Fortune
// SPDX-License-Identifier: AGPL-3.0-or-later
// fluidfortune.com
//
// ─────────────────────────────────────────────
//  c28p_weather.cpp — RETIRED 2026-05-28
//
//  This file is DEAD. The C28P weather app is now provided by the
//  multi-device weather.cpp, which exposes run_weather() and a
//  c28p_run_weather() backward-compat alias consumed by
//  c28p_boot.cpp's dispatch table.
//
//  The old C28P-only implementation that lived here defined a
//  CONFLICTING c28p_run_weather() symbol. It has been emptied to a
//  tombstone so that:
//    1. It is NOT in any build_src_filter (C28P uses weather.cpp).
//    2. If anyone re-adds it to a filter by mistake, it compiles to
//       nothing instead of producing a duplicate-symbol link error.
//
//  Safe to delete entirely (git rm src/c28p_weather.cpp). Kept as a
//  tombstone only because this session's tooling could not perform
//  the file move/delete directly. Full prior contents are in git
//  history (last real version: 2026-05-24).
// ─────────────────────────────────────────────

// Intentionally empty. See header above.
