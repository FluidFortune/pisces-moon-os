#!/usr/bin/env python3
# Pisces Moon OS — pdf_to_text.py
# Copyright (C) 2026 Eric Becker / Fluid Fortune
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ─────────────────────────────────────────────
#  pdf_to_text.py — host-side PDF → .txt prepper for the e-Reader
#
#  The Pisces Moon e-Reader reads plain .txt and .md from the SD
#  card's /books/ folder. That's a deliberate choice: parsing PDF
#  on ESP32 would cost more flash, more PSRAM, and more battery
#  than the format deserves. This tool runs ONCE on the host
#  (Mac/Linux/Windows), converts a PDF (or a folder of PDFs) into
#  e-Reader-ready .txt files, and the resulting files drop into
#  the SD card's /books/ folder ready to read.
#
#  EXTRACTION STRATEGY (three tiers, fall-through)
#
#    Tier 1 — PyMuPDF (fitz)
#        Fast, accurate on most modern PDFs. Handles ligatures,
#        column layouts, embedded fonts. First choice.
#
#    Tier 2 — pdfminer.six
#        Slower, more permissive on weird/old PDFs that PyMuPDF
#        chokes on. Better at preserving reading order for some
#        layouts. Fallback if Tier 1 returns suspiciously little.
#
#    Tier 3 — Tesseract OCR (via pdf2image + pytesseract)
#        For scan-only PDFs (no embedded text layer). Last resort
#        because it's slow and accuracy depends on scan quality.
#        Triggered when Tiers 1+2 average < OCR_THRESHOLD chars
#        per page (default 50).
#
#  PARAGRAPH PRESERVATION
#
#  e-Reader on the device does its own word-wrap based on the
#  display width. So this tool DELIBERATELY does not hard-wrap
#  output to 80 columns or anything like that. Hard-wraps would
#  freeze the line breaks at this tool's idea of column width
#  and look broken on every device the e-Reader runs on.
#
#  Instead: paragraphs are preserved as long lines, separated
#  by single blank lines. The e-Reader's paginator will reflow
#  them to whatever the device's column width is.
#
#  FAT32-SAFE FILENAMES
#
#  SD cards on the supported Pisces Moon devices are FAT32 (or
#  exFAT). Filenames must avoid: \\ / : * ? " < > | and trailing
#  spaces/dots. This tool sanitizes output filenames so they
#  drop into /books/ without rename gymnastics.
#
#  USAGE
#
#    Single file:
#        python pdf_to_text.py book.pdf
#        → book.txt in the same folder
#
#    Folder (recursive):
#        python pdf_to_text.py ~/Library/PDFs/ -o ~/sdcard/books/
#
#    Force OCR (skip Tiers 1+2):
#        python pdf_to_text.py scan.pdf --mode ocr-only
#
#    Skip OCR entirely (text-PDFs only):
#        python pdf_to_text.py book.pdf --mode no-ocr
#
#    Tune the OCR fallback threshold:
#        python pdf_to_text.py book.pdf --ocr-threshold 100
#
#    Force re-conversion of files that already have a .txt:
#        python pdf_to_text.py ~/PDFs/ -o ~/books/ --force
#
#  DEPENDENCIES
#
#    Required:
#        pip install pymupdf pdfminer.six
#
#    For OCR (optional, only needed for scan-PDFs):
#        pip install pytesseract pdf2image Pillow
#        + system: tesseract, poppler-utils
#            macOS:   brew install tesseract poppler
#            Ubuntu:  sudo apt install tesseract-ocr poppler-utils
#            Windows: scoop install tesseract; download poppler
#
#  The OCR dependencies are optional. If they're not installed,
#  Tiers 1+2 still work and the tool just refuses to OCR (with
#  a clear message) when it would otherwise need to.
# ─────────────────────────────────────────────

import argparse
import os
import re
import sys
from pathlib import Path

# ── Tier 1: PyMuPDF ──
try:
    import fitz  # PyMuPDF
    HAS_PYMUPDF = True
except ImportError:
    HAS_PYMUPDF = False

