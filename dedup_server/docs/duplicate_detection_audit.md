# Duplicate Detection Audit: Artifacts and Matching Logic

**Date:** 2026-02-16  
**Context:** After adding full CLIP ONNX support (multi-input, dummy text inputs, dynamic dims). This audit confirms whether duplicate matching logic is still correct and whether generated artifacts remain valid for deduplication.

---

## 1. How Duplicates Are Detected

### 1.1 Data flow

1. **Artifact generation (ingestion)**  
   - **QualityPipeline** (file and memory paths) calls **OnnxAdapter::ComputeEmbedding** with config: `media.image.onnx.modelPath`, `media.image.embeddingDim` (default 512), `media.image.onnx.inputSize` (224).  
   - ONNX runs the image through the vision branch (with dummy `input_ids`/`attention_mask` for full CLIP). We select the **image** embedding output: never use an output whose name contains `"text"` (that would make every image get the same embedding → all similarities 1.0). We prefer names containing `"image"`, `"vision"`, or `"pooled"`; if none, we use the **first** non-text output (many models use [image, text] order).  
   - Result is stored in **image_artifacts**: `file_path` (PK), `embedding_model` (model path string), `embedding_dim`, `embedding` (BLOB of float32s).  
   - One row per file (PK = `file_path`); reprocessing overwrites with the current model’s embedding.

2. **Duplicate finder batch**  
   - **processBatch** selects from **scanned_files**: `id > last_processed_id AND processed = 2`, ordered by `id`, limited by `batch_size`.  
   - No filter on `embedding_model` or `image_artifacts` in this query.  
   - For each `file_id`, **loadFileArtifacts** does:
     - `JOIN scanned_files sf WITH image_artifacts ia ON sf.file_path = ia.file_path WHERE sf.id = ?`  
   - So only files that have an `image_artifacts` row are loaded; others are skipped (no artifact ⇒ no duplicate check).

3. **Similarity and grouping**  
   - **computeSimilarity(file1, file2, mode)** calls:
     - **SimilarityCalculator::computeEmbeddingSimilarity**(`file1.embedding`, `file2.embedding`, **file1.embedding_dim**).  
   - Similarity is **cosine similarity** of the two float32 vectors: `(A·B) / (||A|| * ||B||)`, clamped to [-1, 1].  
   - **Dimension handling:** Expected size is `dim * 4` bytes. If either blob size differs, the function returns **0.0** (no match).  
   - **Thresholds:** Config `duplicates.threshold.min` (e.g. 0.90–0.92) and `duplicates.threshold.max` (e.g. 0.96–0.98). A file is added to a group only if similarity to the representative and to **all** members is ≥ threshold_min.  
   - Representative selection uses `duplicates.representative.strategy` (e.g. size_then_age).

### 1.2 Relevance of current artifacts

- **Format:** Embeddings are float32 vectors stored as BLOBs; duplicate finder and SimilarityCalculator expect exactly that.  
- **Dimension:** Config `media.image.embeddingDim` (512) is used both when writing (QualityPipeline) and when comparing (artifact’s `embedding_dim` is read; we use `file1.embedding_dim` for both sides in the calculator).  
- **Semantics:** With the current full CLIP setup we still run the **image** through the vision encoder and take the first output. For standard Hugging Face–style CLIP ONNX, output 0 is **image_embeds**, so the stored vector is a valid image embedding in the same space as before (same vision backbone). So the **artifacts we generate are still valid for duplicate detection** as long as comparisons are done within the same model.

---

## 2. Gaps and Risks

### 2.1 No same-model constraint (fixed by implementation)

- **loadFileArtifacts** returns whatever single row exists for that file (one row per `file_path`).  
- **computeSimilarity** does **not** check that `file1.embedding_model == file2.embedding_model`.  
- If the config model is changed (e.g. from `clip-RN50.onnx` to `clip-image-vitb32.onnx`), over time we get:
  - Some files with artifacts from model A, others from model B.  
