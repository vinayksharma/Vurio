## Object-Aware Template Pipeline

### Intent

- Extend media processing so every image produces region-level embeddings (general, face, nature/building) plus a fused template vector for duplicate detection.
- Preserve current API contracts while replacing the underlying artifact storage and similarity math.
- Enable future region search workflows (lasso queries, named regions) by making region vectors instantly searchable through ANN indexes.

---

### Processing Flow Changes

1. **Decode + Preprocess**
   - Reuse existing loader; ensure decoded frame is handed to all downstream extractors to avoid redundant IO.
2. **Detectors**
   - General objects: YOLOv8-seg (pretrained) on GPU; outputs bounding boxes + masks.
   - Faces: RetinaFace for bounding boxes and landmarks.
   - Nature/buildings: leverage YOLO labels + Places365 scene classifier to tag mountains, skyline, trees.
3. **Embedding Generation**
   - General encoder: CLIP ViT-B/32 ONNX model; run on full frame and each detected region (crops or masked).
   - Face encoder: ArcFace ResNet100 ONNX model, normalized outputs.
   - Nature/building encoder: ViT trained on Places365; only invoked for regions with matching labels.
4. **Region Records**
   - Assign `region_id` per detection, capture label, detector_type, confidence, bbox, optional mask reference.
   - Store general embedding for **every** region; attach specialized embedding where applicable (face, nature/building).
5. **Unified Template**
   - Inputs: whole-image CLIP vector + pooled (mean + max) general region vectors + pooled specialized vectors.
   - Pass through lightweight projection MLP (e.g., [dim 512 → 1024 → 512]) to create final template vector used by duplicate detection.
6. **Indexing**
   - Insert region embeddings into FAISS indexes (separate index per model_type) during processing.
   - Record index metadata (version, vector count) to support rebuilds.

---

### Database Schema (fresh DB)

#### `image_templates`

| Column                      | Type      | Notes                              |
| --------------------------- | --------- | ---------------------------------- |
| `image_id`                  | FK        | References existing `images` table |
| `template_version`          | TEXT      | e.g., `templates.v2`               |
| `vector`                    | BLOB      | 512-d float array (serialized)     |
| `created_at` / `updated_at` | TIMESTAMP | managed by ORM                     |

#### `image_regions`

| Column                                 | Type          | Notes                                          |
| -------------------------------------- | ------------- | ---------------------------------------------- |
| `region_id`                            | INTEGER PK    | Unique per region                              |
| `image_id`                             | FK            | Parent image                                   |
| `label`                                | TEXT          | e.g., `face`, `mountain`, `building`, `person` |
| `detector_type`                        | TEXT          | `yolov8`, `retinaface`, `places365`            |
| `confidence`                           | REAL          | 0–1                                            |
| `bbox_x`, `bbox_y`, `bbox_w`, `bbox_h` | REAL          | Normalized coordinates                         |
| `mask_path`                            | TEXT nullable | Optional serialized mask                       |
| `general_embedding_id`                 | FK            | Points to `region_embeddings`                  |
| `special_embedding_id`                 | FK nullable   | Points to specialized embedding (face/nature)  |
| `created_at`                           | TIMESTAMP     |                                                |

#### `region_embeddings`

| Column          | Type       | Notes                                |
| --------------- | ---------- | ------------------------------------ |
| `embedding_id`  | INTEGER PK |                                      |
| `region_id`     | FK         |                                      |
| `model_type`    | TEXT       | `general`, `face`, `nature_building` |
| `model_version` | TEXT       | e.g., `clip.vitb32.onnx.v1`          |
| `dimension`     | INTEGER    | 512 / 256 etc.                       |
| `vector`        | BLOB       | Raw float bytes                      |
| `created_at`    | TIMESTAMP  |                                      |

#### `region_index_metadata`

Tracks FAISS indexes.

| Column          | Type      | Notes                             |
| --------------- | --------- | --------------------------------- |
| `model_type`    | TEXT PK   |                                   |
| `model_version` | TEXT PK   |                                   |
| `index_uuid`    | TEXT      | Identifier for stored FAISS index |
| `index_type`    | TEXT      | e.g., `IVF_PQ`, `HNSW32`          |
| `vector_count`  | INTEGER   |                                   |
| `built_at`      | TIMESTAMP |                                   |

---

### Duplicate Detection Adjustments

- Loader pulls unified vectors from `image_templates` instead of legacy artifact blobs.
- Configuration adds `duplicates.templates.v2.threshold_min/max`; tuning happens separately once embeddings are validated.
- Representative cache stores `(image_id, template_version, vector)`; mismatched versions trigger regeneration or fallback.
- Similarity calculator remains cosine-based but must accept new vector length (512) and normalizes accordingly.
- Batch flow (compare to representatives → add/move members → update checkpoint) stays unchanged, so API responses do not change.

---

### API Impact

- `/api/duplicates/*` endpoints continue emitting the same fields (`group_id`, `representative_path`, similarity values).
- Internal DTOs source template vectors via new DAO; no response contract changes.
- Logging/metrics add template/index version tags for observability.

---

### Region Search Enablement

- Lasso query flow: crop → CLIP embedding → search `general` FAISS index; if classifier says “face” or “nature/building,” also search specialized indexes.
- Named regions map to stored region IDs; label metadata stays alongside the embedding so we can filter (e.g., only faces).
- Future API work can expose `/api/regions/search` without modifying duplicate detection endpoints.

---

### Open Tasks (Future PRs)

1. Implement detector + embedding orchestrator (reuse existing media processor hooks).
2. Define serialization helpers for embeddings (float32 → BLOB) and integrate with database manager.
3. Train/validate fusion MLP and set threshold defaults.
4. Build FAISS management utilities (insert, snapshot, rebuild).
5. Add migration scripts to create the new tables and drop legacy artifact columns during fresh deploy.