# ── Tier 2: pdfminer.six ──
try:
    from pdfminer.high_level import extract_text as pdfminer_extract
    HAS_PDFMINER = True
except ImportError:
    HAS_PDFMINER = False

# ── Tier 3: Tesseract OCR via pdf2image ──
try:
    import pytesseract
    from pdf2image import convert_from_path
    HAS_OCR = True
except ImportError:
    HAS_OCR = False


# ─────────────────────────────────────────────
#  FILENAME SANITIZATION
# ─────────────────────────────────────────────
# FAT32 disallowed characters + trailing whitespace/dots. We also
# strip control characters and collapse runs of whitespace.
_FAT32_BAD = re.compile(r'[\\/:*?"<>|\x00-\x1f]')

def sanitize_filename(stem: str) -> str:
    """Return a FAT32-safe filename stem (without extension)."""
    # Replace bad chars with spaces, collapse whitespace, trim.
    clean = _FAT32_BAD.sub(' ', stem)
    clean = re.sub(r'\s+', ' ', clean).strip()
    # FAT32 doesn't like trailing dots either
    clean = clean.rstrip('.')
    # Keep under 200 chars (SD readers stumble on extremely long names)
    if len(clean) > 200:
        clean = clean[:200].rstrip()
    return clean or 'untitled'


# ─────────────────────────────────────────────
#  PARAGRAPH NORMALIZATION
# ─────────────────────────────────────────────
def normalize_paragraphs(text: str) -> str:
    """
    Collapse single newlines (mid-paragraph wraps from the PDF
    extractor) into spaces, preserve double newlines as paragraph
    breaks. Strip per-page form-feed (\\x0c) artifacts. The
    e-Reader's paginator will re-wrap based on screen width.
    """
    if not text:
        return ""

    # Drop form-feed page separators
    text = text.replace('\x0c', '\n\n')

    # Normalize line endings
    text = text.replace('\r\n', '\n').replace('\r', '\n')

    # Collapse 3+ blank lines down to a single paragraph break
    text = re.sub(r'\n{3,}', '\n\n', text)

    # Within paragraphs: join lines that aren't separated by a
    # blank line. Use a two-pass approach so we don't accidentally
    # merge paragraphs.
    paragraphs = text.split('\n\n')
    out_paragraphs = []
    for p in paragraphs:
        # Join intra-paragraph wraps. Preserve indentation in
        # likely-code blocks (lines starting with 4+ spaces).
        lines = p.split('\n')
        if any(re.match(r'^( {4,}|\t)', ln) for ln in lines):
            # Looks like a code/preformatted block — keep linebreaks
            joined = '\n'.join(ln.rstrip() for ln in lines)
        else:
            joined = ' '.join(ln.strip() for ln in lines if ln.strip())
        if joined:
            out_paragraphs.append(joined)

    return '\n\n'.join(out_paragraphs) + '\n'


# ─────────────────────────────────────────────
#  TIER 1 — PyMuPDF
# ─────────────────────────────────────────────
def extract_pymupdf(pdf_path: Path, verbose: bool = False) -> str:
    if not HAS_PYMUPDF:
        return ""
    try:
        doc = fitz.open(str(pdf_path))
    except Exception as e:
        if verbose:
            print(f"  [tier1] PyMuPDF open failed: {e}", file=sys.stderr)
        return ""
    pages = []
    for i, page in enumerate(doc):
        try:
            pages.append(page.get_text("text"))
        except Exception as e:
            if verbose:
                print(f"  [tier1] page {i+1} extract failed: {e}",
                      file=sys.stderr)
    doc.close()
    return '\n\n'.join(pages)


# ─────────────────────────────────────────────
#  TIER 2 — pdfminer.six
# ─────────────────────────────────────────────
def extract_pdfminer(pdf_path: Path, verbose: bool = False) -> str:
    if not HAS_PDFMINER:
        return ""
    try:
        return pdfminer_extract(str(pdf_path)) or ""
    except Exception as e:
        if verbose:
            print(f"  [tier2] pdfminer failed: {e}", file=sys.stderr)
        return ""


