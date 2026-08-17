# TODO

The project's task list. One line per task, newest first, each pointing at the
document that holds its detailed specification.

An AI agent picking up a task should read the linked design document end to end
before touching code — the line below is an index entry, not a specification.

---

## Open

- **EXIF-1 — extract comprehensive EXIF metadata (25+ fields).**
  Extend `ExifInfo` struct and `ExifReader::read()` to extract all major EXIF categories: camera/lens (make, model, lens model), exposure/light (ISO, aperture, shutter speed, exposure compensation, metering mode, flash), optics (focal length, 35mm equivalent, focus distance/mode), white balance/color (WB mode, color space, scene mode, contrast/saturation/sharpness), location/time (GPS, datetime), and geometry (orientation). Uses ImageMagick `-ping` and LibRaw metadata-only parsing.
  **Full specification: [todo_exif_searchable_queries.md](todo_exif_searchable_queries.md)**

- **EXIF-2 — update database schema with 25+ indexed EXIF columns.**
  Add comprehensive EXIF columns to `scanned_files` table (camera, exposure, optics, white balance, light, location, geometry). Create 7 indices on ISO, aperture, exposure, camera make/model, white balance, date, and GPS for efficient range and text queries.
  **Full specification: [todo_exif_searchable_queries.md](todo_exif_searchable_queries.md)**

- **EXIF-3 — implement location geocoding index (place names → GPS bounds).**
  Create `location_geocoder.hpp` to map location names (e.g., "yellowstone", "utah") to GPS bounding boxes using free GeoJSON data (Natural Earth or OSM). Cache parsed boundaries in memory for O(1) lookup.
  **Full specification: [todo_exif_searchable_queries.md](todo_exif_searchable_queries.md)**

- **EXIF-4 — parse comprehensive EXIF constraints with numeric operators.**
  Implement `ParseExifConstraints()` in text_search_engine to detect and extract all EXIF field references with operators: numeric comparisons ("iso above 1600", "shutter faster than 1/500", "f/2.8 or wider"), text matching ("canon camera", "tungsten wb", "manual focus"), location geocoding, and date ranges. Extract remaining semantic text for CLIP KNN.
  **Full specification: [todo_exif_searchable_queries.md](todo_exif_searchable_queries.md)**

- **EXIF-5 — integrate EXIF filters into text search pipeline.**
  Modify `SearchQuery()` to parse EXIF constraints, build dynamic SQL WHERE clause, apply post-KNN filtering. Combine CLIP text similarity with EXIF constraints. Run comprehensive unit/integration tests covering all field types and operators.
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
