# Duplicate Detection Tuning Guide

## Overview

This guide helps you tune the duplicate detection system for optimal accuracy based on your specific image dataset and requirements. The system uses three different modes (FAST, BALANCED, QUALITY), each with configurable parameters to balance precision (avoiding false positives) and recall (catching all true duplicates).

## Quick Start - Recommended Settings

For most use cases, use these tested configurations:

```yaml
# FAST Mode - Perceptual Hash (best for exact/near-exact duplicates)
duplicates.fast.threshold: 0.92 # 5 bits can differ out of 64
duplicates.fast.minHashSize: 64 # Standard pHash size

# BALANCED Mode - Feature Matching (best for edited/cropped versions)
duplicates.balanced.threshold: 0.35 # 35% of features must match
duplicates.balanced.minGoodMatches: 15 # At least 15 good feature matches required
duplicates.balanced.ratioTestThreshold: 0.75 # Lowe's ratio test (stricter = higher)
media.image.balanced.maxKeypoints: 1500 # Detect up to 1500 features per image

# QUALITY Mode - Deep Learning Embeddings (best for semantic similarity)
duplicates.quality.threshold: 0.96 # 96% cosine similarity required
duplicates.quality.minConfidence: 0.90 # Minimum confidence threshold
```

## Understanding Each Mode

### FAST Mode - Perceptual Hash (pHash)

**How it works:**

- Converts image to grayscale, resizes to 32×32
- Applies DCT (Discrete Cosine Transform)
- Generates 64-bit hash from frequency components
- Compares hashes using Hamming distance

**What it catches:**
✅ Exact duplicates
✅ Resized versions
✅ Minor compression changes
✅ Slight color adjustments

**What it misses:**
❌ Cropped images
❌ Rotated images (>15°)
❌ Heavily edited versions
❌ Images with different aspect ratios

**Threshold Guide:**

| Threshold | Bits Different | Use Case                                          |
| --------- | -------------- | ------------------------------------------------- |
| 0.95      | 3 bits         | Very strict - only near-identical                 |
| 0.92      | 5 bits         | **Recommended** - catches compression/minor edits |
| 0.90      | 6 bits         | Looser - more false positives possible            |
| 0.85      | 10 bits        | Too loose - many false positives                  |

**Example:**

- Original: IMG_001.jpg (2MB, full quality)
- Compressed: IMG_001_compressed.jpg (200KB, 80% quality)
- Similarity: ~0.94 ✅ (Caught with threshold 0.92)

---

### BALANCED Mode - ORB Feature Matching

**How it works:**

- Detects up to 1500 keypoints (corners, edges)
- Generates 256-bit ORB descriptors for each keypoint
- Matches descriptors using Hamming distance
- Applies Lowe's ratio test to filter bad matches
- Similarity = good_matches / max(features1, features2)

**What it catches:**
✅ Cropped images
✅ Rotated images
✅ Scaled images
✅ Partially occluded images
✅ Images with different borders/frames

**What it misses:**
❌ Uniform/texture-less images (not enough features)
❌ Heavily filtered images
❌ Completely different perspectives
❌ Images with < 15 features

**Parameters:**

1. **threshold** (0.35 recommended):

   - Percentage of features that must match
   - 0.30: Looser, catches more variations
   - 0.35: Balanced, good precision/recall
   - 0.40: Stricter, fewer false positives

2. **minGoodMatches** (15 recommended):

   - Absolute minimum number of good feature matches
   - Prevents false positives with feature-poor images
   - 10: Minimum for basic confirmation
   - 15: Recommended for good confidence
   - 20: Stricter, may miss some true duplicates

3. **ratioTestThreshold** (0.75 recommended):

   - Lowe's ratio test: best_dist / second_best_dist
   - 0.60: Very strict - only unambiguous matches
   - 0.75: **Recommended** - good balance
   - 0.85: Looser - more matches but less reliable

4. **maxKeypoints** (1500 recommended):
   - Maximum features to detect per image
   - 500: Fast but may miss details
   - 1000: Default balance
   - 1500: **Recommended** - better coverage
   - 2000: Slower but maximum coverage

**Example:**