# ─────────────────────────────────────────────
#  TIER 3 — Tesseract OCR
# ─────────────────────────────────────────────
def extract_ocr(pdf_path: Path, verbose: bool = False) -> str:
    if not HAS_OCR:
        return ""
    try:
        # 200 DPI is a reasonable speed/accuracy compromise. Higher
        # DPI improves accuracy on small fonts but multiplies time.
        images = convert_from_path(str(pdf_path), dpi=200)
    except Exception as e:
        if verbose:
            print(f"  [tier3] pdf2image failed: {e}", file=sys.stderr)
        return ""

    pages = []
    for i, img in enumerate(images):
        if verbose:
            print(f"  [tier3] OCRing page {i+1}/{len(images)}...",
                  file=sys.stderr)
        try:
            pages.append(pytesseract.image_to_string(img))
        except Exception as e:
            if verbose:
                print(f"  [tier3] OCR page {i+1} failed: {e}",
                      file=sys.stderr)
    return '\n\n'.join(pages)


# ─────────────────────────────────────────────
#  PAGE-COUNT HELPER (for OCR-threshold decision)
# ─────────────────────────────────────────────
def count_pages(pdf_path: Path) -> int:
    if HAS_PYMUPDF:
        try:
            doc = fitz.open(str(pdf_path))
            n = doc.page_count
            doc.close()
            return n
        except Exception:
            pass
    if HAS_PDFMINER:
        try:
            from pdfminer.pdfparser import PDFParser
            from pdfminer.pdfdocument import PDFDocument
            from pdfminer.pdfpage import PDFPage
            with open(pdf_path, 'rb') as f:
                parser = PDFParser(f)
                doc = PDFDocument(parser)
                return sum(1 for _ in PDFPage.create_pages(doc))
        except Exception:
            pass
    return 1   # avoid divide-by-zero; threshold check stays meaningful


# ─────────────────────────────────────────────
#  CONVERT ONE FILE
# ─────────────────────────────────────────────
def convert_pdf(pdf_path: Path, out_path: Path,
                mode: str = 'auto',
                ocr_threshold: int = 50,
                verbose: bool = False) -> bool:
    """
    Convert one PDF to .txt. Returns True on success.
    mode ∈ {'auto', 'no-ocr', 'ocr-only'}
    """
    if verbose:
        print(f"[convert] {pdf_path.name}")

    text = ""

    if mode == 'ocr-only':
        if not HAS_OCR:
            print(f"  ERROR: --mode ocr-only requested but pytesseract "
                  f"or pdf2image not installed.", file=sys.stderr)
            return False
        text = extract_ocr(pdf_path, verbose=verbose)
    else:
        # Tier 1 first
        text = extract_pymupdf(pdf_path, verbose=verbose)
        if verbose:
            print(f"  [tier1] PyMuPDF: {len(text)} chars")

        # Tier 2 if Tier 1 returned nothing
        if not text.strip():
            text = extract_pdfminer(pdf_path, verbose=verbose)
            if verbose:
                print(f"  [tier2] pdfminer: {len(text)} chars")

        # Tier 3 (OCR) if combined result is too thin and mode allows
        if mode == 'auto':
            page_count = count_pages(pdf_path)
            chars_per_page = len(text) / max(1, page_count)
            if chars_per_page < ocr_threshold:
                if verbose:
                    print(f"  [tier?] only {chars_per_page:.0f} chars/page "
                          f"(threshold {ocr_threshold}) — trying OCR")
                if not HAS_OCR:
                    print(f"  WARN: {pdf_path.name} appears to be scan-only "
                          f"({chars_per_page:.0f} chars/page) but OCR "
                          f"dependencies aren't installed. Output will be "
                          f"sparse. Install pytesseract + pdf2image to enable "
                          f"OCR fallback.", file=sys.stderr)
                else:
                    ocr_text = extract_ocr(pdf_path, verbose=verbose)
                    # Use whichever produced more output
                    if len(ocr_text) > len(text):
                        text = ocr_text

    if not text.strip():
        print(f"  FAIL: no text extracted from {pdf_path.name}",
              file=sys.stderr)
        return False

    # Paragraph normalization + final write
    normalized = normalize_paragraphs(text)
    try:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(normalized, encoding='utf-8')
    except Exception as e:
        print(f"  FAIL: write {out_path}: {e}", file=sys.stderr)
        return False

    if verbose:
        print(f"  OK: {out_path} ({len(normalized)} chars)")
    return True


