# TODO

The project's task list. One line per task, newest first, each pointing at the
document that holds its detailed specification.

An AI agent picking up a task should read the linked design document end to end
before touching code — the line below is an index entry, not a specification.

---

## Open

- **EXIF-1..5 — comprehensive EXIF-searchable prompt queries.**
  *Implemented 2026-08-19 on `feature/searchable_exif` (EXIF-1 `10b737a3`,
  EXIF-2 `73b2e26d`, EXIF-3 `724eecb8`, EXIF-4 `63b039cc`, EXIF-5 in the
  final commit). All 370 unit tests pass; extraction verified against live
  library photos (rational f-numbers, EXIF 2.3 PhotographicSensitivity ISO,
  DMS GPS with hemisphere signs). Remaining before Done: restart the server
  (ensureTable adds the 27 columns + 7 indices), force a re-process of the
  existing library so old rows gain EXIF values (new columns are NULL until
  each file is re-processed), then verify live UI queries like "find all
  pictures taken with a canon in yellowstone".*
  Queries now split into EXIF constraints + semantic text: camera makes,
  ISO / aperture / shutter / focal length with numeric operators, flash
  on/off, year & month dates, and preposition-triggered place names
  ("in yellowstone") geocoded to GPS bounds via a bundled Natural Earth
  gazetteer. Pure-metadata queries skip CLIP entirely (SQL-only path);
  mixed queries run CLIP KNN then filter matches by SQL. Missing EXIF
  stores NULL, never 0 — a GPS-less photo can never match a location.
  **Full specification: [todo_exif_searchable_queries.md](todo_exif_searchable_queries.md)**

- **THUMB-1 — render thumbnails at the source image's aspect ratio.**
  *Implemented 2026-08-16 (uncommitted); remaining: run the §9.1 upgrade flow
  (restart the server → Advanced → Clear image caches), the §9.2 stale-EXIF
  snippet, and the §10 manual verification list on a live library. Move to
  Done once those pass.*
  Every thumbnail in the SwiftUI app was center-cropped into a square. Now
  fixed-height / variable-width tiles driven by the image's display geometry,
  clamped to 3:1 and letterboxed (never cropped) past the clamp.
  **Full specification: [todo_aspect_ratio_thumbnails.md](todo_aspect_ratio_thumbnails.md)**
  — read §3 for the display-geometry model and the EXIF orientation bug it
  depends on, §4 for the seams that keep the future non-destructive edit
  pipeline (rotate/crop as a stored op list) cheap to add, §5 for the backend
  (proto, SQL, gRPC), §6 for the SwiftUI changes including the `BracketView`
  correctness bug in §6.5, §7 for the 128→256 asset-size bump, §9 for the
  cache-wipe-instead-of-migrate decision, §10 for the required tests, and §11
  for the step-by-step implementation order.
  Scope: `swift_ui/` only; `web_ui/` is explicitly out of scope. The edit
  pipeline itself is out of scope — see §4.3 for what is deliberately deferred.

## Done

- **Scrolled-off thumbnails kept their download slots — cancellation never
  reached the shared load.** Fixed 2026-08-17; see `troubleshooting_and_fixes.md`.
- **DF-4 — the finder's per-run orphan-edge purge held the write lock ~25 s
  every 5 s at 14.9M edges.** Fixed 2026-08-17; see `troubleshooting_and_fixes.md`
  ("DF-4: Write-Lock Storm From the Per-Run Orphan-Edge Purge").
- **FILT-1 — the low-resolution filter un-ignored every file it rejected.**
  Fixed 2026-08-16 in `e759a3d1`; see `troubleshooting_and_fixes.md`.
- **DISC-1 — replace the id watermark with per-file discovery state.**
  Fixed 2026-08-15; see [todo_discovery_watermark.md](todo_discovery_watermark.md).
