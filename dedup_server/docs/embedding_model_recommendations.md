# Embedding Model Recommendations for Image Deduplication & Media Management

**Context:** Your system uses ONNX-based image embeddings and cosine similarity to group near-duplicate images. This doc summarizes research and recommends models that fit a C++/ONNX pipeline.

---

## Setup (CLIP ViT-B/32)

To download and place the recommended model in the correct location:

```bash
cd dedup_server && ./scripts/setup_clip_image_encoder.sh
```

- If **torch** and **transformers** are installed, this exports an **image-only** ONNX (1 input, 1 output) to `models/clip-image-vitb32.onnx`.
- Otherwise it downloads a full CLIP ONNX from Hugging Face; the server automatically uses the **image** output (not text).

Config already points to `models/clip-image-vitb32.onnx` (see `media.image.onnx.modelPath` in `config/config.yaml`).

### Which model is currently working?

The file that works in production is typically the **full CLIP** ONNX from Hugging Face:

- **Source:** `sayantan47/clip-vit-b32-onnx` → `onnx/model.onnx` (used by `setup_clip_image_encoder.sh` fallback).
- **Size:** ~578 MB (full CLIP: image + text inputs; one image output).
- **How the server uses it:** The ONNX adapter detects the 4D image input (`pixel_values`), feeds it plus dummy text inputs, and reads **output 0** (image embeddings). So full CLIP works without an image-only export.

Scripts that download a *different* CLIP (e.g. `onnx/models` or `onnx/clip-vit-base-patch32`) can produce a different graph layout or input/output order. Those may work too, but the **known-good** source is `sayantan47/clip-vit-b32-onnx`. `download_clip_model.py` now tries that URL first so automated downloads match the working model.

---

## 1. What You Need From the Model

| Goal | Description |
|------|-------------|
| **Near-duplicate detection** | Same image with different resolution, crop, format, or light compression → **high similarity** (e.g. 0.97+). |
| **Avoid semantic grouping** | Different images that are only “same kind” (e.g. two different documents) → **lower similarity** so they don’t merge into one group. |
| **Technical fit** | Single **image embedding** output (no text branch), ONNX-friendly, 224×224 or similar input, fixed-dim vector (e.g. 512 or 768). |

So you want a **vision-only** encoder that is good at **perceptual / instance-level** similarity, not “same semantic category.”

---

## 2. Model Options (Summary)

### A. CLIP ViT-B/32 (image encoder only) — **Recommended default**

- **Why it fits:** Strong, well-understood vision features; good for “same photo, different size/format”; widely used for retrieval/similarity.
- **Catch:** You must use the **image** output only. Full CLIP ONNX has image + text outputs; using the wrong one gives constant embeddings (your earlier bug). Use either:
  - An **image-only** ONNX export (1 input, 1 output), or  
  - Multi-output ONNX with correct output selection (image, not text).
- **Export:** Your repo’s `scripts/export_clip_image_encoder_onnx.py` exports the vision encoder only → single output (e.g. `pooled_output`). That matches the “image-only” path in the adapter.
- **Input:** 224×224, same preprocessing you already have (resize, normalize with CLIP stats).
- **Embedding dim:** 512.
- **Where to get:** Export locally with the script above, or use a pre-exported **vision-only** ONNX (e.g. from [clip2onnx](https://github.com/QIN2DIM/clip2onnx) or [CLIP-to-onnx-converter](https://github.com/jalberse/CLIP-to-onnx-converter) → use the **visual** encoder `.onnx` only).

**Verdict:** Stick with **CLIP ViT-B/32 image-only** for dedup. Ensure the ONNX has 1 input + 1 output (or that the adapter always selects the image output). Then tune thresholds (e.g. 0.97–0.99) for “near-duplicate only.”

---

### B. ResNet-50 (classification backbone as feature extractor)

- **Pros:** Simple, fast, small; many ONNX versions (e.g. [Qdrant/resnet50-onnx](https://huggingface.co/Qdrant/resnet50-onnx)); good for instance-level similarity in production systems (e.g. Milvus-style dedup).
- **Cons:** Slightly older than ViT; typically 2048-d before pooling; you’d take the layer before the classifier and optionally project to 512.
- **Use case:** Good if you want smaller/faster models and can accept a different embedding dimension and preprocessing.

---

### C. DINOv2 (ViT, self-supervised)

- **Pros:** Strong visual features, no labels; good for retrieval and “same instance” similarity; PyTorch Hub.
- **Cons:** No official ONNX in the repo; you’d export yourself (e.g. `torch.onnx` from the vision trunk). Not a drop-in replacement without an export script.
- **Use case:** Consider if you later need better robustness than CLIP and are willing to maintain an ONNX export pipeline.

---

### D. Perceptual hashing (pHash, etc.)

- **Pros:** Very fast, small, no GPU; good for exact / very-near duplicates.
- **Cons:** Less robust to crop/scale/compression; not a single “embedding model” you swap in; usually used as a first stage or for exact dupes only.
- **Use case:** Complement: use pHash for exact/near-exact, keep CLIP (or another embedding model) for robust near-duplicate grouping.

---

## 3. Recommended Path for Your System

1. **Primary:** Use **CLIP ViT-B/32 image encoder only**:
   - Prefer an ONNX with **1 input, 1 output** (your adapter’s “image-only” path uses input 0 and output 0 → no text, no selection bugs).
   - Generate it with `scripts/export_clip_image_encoder_onnx.py` (output name e.g. `pooled_output`), or download a **vision-only** CLIP ONNX (e.g. visual encoder from clip2onnx / CLIP-to-onnx-converter).
   - Config: `media.image.onnx.modelPath` → that single-output ONNX; `media.image.embeddingDim`: 512.

2. **Thresholds:** Keep strict so only near-duplicates group:
   - `duplicates.threshold.min`: **0.97**
   - `duplicates.threshold.max`: **0.99**  
   So “same photo, different size/format” matches; “different documents” stay separate.

3. **Optional later:** Add **pHash** (or similar) as a fast pre-filter for exact/near-exact duplicates; keep CLIP for the main near-duplicate grouping.

4. **If you outgrow CLIP:** Consider **DINOv2** (after exporting to ONNX) or **ResNet-50**-based feature ONNX for a different speed/accuracy trade-off.

---

## 4. References (short)

- Benchmarking pretrained vision embeddings for near- and duplicate detection: [arXiv:2312.07273](https://arxiv.org/abs/2312.07273).
- Milvus image deduplication (ResNet/CLIP): [Milvus Image Deduplication](https://milvus.io/docs/ar/image_deduplication_system.md).
- CLIP vision-only ONNX: [clip2onnx](https://github.com/QIN2DIM/clip2onnx), [CLIP-to-onnx-converter](https://github.com/jalberse/CLIP-to-onnx-converter) (use visual encoder only).
- DINOv2: [facebookresearch/dinov2](https://github.com/facebookresearch/dinov2).

---

## 5. Quick checklist

- [ ] ONNX has **one output** (image embedding only), or adapter correctly selects **image** output by name.
- [ ] Preprocessing matches the model (224×224, same normalization as CLIP if using CLIP).
- [ ] `embedding_dim` in config matches the model’s output size (512 for CLIP ViT-B/32).
- [ ] Thresholds 0.97–0.99 for “near-duplicate only”; relax only if you explicitly want semantic grouping.
- [ ] After changing model or export: run `scripts/reset_embeddings_and_duplicates.sh` and re-run the server to recompute all embeddings and groups.
