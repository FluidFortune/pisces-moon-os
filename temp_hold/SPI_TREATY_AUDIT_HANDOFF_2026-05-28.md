# SPI Bus Treaty Audit — Handoff Log

**Date:** 2026-05-28 (UTC)
**From:** Opus instance, fresh session, working from Eric's MacBook with read/write to `/Users/eric/Documents/GitHub`
**To:** The Pisces Moon working instance (recent handoff)
**Subject:** Possible mismatch between Bible v1.2.0 ship record and current state of `nosql_store.cpp`. Pause requested before any audit work is performed by this instance.

---

## TL;DR

I was asked to begin the v1.3 SPI Bus Treaty audit on the ~23 remaining unwrapped `sd.open()` files named in the v1.2.0 ship record. Before touching anything I read `nosql_store.cpp` to confirm the canonical wrapped pattern. **The file is not wrapped.** The Bible's v1.2.0 changelog (Chapter 91A) explicitly claims it is. Eric paused the work and asked me to hand off what I observed so the Pisces Moon instance — which is closer to the actual ship work — can reconcile it against its own notes.

This document is the receipt. No code was modified. Nothing was committed. Only reads.

---

## What I Observed

### 1. Canonical Treaty macros — confirmed location and signature

`/Users/eric/Documents/GitHub/pisces-moon-os/include/spi_treaty.h` defines the canonical Treaty interface:

```c
#define PM_SPI_TAKE(who) \
    (_TREATY_LOG(who, "TAKE"), \
     spi_mutex \
         ? (xSemaphoreTakeRecursive(spi_mutex, \
                pdMS_TO_TICKS(TREATY_TIMEOUT_MS)) == pdTRUE) \
         : true)

#define PM_SPI_GIVE() \
    (_TREATY_LOG("", "GIVE"), \
     (void)(spi_mutex ? xSemaphoreGiveRecursive(spi_mutex) : pdFALSE))
```

- Recursive mutex (`xSemaphoreTakeRecursive` / `xSemaphoreGiveRecursive`)
- 500ms timeout (`TREATY_TIMEOUT_MS`)
- Falls through to `true` if `spi_mutex` is null (graceful in early boot)
- `_TREATY_LOG` expands to `((void)0)` in release builds (`TREATY_DEBUG=0`)
- `spi_mutex` is declared `extern SemaphoreHandle_t` here, defined in `wardrive.cpp`
- Companion ISR variants exist: `PM_SPI_TAKE_ISR(pxWoken)` / `PM_SPI_GIVE_ISR(pxWoken)`

The header is comprehensive and well-commented. Treaty participant list is in the file header comment:
- T-Deck Plus: LoRa + SD (2)
- T-LoRa Pager: LCD + LoRa + SD + NFC (4)
- Cardputer ADV: LCD + LoRa + SD when ready (3)
- C28P: LCD + SDIO (2)

### 2. `nosql_store.cpp` — actual state

Read in full (lines 1–250 covering the entry points the Bible names). Findings:

- **Zero occurrences** of `PM_SPI_TAKE`, `PM_SPI_GIVE`, `spi_mutex`, `_take`, `_give`, `xSemaphoreTake`, or any other mutex-related identifier
- **Zero `#include` of `spi_treaty.h`**
- Header `include/nosql_store.h` also has no Treaty references
- Every `sd.open(...)` call is bare. Counted from the file: at least 6 distinct `sd.open()` sites across the public functions audited (`nosql_init`, `nosql_get_count`, `nosql_save_entry`, `nosql_get_entry`, `nosql_search`)
- File header SPDX is correct (AGPL-3.0-or-later) and dated 2026

### 3. The Bible's claim (v1.2.0 ship record, Chapter 91A)

The Bible says, verbatim:

> "`src/nosql_store.cpp` — all five public functions wrapped with `_nm_take()`/`_nm_give()`. `src/database.cpp` — both public functions wrapped with `_db_take()`/`_db_give()`. `src/elf_loader.cpp` — manifest read, directory scan, and ELF file open wrapped."

Two issues:

1. **The naming convention `_nm_take()` / `_nm_give()` does not appear anywhere in `nosql_store.cpp` or `nosql_store.h`.** If those were intended as file-local static inline wrappers around `PM_SPI_TAKE`/`PM_SPI_GIVE`, they're not present in either file.

2. **No mutex calls of any kind are present.** Not just the named convention — the calls themselves are absent.

I did NOT verify `database.cpp` or `elf_loader.cpp` against their claims. The Pisces Moon instance should check those as part of the reconciliation.

### 4. What I did NOT do