- Original: DSC_0001.jpg (3000×2000)
- Cropped: DSC_0001_cropped.jpg (1500×1000, center crop)
- Features matched: 180 out of 450 = 0.40 ✅
- Good matches: 25 ✅ (exceeds minGoodMatches=15)
- Result: Caught as duplicate

---

### QUALITY Mode - CLIP Embeddings

**How it works:**

- Passes image through CLIP Vision Transformer
- Generates 512-dimensional semantic embedding
- Compares embeddings using cosine similarity
- Understands image content, not just pixels

**What it catches:**
✅ Semantically similar images
✅ Same scene, different angles
✅ Same subject, different crops
✅ Images edited heavily but same content
✅ RAW vs JPEG of same photo

**What it misses:**
❌ Nothing (most comprehensive)
❌ May group semantically similar but visually different
(e.g., different photos of same person)

**Parameters:**

1. **threshold** (0.96 recommended):

   - Cosine similarity required
   - 0.98: Very strict - only near-duplicates
   - 0.96: **Recommended** - catches true duplicates
   - 0.94: Looser - may group similar but non-duplicate images
   - 0.90: Too loose - groups semantically similar images

2. **minConfidence** (0.90 recommended):
   - Secondary confidence threshold
   - Reserved for future metadata filtering
   - Currently not enforced

**Example:**

- Original: IMG_5432.CR2 (RAW, 25MB, 6000×4000)
- JPEG Export: IMG_5432.jpg (JPEG, 3MB, 4000×2667, cropped + edited)
- Embedding similarity: 0.97 ✅
- Result: Caught as duplicate despite different format/size/crop

---

## Tuning Methodology

### Step 1: Start with Recommended Defaults

Use the recommended settings above and process your dataset.

### Step 2: Evaluate Results

Query your duplicate groups:

```bash
# Count groups by size
sqlite3 data/dedup_server.db "
SELECT member_count, COUNT(*) as num_groups
FROM duplicate_groups
WHERE mode='FAST'
GROUP BY member_count
ORDER BY member_count;
"

# Check largest groups (potential false positives)
sqlite3 data/dedup_server.db "
SELECT id, mode, member_count, representative_file_path
FROM duplicate_groups
ORDER BY member_count DESC
LIMIT 10;
"
```

### Step 3: Identify Issues

**Too many false positives (unrelated images grouped):**

- FAST: Increase threshold (0.92 → 0.93 → 0.94)
- BALANCED: Increase threshold (0.35 → 0.38 → 0.40)
  OR increase minGoodMatches (15 → 20 → 25)
- QUALITY: Increase threshold (0.96 → 0.97 → 0.98)

**Too many false negatives (true duplicates missed):**

- FAST: Decrease threshold (0.92 → 0.90 → 0.88)
- BALANCED: Decrease threshold (0.35 → 0.32 → 0.30)
  OR decrease minGoodMatches (15 → 12 → 10)
- QUALITY: Decrease threshold (0.96 → 0.95 → 0.94)

### Step 4: Iterative Refinement

1. Adjust one parameter at a time
2. Reset database: `./scripts/reset_duplicates.sh`
3. Restart server and wait for processing
4. Evaluate results
5. Repeat until satisfied

## Common Scenarios & Solutions

### Scenario 1: Photography Workflow

**Use case:** Photographer with RAW + JPEG exports

**Recommended settings:**

```yaml
server.mode: quality # Use QUALITY mode
duplicates.quality.threshold: 0.96 # Catches RAW/JPEG pairs
```

**Why:** CLIP embeddings understand that RAW and its JPEG export are the same image, even with different formats, sizes, and edits.

---

### Scenario 2: Downloaded Images

**Use case:** Images downloaded from internet, many duplicates with different compression

**Recommended settings:**

```yaml
server.mode: fast # Use FAST mode
duplicates.fast.threshold: 0.90 # Catches compression variations
```

**Why:** pHash is fast and catches different compression levels efficiently.

---

### Scenario 3: Edited Photos

**Use case:** Photos with crops, filters, adjustments

**Recommended settings:**