- **Same dimension, different model:** We would still compute cosine similarity between two 512‑dim vectors from **different** embedding spaces. Similarity would be meaningless (can be high or low by chance), so we could get false duplicates or miss real ones.  
- **Different dimension:** SimilarityCalculator returns 0.0 (size mismatch), so we would not group them.  
- **Conclusion:** We must only treat two files as duplicates when their embeddings come from the **same** `embedding_model`. Recommended fix: in **computeSimilarity**, if `file1.embedding_model != file2.embedding_model`, return **0.0** (do not match).

### 2.2 Output selection (fixed)

- Full CLIP ONNX has **two outputs**: image_embeds and text_embeds. Using **text_embeds** (from our dummy zero `input_ids`) would make every image produce nearly the same vector and **destroy duplicate matching** (all similarities 1.0, all images in one group).
- **Fix:** The ONNX adapter **never** uses an output whose name contains `"text"`. It prefers names containing `"image"`, `"vision"`, or `"pooled"`. If none match, it uses the **first** non-text output (many exports use [image, text] order; using the last non-text was wrong when the first output was text-named). If all outputs contain "text", inference fails with a clear error.
- **After fixing output selection:** Run `./scripts/reset_embeddings_and_duplicates.sh` to clear embeddings and duplicate groups and mark files for re-processing, so the server recomputes embeddings with the correct output.

### 2.3 Batch query does not filter by model

- The batch of `file_id`s is “all processed files with id > last”. So we can load a mix of artifacts from different models in the same run.  
- With the **same-model check in computeSimilarity**, we will simply never form groups across different models (similarity forced to 0.0). No need to change the batch query unless we want to avoid loading artifacts from other models at all (optional optimization).

### 2.4 Threshold range

- **Semantics matter:** CLIP embeddings capture high-level meaning. With **low** thresholds (e.g. min 0.90), images that are only *semantically* similar (e.g. “all documents”, “all passports”) get grouped, so you can see hundreds of different documents in one group.
- For **true duplicate** detection (same or near-identical image only), use **stricter** thresholds: e.g. **min 0.97**, **max 0.99**. Then only very similar images (resize, recompress, slight crop) group together.
- Defaults in config are now 0.97 / 0.99. If you had lowered them, raise them again and reset duplicate groups so the finder re-runs with the new thresholds.

### 2.5 Similarity score meaning and “all 1.000”

- **Stored value:** Each member’s `similarity_score` is **similarity to the group representative** (cosine similarity in [0, 1]).
- **Representative:** The representative is the same file as itself, so it correctly has **1.0**.
- **Other members:** Should be **&lt; 1.0** (e.g. 0.97–0.999) for true duplicates. If **every** member shows 1.000, embeddings are almost certainly **identical** (e.g. wrong ONNX output such as text_embeds, or a bug). Re-embed with the correct image output and reset duplicate groups.
- **Ranking:** Members are ordered by `similarity_score` descending (highest first). The representative (1.0) is first; then the next most similar to the rep. No change needed for ranking.

---

## 3. Summary Table

| Component              | Status / action |
|------------------------|-----------------|
| Artifact format        | OK (float32 BLOB, dim in config and DB). |
| Artifact semantics     | OK (output 0 = image embedding; same vision space). |
| Similarity metric      | OK (cosine; dimension mismatch → 0.0). |
| Same-model comparison  | **Fixed:** compare only when `embedding_model` matches. |
| Output selection        | Fixed: never text; prefer image/vision/pooled; else first non-text. |
| Batch query            | OK; same-model check in similarity is sufficient. |
| Thresholds             | No change needed. |

---

## 4. Conclusion

With the latest ONNX changes, the **artifacts we generate are still relevant** for duplicate detection: they are image embeddings in the same CLIP vision space, and the pipeline (dimension, format, cosine similarity, thresholds) is unchanged. The only required fix is to **never compare embeddings from different models** by enforcing a same-`embedding_model` check in the duplicate finder’s similarity path. That fix is implemented in code; this document serves as the audit record.