- Did not modify `nosql_store.cpp` or any other source file
- Did not check `wardrive.cpp` v2.7 mutex pattern (Bible says it's the reference; only confirmed the header comment versioning, not the actual SD-open sites)
- Did not check `wardrive_inspect.cpp` (Bible says this is Treaty-compliant; confirmed it exists in `src/` but did not open it)
- Did not check `database.cpp` or `elf_loader.cpp` (Bible names them as v1.2.0-wrapped)
- Did not run `git log` or `git blame` to check whether these files were ever wrapped and then reverted
- Did not start the audit on any of the ~23 named-as-unwrapped files

---

## Possible Explanations

In rough order of likelihood as I see it from outside the ship work:

1. **Stale Bible claim.** The v1.2.0 ship record was written in advance of the actual completion of the wrapping work. The work was planned, documented as done, then deferred or partially completed, and the Bible was never reconciled to the actual ship.

2. **Reverted work.** The wrapping was done at some point, broke something (recursive mutex deadlock? compile error? behavioral regression?), and was reverted before the v1.2.1 tag. The Bible reflects the intent but not the revert.

3. **Different file in a branch.** A `nosql-treaty-audit` branch (or similar) has the wrapped version; main does not. The Bible is describing branch state that was never merged.

4. **Different canonical macro than I expected.** It's possible `_nm_take()` / `_nm_give()` are NOT wrappers around `PM_SPI_TAKE`/`PM_SPI_GIVE` but a separate, lighter discipline (e.g., a flag-based "I'm using SD" hint to the wardrive task, similar to `wifi_in_use` or `sd_in_use`). If so, they should still exist somewhere — I just didn't find them. Worth a search of the wider codebase.

5. **The audit work was real but lives in C28P-specific paths.** The C28P branch is newer; possibly the wrapping happened only on `c28p_*` files and not on the shared `nosql_store.cpp`. The Bible chapter would still be wrong but the work would have happened — just not where the chapter says.

6. **Bible self-report drift.** Same failure mode the Bible itself documents in other contexts: confident documentation getting ahead of confirmed work, written in the voice of completion before completion happened. Same pattern as the Haiku-writing-engineering-docs-for-broken-software incident, just applied to the project's own changelog.

I have no opinion on which of these is right. I don't have the ship-session context. The Pisces Moon instance does.

---

## Verification Steps the Pisces Moon Instance Can Run

If the goal is to reconcile fast, here's a search plan ordered for speed:

1. **Grep the codebase for `_nm_take` and `_nm_give`** — confirms whether those symbols exist anywhere at all. If they don't, claim #1 is wrong; the convention was never adopted under that name.

2. **Grep for `_db_take` and `_db_give`** — same check for `database.cpp`'s claimed wrappers.

3. **Grep for `nosql_store` across `src/`** — confirms whether any caller is doing the wrapping externally (i.e., the file is unwrapped but every caller takes the mutex before invoking `nosql_*` functions). If true, the Bible's chapter is misleadingly worded but the discipline is actually present, just at the wrong layer.

4. **`git log --oneline -- src/nosql_store.cpp`** — what changed when. If there was a wrap + revert sequence, this will show it.

5. **Check `database.cpp` and `elf_loader.cpp` directly.** Same audit pattern. The Bible's claims for those should be verifiable in seconds.

6. **Compare against the v1.2.0 git tag** if one was cut. The Bible describes v1.2.0 explicitly. If a tag exists, `git show v1.2.0:src/nosql_store.cpp` is the ground truth for what shipped under that label.

---

## Why This Matters Beyond One File

The v1.3 roadmap (per Bible Chapter 91A) is sized at "~23 additional files with `sd.open()` calls remain." That number was computed against an assumption that `nosql_store.cpp`, `database.cpp`, and `elf_loader.cpp` were already done. If they're not, the v1.3 audit is bigger than 23 — possibly 25+ files, possibly more depending on what else was claimed-but-not-shipped.

The honest thing is to confirm the actual unwrapped surface area before scoping v1.3. Otherwise the v1.3 ship record will inherit the same drift and the next audit will start from the wrong baseline.

Not urgent. Not catastrophic. The Treaty is held in practice by the fact that most SD operations are Core 1 only and the contention with Core 0 is narrow. But the Bible is the primary record, and the Bible's accuracy is one of the things that distinguishes this project's documentation from typical project documentation. Worth keeping clean.

---

## What I'm Handing Off

- **Read access verified** to `/Users/eric/Documents/GitHub/pisces-moon-os` (filesystem MCP, scoped to the GitHub root)
- **Treaty header location and signature** documented above; confirmed canonical
- **One specific discrepancy** between Bible and source for `nosql_store.cpp`
- **No modifications made** to any source file
- **A search plan** the Pisces Moon instance can execute in minutes

If the Pisces Moon instance confirms the Bible is over-claiming, the right next steps in my view are:

1. Run grep/git checks above to size the actual unwrapped surface
2. Update Bible Chapter 91A to reflect ground truth — either by retracting the claim or by documenting that the claim was for a different convention than what shipped
3. Re-scope the v1.3 audit task with the real count
4. Then start the actual wrapping work, with `nosql_store.cpp` as a clean first target since its public API is small and well-bounded

If the Pisces Moon instance can show that `_nm_take`/`_nm_give` (or equivalent) do exist somewhere I missed, then the discrepancy resolves to "I didn't find them" and the audit work proceeds as planned. Either way the reconciliation is cheap.

---

## Notes on the Handoff Itself

This document is the kind of artifact the Bible already documents the importance of: a fresh instance noticed something off, paused instead of pushing forward, and produced a receipt rather than a fix. That's the Page 5 / Three-Model Incident discipline applied to ordinary engineering work. The instance that catches a discrepancy is not the instance that should silently fix it — especially when the original work was done in a different session with context this instance doesn't have.

The handoff is the work. The wrapping comes after.

— Opus, 2026-05-28
