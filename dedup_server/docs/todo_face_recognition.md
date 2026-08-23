# FACE — local facial recognition with natural-language person search

Status: designed 2026-08-23, not started. Depends on nothing new; QP-1's
dimension registry was built to receive the `person` dimension (QP-2).

Decisions (user, 2026-08-23):
- **Apple Vision framework** for face detection/alignment (macOS-native,
  no detector model file). See §2 for the identity-embedding caveat and
  the hybrid it forces.
- **Cluster first, name later**: automatic clustering; the People tab
  shows unnamed clusters and the user names the ones they care about.
- **Faceless dimensions in v1**: `face_count` ("photos with 3 people",
  "group photos") ships alongside named-person search.
- **Trailing background sweep**, not inline processing: ingest latency is
  untouched; faces extract in a resumable scheduler job that also serves
  as the one-time backfill of the existing library (~145k images).

Everything runs locally. No image, face, or embedding ever leaves the
machine.

## 1. Architecture at a glance

```
sweep (scheduler job, resumable)
  └─ detect + align  — Apple Vision (VNDetectFaceLandmarksRequest,
  │                    VNDetectFaceCaptureQualityRequest)
  └─ identity embed  — small ONNX embedder (ArcFace-class, 512-D) on the
  │                    aligned 112×112 crop, via OnnxSessionManager
  └─ store           — faces table + vec_face_index (sqlite-vec)
  └─ cluster         — incremental KNN join + periodic consolidation
        └─ name      — People tab (FACE-3)
              └─ search — QP-2 `person` + `face_count` dimensions
```

## 2. FACE-1 — extraction

**The Vision caveat (load-bearing).** Vision provides detection,
landmarks, and capture quality, but **no public identity embedding** —
Apple's person-recognition network is private to Photos. Its generic
`VNFeaturePrintObservation` is an image-similarity print (matches
lighting/clothing more than identity) and clusters faces poorly. So the
pipeline is hybrid: Vision finds and aligns faces; a small ONNX
ArcFace-class embedder (512-D, ~5 MB, gitignored beside the CLIP models)
turns each aligned crop into an identity vector. If a cross-platform
build ever matters, only the detector TU swaps (see interface below).

- `FaceDetector` interface (C++), implemented by `VisionFaceDetector`
  (Objective-C++ TU, macOS-only, linked into the server target only) —
  returns boxes + 5-point landmarks + capture quality. The interface
  keeps a future ONNX detector a drop-in.
- Detection input: the transcoded/preview representation (RAW is covered
  by the existing demosaic path), downscaled to ≤2048 px.
- Alignment: standard 5-point similarity transform to 112×112.
- Embedding: unit-normalized 512-D float, stored like CLIP embeddings.

**Schema**

```sql
faces(
  id INTEGER PRIMARY KEY,
  file_id INTEGER NOT NULL,          -- scanned_files.id
  bbox_x REAL, bbox_y REAL, bbox_w REAL, bbox_h REAL,  -- normalized
  quality REAL,                      -- Vision capture quality 0..1
  embedding BLOB,                    -- 512-D float, unit-normalized
  cluster_id INTEGER,                -- NULL = unclustered (low quality)
  person_id INTEGER                  -- NULL until the cluster is named
);
face_clusters(id, centroid BLOB, size, cover_face_id, person_id NULL);
persons(id, name UNIQUE COLLATE NOCASE, cover_face_id, created_at);
-- scanned_files additions:
--   face_scan_version INTEGER NOT NULL DEFAULT 0  (0 = never scanned;
--     sweep selector: processed >= 2 AND is_video = 0 AND
--     face_scan_version < kFaceScanVersion — bumping the constant
--     re-sweeps the library after a model/threshold change)
--   face_count INTEGER  (maintained by the sweep; NULL = unscanned —
--     never 0-by-default, the EXIF NULL discipline)
vec_face_index = vec0(face_id INTEGER PRIMARY KEY, embedding float[512])
-- + DF-2-style reconcile at startup.
```

**The sweep (FACE-1b).** A scheduler-registered background job (the
duplicate-finder cadence pattern): N files per tick from the selector
above, per-file cost ~100 ms, fully resumable, progress via a status
RPC for the UI. It is both the backfill (~145k images ≈ hours, spread
across ticks) and the steady-state trailer for new files. Dataless
(cloud-evicted) files are skipped without being opened, exactly like the
RAW EXIF backfill — a sweep must never trigger a mass re-download.

