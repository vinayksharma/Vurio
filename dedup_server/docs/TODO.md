# TODO

The project's task list. One line per task, newest first, each pointing at the
document that holds its detailed specification.

An AI agent picking up a task should read the linked design document end to end
before touching code — the line below is an index entry, not a specification.

Completed tasks move to Done as one-liners; their spec documents are deleted
(the full specifications remain available in git history).

---

## Open

- **STRIP-1 — film strips resize to fill the available width on window resize.**
  *Reported 2026-08-24; not yet designed.*
  The horizontal film strips (VU day strips and BINNED group cards) keep the
  width they were laid out with, so growing or shrinking the window leaves a
  ragged right-hand gap or clips the strip instead of reflowing. They should
  track the container's width and re-lay out the tiles to fill it whenever the
  window size changes.

## Done

- **VER-1 — git-derived versioning, sidebar build display, build-pair handshake
  check.** Completed 2026-08-28 (PR #89). Version computed from git at build
  time via one generator script embedding identical constants into the C++
  server and Swift client; sidebar shows build identity under the advanced
  overlay; the pairing Handshake verifies both were built from the same commit.
- **FACE-8 — configurable purge cutoff (`media.face.mergePurgeCutoff`): merges
  filter misfits, live from Settings; FACE-8b "Clear All Faces" factory reset
  of the people layer.** Completed 2026-08-24.
- **FACE-7 — matching accuracy: same-name merge, multi-exemplar clusters, join
  margin, outlier review.** Completed 2026-08-24.
- **STAMP-1 — film-camera date-stamp treatment for the day date/time label
  (DSEG7 Classic Italic, amber halation glow, ghost segments).** Completed
  2026-08-24.
- **FACE-6 — face-stack popup: click-to-open, inline rename, lens search,
  pick-the-cover (`SetFaceClusterCover`, pinned covers survive elections and
  merges).** Completed 2026-08-24.
- **FACE-1..5 — local facial recognition + natural-language person search.**
  Completed 2026-08-23/24: Vision detection + AuraFace ONNX embedding,
  incremental centroid clustering, PEOPLE tab with naming/merge/hide, QP-2
  person/face_count query dimensions, cannot-links, person aliases. Video
  faces deferred to a future video-support program.
- **QP-1 — local LLM query structurer (llama.cpp + Qwen2.5-1.5B Q4, GBNF
  grammar-constrained; LLM proposes, deterministic code verifies; regex parser
  stays as fallback and hermetic test baseline).** Completed 2026-08-23
  (PRs #53–#56).
- **RAW-EXIF — RAW files get the full EXIF field set.** Completed 2026-08-23
  on `feature/raw_exif_fields`, live-verified against real DNGs. LibRaw now
  extracts make/model/lens, ISO, aperture, shutter, focal, GPS, orientation;
  `MediaBackfill::backfillRawExif` converges old rows. Follow-ups: PR #58
  fixed the never-executed sweep selectors; SF_DATALESS files skipped; dead
  MP-2 thumbnail backfill removed.
- **EXIF-7 — query intent: bound constraints intersect, bare facts union.**
  Completed 2026-08-23 via QP-1a (algebra + regex fallback parser) and
  QP-1b–e (LLM structurer); every §11 acceptance item golden-tested.
  Spec: [todo_exif_query_intent.md](todo_exif_query_intent.md).
- **EXIF-1..6 — comprehensive EXIF-searchable prompt queries.** Completed
  2026-08-19/20: camera/ISO/aperture/shutter/focal/flash/date constraints plus
  gazetteer-geocoded place names split from semantic text; pure-metadata
  queries skip CLIP; parser vocabulary externalized to
  `config/exif_search_keywords.json`.
- **THUMB-1 — render thumbnails at the source image's aspect ratio.**
  Completed 2026-08-16 (PR #46). Fixed-height / variable-width tiles driven by
  display geometry, clamped to 3:1 and letterboxed past the clamp;
  `render_revision` left as the seam for the future non-destructive edit
  pipeline.
- **Scrolled-off thumbnails kept their download slots — cancellation never
  reached the shared load.** Fixed 2026-08-17; see `troubleshooting_and_fixes.md`.
- **DF-4 — the finder's per-run orphan-edge purge held the write lock ~25 s
  every 5 s at 14.9M edges.** Fixed 2026-08-17; see `troubleshooting_and_fixes.md`
  ("DF-4: Write-Lock Storm From the Per-Run Orphan-Edge Purge").
- **FILT-1 — the low-resolution filter un-ignored every file it rejected.**
  Fixed 2026-08-16 in `e759a3d1`; see `troubleshooting_and_fixes.md`.
- **DISC-1 — replace the id watermark with per-file discovery state.**
  Fixed 2026-08-15; see [todo_discovery_watermark.md](todo_discovery_watermark.md).