# ─────────────────────────────────────────────
#  CLI
# ─────────────────────────────────────────────
def main():
    p = argparse.ArgumentParser(
        description="Convert PDF(s) to plain text for the Pisces Moon "
                    "e-Reader.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('input',
                   help="Input PDF file or directory of PDFs (recursive).")
    p.add_argument('-o', '--output',
                   help="Output directory (defaults to same as input).")
    p.add_argument('--mode', choices=['auto', 'no-ocr', 'ocr-only'],
                   default='auto',
                   help="Extraction strategy. auto = try text first, "
                        "OCR if too sparse; no-ocr = never OCR; ocr-only = "
                        "always OCR. Default: auto.")
    p.add_argument('--ocr-threshold', type=int, default=50,
                   help="Chars-per-page below which auto-mode falls back "
                        "to OCR. Default: 50.")
    p.add_argument('--force', action='store_true',
                   help="Overwrite existing .txt outputs.")
    p.add_argument('-v', '--verbose', action='store_true',
                   help="Verbose progress output.")
    args = p.parse_args()

    in_path = Path(args.input).expanduser().resolve()
    if not in_path.exists():
        print(f"ERROR: input not found: {in_path}", file=sys.stderr)
        sys.exit(1)

    # Report dependency status up front so the user knows what they're
    # working with before any conversions run.
    if not HAS_PYMUPDF and not HAS_PDFMINER:
        print("ERROR: neither PyMuPDF nor pdfminer.six is installed. "
              "Install at least one:\n  pip install pymupdf pdfminer.six",
              file=sys.stderr)
        sys.exit(1)
    if args.verbose:
        print(f"[deps] PyMuPDF: {'yes' if HAS_PYMUPDF else 'no'}, "
              f"pdfminer: {'yes' if HAS_PDFMINER else 'no'}, "
              f"OCR: {'yes' if HAS_OCR else 'no'}")

    # Build list of (input_pdf, output_txt) pairs
    out_root = Path(args.output).expanduser().resolve() if args.output else None
    jobs = []

    if in_path.is_file():
        if in_path.suffix.lower() != '.pdf':
            print(f"ERROR: not a PDF: {in_path}", file=sys.stderr)
            sys.exit(1)
        out_dir = out_root if out_root else in_path.parent
        out_file = out_dir / (sanitize_filename(in_path.stem) + '.txt')
        jobs.append((in_path, out_file))
    else:
        # Directory walk
        pdfs = sorted(in_path.rglob('*.pdf'))
        if not pdfs:
            print(f"ERROR: no .pdf files found under {in_path}",
                  file=sys.stderr)
            sys.exit(1)
        for pdf in pdfs:
            if out_root:
                # Preserve relative subdirectory structure
                rel = pdf.parent.relative_to(in_path)
                out_dir = out_root / rel
            else:
                out_dir = pdf.parent
            out_file = out_dir / (sanitize_filename(pdf.stem) + '.txt')
            jobs.append((pdf, out_file))

    if args.verbose:
        print(f"[plan] {len(jobs)} file(s) to convert")

    ok = 0
    skipped = 0
    failed = 0
    for pdf_path, out_path in jobs:
        if out_path.exists() and not args.force:
            if args.verbose:
                print(f"[skip] {out_path.name} already exists "
                      f"(use --force to overwrite)")
            skipped += 1
            continue
        try:
            if convert_pdf(pdf_path, out_path,
                           mode=args.mode,
                           ocr_threshold=args.ocr_threshold,
                           verbose=args.verbose):
                ok += 1
            else:
                failed += 1
        except Exception as e:
            print(f"  FAIL: {pdf_path.name}: {e}", file=sys.stderr)
            failed += 1

    print(f"[done] {ok} converted, {skipped} skipped, {failed} failed")
    sys.exit(0 if failed == 0 else 2)


if __name__ == '__main__':
    main()
