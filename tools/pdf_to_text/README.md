# `pdf_to_text` — Host-side PDF → text converter for the e-Reader

Pisces Moon's on-device e-Reader reads plain `.txt` and `.md` files from the
SD card's `/books/` folder. This host-side tool turns PDFs into e-Reader-ready
text files so you can drop them onto the SD card and start reading.

Runs on macOS, Linux, and Windows. Not part of the firmware — pure host
tooling.

## Why a host tool and not on-device

Parsing PDF on an ESP32-S3 would cost more flash, more PSRAM, and more
battery than the format warrants. PDFs have absolute positioning, embedded
fonts, ligatures, and a 1000-page spec full of edge cases. Doing the
extraction once on a Mac/PC and shipping plain text to the device is a much
better trade — the e-Reader stays small, fast, and reflows freely to
whichever device it's running on.

## What it does

Three-tier extraction, falling through as needed:

1. **PyMuPDF** (Tier 1) — fast and accurate on most modern PDFs. First
   choice.
2. **pdfminer.six** (Tier 2) — used when PyMuPDF returns nothing. Handles
   some weird/old PDFs that PyMuPDF can't parse.
3. **Tesseract OCR** (Tier 3) — kicks in when Tiers 1+2 produce
   suspiciously little text (default: under 50 chars per page on average,
   meaning the PDF is probably a scanned image with no embedded text
   layer). Optional dependency; if it's not installed, the tool warns and
   produces whatever text the first two tiers found.

Output:

- Paragraphs preserved as long lines separated by blank lines. **No
  hard-wrapping** — the e-Reader's paginator reflows based on device
  display width, and pre-wrapped lines would look broken at every screen
  size.
- Filenames sanitized for FAT32 (no `\ / : * ? " < > |`, no trailing dots
  or spaces, capped at 200 chars).
- UTF-8 encoding.

## Install

### Python packages

```bash
cd tools/pdf_to_text
pip install -r requirements.txt
```

The text-extraction packages (PyMuPDF, pdfminer.six) are required. The OCR
packages (pytesseract, pdf2image, Pillow) are optional — only needed if you
have scan-only PDFs.

### System binaries (only if you want OCR)

OCR also needs `tesseract` and `poppler` installed at the OS level so
`pytesseract` and `pdf2image` have something to call into.

**macOS** (Homebrew):
```bash
brew install tesseract poppler
```

**Ubuntu / Debian**:
```bash
sudo apt install tesseract-ocr poppler-utils
```

**Windows**:
- [Tesseract installer](https://github.com/UB-Mannheim/tesseract/wiki) (the
  UB Mannheim build is the maintained Windows one)
- [Poppler for Windows](https://github.com/oschwartz10612/poppler-windows/releases)
  — extract and add `bin/` to your `PATH`
- Or with Scoop: `scoop install tesseract poppler`

If you skip the OCR install, the tool runs Tiers 1+2 fine and only complains
when a scan-PDF would have needed OCR.

## Usage

### Convert one file

```bash
python pdf_to_text.py book.pdf
```

Produces `book.txt` next to the PDF.

### Convert a folder of PDFs into your SD card's `/books/` folder

```bash
python pdf_to_text.py ~/Library/PDFs/ -o /Volumes/PISCES/books/
```

Walks the input directory recursively, preserves subfolder structure in
the output, and skips any PDF that already has a `.txt` next to it (use
`--force` to re-convert).

### Force OCR (for scan PDFs you know need it)

```bash
python pdf_to_text.py scan.pdf --mode ocr-only
```

Skips Tiers 1+2 entirely. Slow but works on scanned books that have no
embedded text layer.

### Skip OCR (text-only PDFs)

```bash
python pdf_to_text.py book.pdf --mode no-ocr
```

If a text PDF mysteriously produces no output, this mode fails loudly
instead of falling through to slow OCR.

### Tune the OCR-fallback threshold

```bash
python pdf_to_text.py book.pdf --ocr-threshold 100
```

Default is 50 chars/page. Bump it up if you have heavily-watermarked or
table-heavy PDFs that produce some text but still benefit from OCR. Drop it
to 10 for PDFs where you trust Tier 1+2 output even when it's sparse.

### Re-convert everything

```bash
python pdf_to_text.py ~/PDFs/ -o ~/books/ --force
```

Overwrites existing `.txt` files. Useful after upgrading dependencies that
might improve extraction quality.

### Verbose mode

```bash
python pdf_to_text.py book.pdf -v
```

Reports which tier produced output, how many characters per page,
fallback decisions, and per-file outcomes. Use this when something
unexpected happens.

## Workflow with the e-Reader

1. Run the converter against your PDF library
2. Mount your Pisces Moon SD card on the host
3. Copy/move the resulting `.txt` files into `/books/` on the card
4. Eject and reinsert the card in the device
5. Open the e-Reader app — your books appear in the picker
6. Bookmarks save automatically per book; the e-Reader will pick up
   where you left off on the next open

You can also keep `.md` files in `/books/` — the e-Reader treats them the
same as `.txt` (basic Markdown styling is a future enhancement; v1 reads
them as plain text).

## Troubleshooting

**"FAIL: no text extracted"**
The PDF is probably a scan and either OCR isn't installed or `--mode no-ocr`
was passed. Run `pip install pytesseract pdf2image Pillow`, install
tesseract + poppler at the OS level, and try again.

**Output looks like one giant blob with no paragraph breaks**
Some PDFs encode paragraphs without the usual blank-line markers. The tool
falls back to single-newline-between-lines in that case, which the e-Reader
will reflow as one paragraph per chunk. Acceptable for reading but not
ideal. Future versions of this tool may add heuristic paragraph detection.

**Output has weird ligature characters or missing letters**
Usually a Tier 1 (PyMuPDF) artifact with old PDFs. Pass `--mode no-ocr` on
the offending file with `-v` to see which tier ran; sometimes manually
running Tier 2 by uninstalling PyMuPDF temporarily gives better results
on specific stubborn PDFs.

**Filename has weird characters that won't show up in `/books/`**
Should be impossible — the sanitizer strips FAT32-disallowed characters
before writing. If you hit this, please file an issue with the offending
PDF filename so we can extend the sanitizer rules.

## Limitations

- Page numbers, headers, footers are not stripped. Some PDFs include them
  in the text layer and they'll show up in your e-Reader output as
  recurring noise. Manual editing (or `sed` against the output `.txt`) is
  currently the only fix.
- Footnote placement is whatever the source PDF put in the text stream —
  usually right where they appeared on the page. Reading order is
  preserved but footnotes mid-paragraph can be jarring.
- Tables are flattened to whatever the extractor reports, which is
  typically not pretty. PDFs are a bad format for tabular data on small
  screens; if a book is heavily tabular, consider converting just the
  prose chapters and skipping the data sections.
- DRM'd PDFs (Adobe DRM, Apple Books exports with DRM) won't open. Strip
  DRM with another tool first if you legally own the book and have a
  legitimate need.

## License

Same as Pisces Moon OS: AGPL-3.0-or-later. See `LICENSE` at the project
root.