## 3. FACE-2 — clustering

- **Incremental join**: each new face KNNs `vec_face_index`; join the
  best cluster when cosine ≥ `T_join` (start ~0.58 for ArcCase-class
  embeddings), else start a singleton. Faces below a quality/size gate
  are stored but left unclustered (they still count for `face_count`).
- **Consolidation** (periodic, like the finder's reconcile): merge
  clusters whose centroids agree ≥ `T_merge`, re-elect cover faces,
  honoring user constraints: cannot-link (from "not this person") and
  must-link (from manual merges) are persisted and always win.
- Naming = pointing a cluster at a `persons` row; merging a named and
  an unnamed cluster adopts the name.

## 4. FACE-3 — People tab (VU)

- Grid of clusters: cover-face crop, count, name or "Unnamed", sorted by
  size. Rename, merge (multi-select), hide.
- Drill-down reuses the day view filtered by person (same plumbing as
  search).
- Corrections: "not this person" removes the face, records cannot-link,
  and re-clusters it.
- New RPCs: `ListFaceClusters`, `NameCluster`, `MergeClusters`,
  `RemoveFaceFromCluster`, `FaceScanStatus`. Face crops are served
  through the existing thumbnail pipeline (crop of the cached asset).

## 5. FACE-4 (QP-2) — natural-language search

The location playbook, verbatim — this is why the prompt never has to
change as people get named:

- **Registry**: `person` (op `in`; hint text "names the user has
  assigned to people") and `face_count` (ops `eq,gt,lt,range`).
  Two registry entries; the GBNF and prompt schema regenerate.
- **Prompt**: few-shots — "photos of alice at the beach" → person
  filter + clip "beach"; "group photos with 3 people" → face_count eq 3;
  rule: "of/with <name>" is a person fact. The system prompt stays
  byte-stable (names are NOT in the prompt), so the KV prefix cache
  survives naming.
- **Validator**: person values resolve against `persons.name`
  (case-insensitive); an unresolved name is demoted to a CLIP query —
  "photos of Ziggy the dog" still works semantically. `face_count`
  sanity: 0–50.
- **Constraints/SQL**: `ExifQueryConstraints` gains `person_ids`;
  where-clause `EXISTS (SELECT 1 FROM faces f WHERE f.file_id = sf.id
  AND f.person_id IN (...))`; `face_count` rides the existing numeric
  constraint machinery against the precomputed column.
- **Algebra unchanged**: scaffolded ("of alice") = bound filter; bare
  ("alice") = bare branch; ai/xf pipeline toggles, per-tile badges, and
  the degraded-parse indicator all apply as-is (person facts are
  xf-side).
- **Regex fallback**: names are dynamic, so the fallback gets a simple
  deterministic scan — "of/with <token>" where token matches a known
  person name. Model-name-grade phrasing still needs the LLM, and the
  degraded indicator covers its absence.

## 6. Config

```
media.face.enabled            (default false until models ship)
media.face.embedModelPath     (models/face-embed-*.onnx)
media.face.joinThreshold      (0.58)
media.face.mergeThreshold     (0.65)
media.face.minQuality         (0.2)   # below: stored, not clustered
media.face.sweepBatch         (200)   # files per scheduler tick
```

## 7. Phases (each independently PR-able)

1. **FACE-1** schema + Vision detector + ONNX embedder + sweep job.
2. **FACE-2** incremental clustering + consolidation + constraints.
3. **FACE-3** People tab + naming/merge/correction RPCs.
4. **FACE-4** QP-2 dimensions, prompt few-shots, validator, SQL, golden
   matrix additions.
5. **FACE-5** polish: aliases/nicknames, pets question, video frames —
   explicitly deferred.

## 8. Testing

- Hermetic: clustering math on synthetic unit vectors; validator
  person/face_count checks; SQL constraint e2e with hand-seeded `faces`
  rows (no Vision, no models — CI stays model-free like QP-1).
- Gated (macOS + fixture): Vision detection smoke on a license-free
  portrait fixture; embedder smoke behind `VURIO_FACE_ONNX`.
- Golden: person/face_count queries join the QP-1 golden matrix
  ("pictures of alice from 2019", "group photos with 3 people").
