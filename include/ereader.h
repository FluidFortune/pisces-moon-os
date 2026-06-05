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

#ifndef EREADER_H
#define EREADER_H

// PISCES MOON E-READER v1.0
//
// SD-sourced text reader. Scans /books/, /Books/, /BOOKS/ for
// .txt and .md files and presents a paginated reading view.
//
// v1 scope:
//   - Plain text and Markdown files (markdown rendered as plain
//     text; styled rendering arrives in v2)
//   - Per-book bookmarks stored at /books/.<filename>.bm as a
//     single int (byte offset). One file per book; no central
//     index to corrupt.
//   - Page-by-page navigation with a back-history stack for
//     precise reverse paging
//   - All 5 devices supported with per-device input bindings
//
// Deferred to v2 or later:
//   - Markdown style rendering (headings, bullets, quotes,
//     inline emphasis)
//   - EPUB support (needs a streaming zip unpacker + XHTML
//     stripping)
//   - PDF support (PDFs convert to .txt on the host instead;
//     see the companion pdf-to-text Python script)
//   - UTF-8 grapheme awareness (v1 is byte-oriented; ASCII
//     reads cleanly, non-ASCII characters may show as garbage
//     glyphs)

void run_ereader();

#endif // EREADER_H