```yaml
server.mode: balanced # Use BALANCED mode
duplicates.balanced.threshold: 0.30 # Looser for crops
duplicates.balanced.minGoodMatches: 20 # Ensure good match quality
media.image.balanced.maxKeypoints: 1500 # More features for better matching
```

**Why:** Feature matching is robust to crops and minor edits.

---

### Scenario 4: Mixed Dataset

**Use case:** Mix of exact duplicates, compressed versions, and edited photos

**Recommended approach:** Run all three modes

1. Run FAST mode first (catches exact/compressed duplicates)
2. Run BALANCED mode (catches crops/edits not found by FAST)
3. Run QUALITY mode (catches semantic duplicates missed by both)

**Note:** Each mode maintains separate groups. You can query by mode.

---

## Advanced Tuning

### Precision vs Recall Trade-off

**High Precision (few false positives):**

```yaml
# Strict settings
duplicates.fast.threshold: 0.94
duplicates.balanced.threshold: 0.40
duplicates.balanced.minGoodMatches: 25
duplicates.quality.threshold: 0.98
```

**High Recall (catch all duplicates):**

```yaml
# Loose settings
duplicates.fast.threshold: 0.88
duplicates.balanced.threshold: 0.28
duplicates.balanced.minGoodMatches: 10
duplicates.quality.threshold: 0.94
```

### Dataset-Specific Tuning

**Large homogeneous dataset (similar types of images):**

- Use stricter thresholds to avoid false positives
- Increase minGoodMatches
- Use FAST mode for speed

**Small heterogeneous dataset (varied image types):**

- Use looser thresholds to catch edge cases
- Decrease minGoodMatches
- Use QUALITY mode for accuracy

**Images with text/logos:**

- Prefer BALANCED mode (features on text/logos)
- Increase maxKeypoints to 2000
- Threshold: 0.40 (stricter)

**Nature/landscape photos:**

- Prefer QUALITY mode (semantic understanding)
- Threshold: 0.95 (can be looser - scenery is distinct)

## Monitoring & Debugging

### Enable Debug Logging

```yaml
logging.level: .debug # See similarity scores
```

Look for log messages like:

- "pHash similarity: 0.9234 (hamming distance=5 bits out of 64)"
- "Feature similarity (ORB): 0.372 (25 good matches out of 450/380 features)"
- "Embedding similarity: 0.9634 (cosine similarity, dim=512)"

### Analyze Similarity Distribution

Run after processing to see similarity score distribution:

```bash
# Get similarity scores from duplicate_members table
sqlite3 data/dedup_server.db "
SELECT
    ROUND(similarity_score, 2) as score,
    COUNT(*) as count
FROM duplicate_members
WHERE is_representative = 0
GROUP BY ROUND(similarity_score, 2)
ORDER BY score DESC;
"
```

This shows how many files were matched at each similarity level.

### Identify Edge Cases

```bash
# Find groups with unusually high member counts (potential false positives)
sqlite3 data/dedup_server.db "
SELECT id, mode, member_count, representative_file_path
FROM duplicate_groups
WHERE member_count > 10
ORDER BY member_count DESC;
"
```

Manually review these large groups to check for false positives.

## Threshold Calculation Formulas

### FAST Mode - pHash

```
Hamming Distance = number of differing bits
Similarity = 1.0 - (hamming_distance / 64)

Threshold 0.92 means: 64 × (1 - 0.92) = 5.12 bits can differ
```

**Recommended range:** 4-6 bits difference (thresholds 0.90-0.94)

### BALANCED Mode - Features

```
Good Match Criteria:
  best_distance / second_best_distance < ratioTestThreshold

Similarity = good_matches / max(features1_count, features2_count)
```

**Recommended:**

