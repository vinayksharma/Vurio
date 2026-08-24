# TODO

The project's task list. One line per task, newest first, each pointing at the
document that holds its detailed specification.

An AI agent picking up a task should read the linked design document end to end
before touching code — the line below is an index entry, not a specification.

---

## Open

- **STRIP-1 — film strips resize to fill the available width on window resize.**
  *Reported 2026-08-24; not yet designed.*
  The horizontal film strips (VU day strips and BINNED group cards) keep the
  width they were laid out with, so growing or shrinking the window leaves a
  ragged right-hand gap or clips the strip instead of reflowing. They should
  track the container's width and re-lay out the tiles to fill it whenever the
  window size changes.

- **QP-1 — local LLM query structurer (llama.cpp + ~1.5B Q4).**
  *Implemented 2026-08-23 (QP-1a–e, PRs #53–#56). All golden acceptance
  tests pass against the shipped Qwen2.5-1.5B q4_k_m (~1.5 s/query);
  hermetic suite green without the model. Remaining before Done: restart
  the server and verify live VU queries ("raw files from 2019", "sunset
  taken with a canon", the f2.5-to-f12 difficult query) with the ai/xf
  badges under the advanced overlay. QP-2 (person dimension) waits on the
  face DB.*
  A small local quantized instruct model (default Qwen2.5-1.5B-Instruct
  Q4_K_M via llama.cpp, Metal, greedy, GBNF grammar-constrained JSON)
  structures natural-language queries into facts (with filter/explore
  intent) + CLIP query segments. The LLM proposes, deterministic code
  verifies: span-grounding against the query text, registry validation,
  fallback ladder to the existing regex parser (which stays as the hermetic
  test baseline — CI never needs the model). Dimension registry generates
  prompt schema + GBNF, making future dimensions (face recognition
  `person`) an entry, not a grammar rewrite. Model file optional at
  runtime, like the CLIP ONNX.
  **Full specification: [todo_llm_query_structurer.md](todo_llm_query_structurer.md)**


- **FACE-1..5 — local facial recognition + natural-language person search.**
  *Designed 2026-08-23; FACE-1 implemented 2026-08-23 (schema, Vision
  detector, alignment, ONNX embedder hook, background sweep). FACE-2
  implemented 2026-08-23: centroid clustering (join 0.58 / merge 0.65,
  live-calibrated), consolidation, catch-up for pre-clustering faces,
  and a face-scan progress overlay on the VU status bar. FACE-3
  implemented 2026-08-23: PEOPLE tab (cluster grid with client-side
  face crops, name/rename, cmd-click multi-select merge, hide) and
  person drill-down into the VU day view. FACE-4 (QP-2) implemented
  2026-08-23: "photos of alice at the beach" and "group photos with 3
  people from 2020" parse to person/face_count filters; unresolved names
  demote to CLIP; regex fallback handles scaffolded known names. FACE-3b
  (2026-08-24): per-face "not this person" + cannot-links (bidirectional
  merge blocking). FACE-5 (2026-08-24): person aliases ("mom" -> Alice);
  pets stay CLIP's job (no local pet-identity model); video faces
  deferred to a future video-support program (library has no videos,
  no frame extraction exists, day view excludes videos). Model:
  AuraFace-v1 glintr100 (Apache-2.0) at models/face-embed.onnx;
  media.face.enabled is on. Decisions: Apple Vision detection
  (hybrid with a small ONNX identity embedder — Vision has no public
  identity embedding), cluster-first naming, face_count in v1, trailing
  background sweep (ingest untouched; doubles as the ~145k backfill).*
  Faces detected/aligned by Vision, embedded to 512-D identity vectors,
  clustered incrementally (user constraints win), named in a new VU
  People tab. Search rides QP-1's registry: `person` and `face_count`
  dimensions, names resolved by the validator (location playbook — the
  KV-cached prompt never changes as people are named), unresolved names
  demote to CLIP. Algebra, toggles, badges, degraded indicator all apply
  unchanged.
  **Full specification: [todo_face_recognition.md](todo_face_recognition.md)**

- **RAW-EXIF — RAW files get the full EXIF field set.**
  *Implemented 2026-08-23 on `feature/raw_exif_fields`. Live-verified against
  real DNGs (iPhone X, Fujifilm X-Pro1: ISO/aperture/shutter/focal/lens/GPS
  all extracted; GPS matches Spotlight's values).*
  The MP-1 LibRaw metadata path captured only camera/datetime/dimensions, so
  every RAW row had NULL in each column a bound search filter touches —
  "f2.5" or "iso above 800" silently excluded every RAW photo. LibRaw parses
  the exposure block in the same metadata-only `open_file()`; the fields are
  now copied out (make/model/lens, ISO, aperture, shutter as exposure time +
  APEX + text, focal + 35mm-equivalent, GPS from `parsed_gps` with
  hemisphere refs — no ref means NaN, never a (0,0) ocean fix — and
  orientation from LibRaw flip). Backfill: the old reader never wrote
  `exif_make`, so "RAW extension, `exif_camera` set, `exif_make` NULL" is
  exactly the old-reader population; `MediaBackfill::backfillRawExif` (runs
  after the thumbnail/preview backfill in integrity-check Phase 6) re-reads
  those rows inline (ms per file, no demosaic) and converges once each row
  gains a make. Offline files (disconnected drives) stay candidates for a
  later sweep.
  Follow-ups (2026-08-23): the sweep's selector statements were never
  executed (pre-existing Poco bug, PR #58); the integrity-check dialog now
  shows the sweep honestly ("Refreshing RAW photo metadata..." with live
  N-of-M via the progress callback); cloud-evicted (SF_DATALESS) files are
  skipped so the sweep never triggers a mass OneDrive/iCloud download; and
  the never-functional MP-2 thumbnail/preview backfill was removed — those
  are generated on demand by design.

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
  EXIF-6 (2026-08-20): the parser's vocabulary (camera brands, comparison
  synonyms, flash phrases, months, prepositions, filler words) lives in
  `config/exif_search_keywords.json` (`search.keywordsPath`), so new words
  are a JSON edit + restart, no rebuild; built-in defaults are the fallback
  and a lockstep test keeps file and defaults identical.
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

- **EXIF-7 — query intent: bound constraints intersect, bare facts union.**
  Completed 2026-08-23 via QP-1a (algebra + regex fallback parser) and
  QP-1b–e (LLM structurer); every §11 acceptance item golden-tested.
  Spec: [todo_exif_query_intent.md](todo_exif_query_intent.md).
- **Scrolled-off thumbnails kept their download slots — cancellation never
  reached the shared load.** Fixed 2026-08-17; see `troubleshooting_and_fixes.md`.
- **DF-4 — the finder's per-run orphan-edge purge held the write lock ~25 s
  every 5 s at 14.9M edges.** Fixed 2026-08-17; see `troubleshooting_and_fixes.md`
  ("DF-4: Write-Lock Storm From the Per-Run Orphan-Edge Purge").
- **FILT-1 — the low-resolution filter un-ignored every file it rejected.**
  Fixed 2026-08-16 in `e759a3d1`; see `troubleshooting_and_fixes.md`.
- **DISC-1 — replace the id watermark with per-file discovery state.**
  Fixed 2026-08-15; see [todo_discovery_watermark.md](todo_discovery_watermark.md).
