# Pisces Moon OS — Reference Database Seed Files

These are seed content files for the offline reference apps (SURVIVAL,
MEDICAL, HISTORY) read by `data_reader.cpp` (keyboard devices) and
`c28p_data_reader.cpp` (touch kiosks: C28P, Maxine). All devices share the
same backing store, `nosql_store.cpp`.

## What this is

The apps read from the device's **microSD card** at runtime, not from the
firmware. The store (`nosql_store.cpp`) expects this on-card layout:

```
/data/<category>/index.json
/data/<category>/entry_NNN.json   (NNN = zero-padded 3-digit id)
```

`index.json` schema:
```json
{
  "category": "survival",
  "count": 6,
  "entries": [
    { "id": 1, "title": "...", "tags": "a,b,c", "file": "entry_001.json" }
  ]
}
```

Each entry file schema:
```json
{ "id": 1, "title": "...", "tags": "a,b,c", "content": "..." }
```

`nosql_init()` auto-creates an empty `index.json` (count 0) on first run, so
the apps never crash on a missing category — they just show "No entries yet."
These files give them real content instead.

## How to deploy

Copy the **entire `data/` folder to the root of the device's microSD card**,
so the card ends up with `/data/survival/`, `/data/medical/`, and
`/data/history/`. Re-insert the card and open the SURVIVAL / MEDICAL / HISTORY
apps.

## Categories included

- **survival** — 6 entries (rule of threes, water, fire, shelter, signaling, navigation)
- **medical** — 6 entries (general, non-prescriptive first-aid awareness)
- **history** — 6 entries (printing press, industrial revolution, electricity, germ theory, transistor, internet)

## IMPORTANT — review before relying on these

The MEDICAL entries are **general awareness information, not medical advice**,
and were drafted to be deliberately non-prescriptive (no dosages, no
diagnosis). Review and edit all content — medical especially — before treating
any of it as authoritative. This is seed scaffolding, not a vetted reference.

## Adding more entries

Two options:
1. By hand: add `entry_NNN.json`, then add a matching record to that
   category's `index.json` and bump `count`. Keep `file` and the zero-padded
   filename in sync.
2. On-device: the Gemini terminal / `nosql_save_entry()` path writes new
   entries and updates the index automatically.