- threshold: 0.30-0.40 (30-40% features match)
- minGoodMatches: 15-25 (absolute minimum)
- ratioTestThreshold: 0.70-0.80 (Lowe's ratio)

### QUALITY Mode - Embeddings

```
Cosine Similarity = (A · B) / (||A|| × ||B||)

Where A and B are 512-dimensional CLIP embeddings
```

**Recommended range:** 0.94-0.98

- < 0.94: Too loose (groups semantically similar images)
- > 0.98: Too strict (misses some true duplicates)

## Performance vs Accuracy

### Execution Speed vs Quality

| Mode         | Speed     | Accuracy | Memory | Use When                      |
| ------------ | --------- | -------- | ------ | ----------------------------- |
| **FAST**     | Very Fast | Good     | Low    | Exact/compressed duplicates   |
| **BALANCED** | Medium    | Better   | Medium | Crops/edits/transformations   |
| **QUALITY**  | Slower    | Best     | High   | RAW/JPEG, semantic duplicates |

### Improving Detection Quality

**Without sacrificing speed:**

1. Tune thresholds carefully (test on sample dataset)
2. Increase maxKeypoints for BALANCED (1500-2000)
3. Enable minGoodMatches filtering
4. Use appropriate mode for your image types

**If speed is not a concern:**

1. Use QUALITY mode for everything
2. Lower thresholds slightly
3. Process in multiple passes (FAST → BALANCED → QUALITY)

## Testing & Validation

### Create a Test Set

1. Select 10-20 known duplicate pairs from your dataset
2. Note their file paths
3. Run duplicate detection
4. Verify they were grouped correctly

### Measure Precision & Recall

**Precision** = true_positives / (true_positives + false_positives)

- How many detected duplicates are actually duplicates?
- High precision = few false positives

**Recall** = true_positives / (true_positives + false_negatives)

- How many actual duplicates were detected?
- High recall = caught most duplicates

**F1 Score** = 2 × (precision × recall) / (precision + recall)

- Balanced metric combining both

### Example Test Script

```bash
#!/bin/bash
# Test duplicate detection accuracy

# Known duplicate pairs (adjust paths to your dataset)
declare -a TEST_PAIRS=(
    "/path/to/img1.jpg:/path/to/img1_copy.jpg"
    "/path/to/img2.jpg:/path/to/img2_edited.jpg"
    "/path/to/img3.cr2:/path/to/img3.jpg"
)

# Query database for each pair
for pair in "${TEST_PAIRS[@]}"; do
    IFS=':' read -ra PATHS <<< "$pair"
    file1="${PATHS[0]}"
    file2="${PATHS[1]}"

    # Check if they're in the same group
    group1=$(sqlite3 data/dedup_server.db "
        SELECT dm.group_id FROM duplicate_members dm
        JOIN scanned_files sf ON dm.file_id = sf.id
        WHERE sf.file_path = '$file1';
    ")

    group2=$(sqlite3 data/dedup_server.db "
        SELECT dm.group_id FROM duplicate_members dm
        JOIN scanned_files sf ON dm.file_id = sf.id
        WHERE sf.file_path = '$file2';
    ")

    if [ "$group1" = "$group2" ] && [ -n "$group1" ]; then
        echo "✓ MATCH: $file1 and $file2 in group $group1"
    else
        echo "✗ MISS: $file1 (group $group1) and $file2 (group $group2)"
    fi
done
```

## Troubleshooting

### Issue: No duplicates found

**Possible causes:**

1. Thresholds too strict
2. Files not processed yet (check processing_status)
3. Artifacts missing (check image_artifacts table)

**Solutions:**

1. Lower thresholds slightly
2. Wait for processing to complete
3. Check logs for processing errors

### Issue: Too many false positives

**Possible causes:**

1. Thresholds too loose
2. Similar-looking but different images
3. Insufficient feature discriminatio

**Solutions:**

1. Increase thresholds
2. Increase minGoodMatches (BALANCED mode)
3. Switch to stricter mode (FAST → BALANCED → QUALITY)

### Issue: Large groups with unrelated images

**Possible causes:**

1. Threshold = 0 (everything matches everything!)
2. Transitive grouping bug (fixed in v3)
3. Insufficient variation in dataset

**Solutions:**

1. Check config: threshold should be 0.90-0.98, NOT 0
2. Ensure using v3 algorithm (representative-based)
3. Increase threshold to be more selective

### Issue: True duplicates not grouped

**Possible causes:**

1. Different file formats (RAW vs JPEG) with FAST mode
2. Heavy edits with FAST mode
3. Threshold too strict

**Solutions:**

1. Use QUALITY mode for cross-format duplicates
2. Use BALANCED mode for edited images
3. Lower threshold slightly

## Mode Selection Guide

### Decision Tree

```
Are images exact/near-exact copies?
├─ YES → Use FAST mode (threshold: 0.92)
└─ NO ↓

Are images cropped/rotated versions?
├─ YES → Use BALANCED mode (threshold: 0.35)
└─ NO ↓

Are images different formats/edits of same photo?
├─ YES → Use QUALITY mode (threshold: 0.96)
└─ NO → Images are probably NOT duplicates
```

### Hybrid Approach

For best results, use multiple modes in sequence:

**Pass 1: FAST mode**

- Catches exact/compressed duplicates quickly
- Threshold: 0.92

**Pass 2: BALANCED mode**

- Catches crops/edits missed by FAST
- Threshold: 0.35

**Pass 3: QUALITY mode**

- Catches semantic duplicates missed by both
- Threshold: 0.96

**Result:** Maximum coverage with good precision

## Configuration Reference

### Full Example Configuration

```yaml
# === Duplicate Detection Settings ===

# Core settings
duplicates.finder.enabled: true
duplicates.finder.batchSize: 1000
duplicates.finder.intervalMs: 3600000 # Run hourly
duplicates.finder.maxGroupSize: 100

# Representative selection
duplicates.representative.strategy: size_then_age # Larger files preferred, then older

# FAST mode - pHash
duplicates.fast.threshold: 0.92
duplicates.fast.minHashSize: 64

# BALANCED mode - ORB features
duplicates.balanced.threshold: 0.35
duplicates.balanced.minGoodMatches: 15
duplicates.balanced.ratioTestThreshold: 0.75
media.image.balanced.maxKeypoints: 1500
media.image.balanced.resizeLongEdge: 1024

# QUALITY mode - CLIP embeddings
duplicates.quality.threshold: 0.96
duplicates.quality.minConfidence: 0.90
media.image.quality.embeddingDim: 512
media.image.quality.onnx.modelPath: models/clip-image-vitb32.onnx
```

## Real-World Examples

### Example Dataset Results

**Test dataset:** 21,089 processed images

**With threshold = 0 (broken):**

- FAST: 147 groups, avg 142 members, max 764 members ❌
- Result: One massive group with unrelated images

**With threshold = 0.92 (fixed):**

- FAST: ~1,500 groups, avg 2-3 members, max 15 members ✅
- Result: Proper grouping of duplicates only

### Validation Examples

Based on your provided duplicate image paths:

**Set 1: JPG duplicates**

```
/Users/vinaysharma/Pictures/Images/JPG/Large/f12933976.jpg
/Users/vinaysharma/Pictures/Images/JPG/Large/f55318080.jpg
/Users/vinaysharma/Pictures/Images/JPG/Large/f183439128.jpg
/Users/vinaysharma/Pictures/Images/JPG/Large/f412507032.jpg
```

Expected: Should be grouped together with FAST mode (threshold 0.92)

**Set 2: CR2/RAW duplicates**

```
/Users/vinaysharma/Pictures/Images/CR2/i31159.tif.cr2
/Users/vinaysharma/Pictures/Images/Canon RAW/Valid/f387703040.cr2
/Users/vinaysharma/Pictures/Images/CR2/i28200.tif.cr2
/Users/vinaysharma/Pictures/Images/CR2/i36818.tif.cr2
```

Expected: Should be grouped with QUALITY mode (threshold 0.96)

## References

- Perceptual Hashing: [pHash Algorithm](http://www.phash.org/)
- Feature Matching: [Lowe's Ratio Test](https://www.cs.ubc.ca/~lowe/papers/ijcv04.pdf)
- CLIP Embeddings: [OpenAI CLIP](https://github.com/openai/CLIP)
- ORB Features: [OpenCV ORB Documentation](https://docs.opencv.org/4.x/d1/d89/tutorial_py_orb.html)

## Support

For issues or questions:

1. Check logs in server output
2. Query duplicate_groups table for current state
3. Review similarity scores in duplicate_members table
4. Consult main architecture doc: `docs/duplicate_detection_architecture.md`

---

**Last Updated:** October 2025  
**Version:** v3 (Representative-Based Algorithm)
