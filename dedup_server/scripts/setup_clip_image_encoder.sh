#!/usr/bin/env bash
# Setup CLIP ViT-B/32 image-only encoder for deduplication.
# Produces models/clip-image-vitb32.onnx (1 input, 1 output) so the server
# uses the image-only code path and embeddings are correct.
#
# Usage: from repo root or dedup_server:
#   dedup_server/scripts/setup_clip_image_encoder.sh
#   or: cd dedup_server && ./scripts/setup_clip_image_encoder.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MODELS_DIR="$PROJECT_ROOT/models"
OUTPUT_FILE="$MODELS_DIR/clip-image-vitb32.onnx"

# Require output path to be under project (safety)
case "$OUTPUT_FILE" in
  "$PROJECT_ROOT"/*) ;;
  *) echo "ERROR: output path not under project"; exit 2 ;;
esac

# Image-only encoder is typically 100-250MB. Full CLIP is ~600MB (causes wrong embeddings).
MIN_VALID_BYTES=100000
MAX_IMAGE_ONLY_BYTES=350000000  # 350MB - above this likely full CLIP

is_valid_onnx() {
  local path="$1"
  local size
  if [[ ! -f "$path" ]]; then return 1; fi
  size=$(stat -f%z "$path" 2>/dev/null || stat -c%s "$path" 2>/dev/null || echo 0)
  if [[ "$size" -lt "$MIN_VALID_BYTES" ]]; then return 1; fi
  if head -c 20 "$path" 2>/dev/null | grep -q "ONNX placeholder"; then return 1; fi
  return 0
}

is_likely_image_only() {
  local path="$1"
  local size
  [[ -f "$path" ]] || return 1
  size=$(stat -f%z "$path" 2>/dev/null || stat -c%s "$path" 2>/dev/null || echo 0)
  [[ "$size" -ge "$MIN_VALID_BYTES" && "$size" -le "$MAX_IMAGE_ONLY_BYTES" ]] || return 1
  return 0
}

cd "$PROJECT_ROOT"
mkdir -p "$MODELS_DIR"

if is_valid_onnx "$OUTPUT_FILE" && is_likely_image_only "$OUTPUT_FILE"; then
  echo "CLIP image encoder already present (image-only size): $OUTPUT_FILE"
  exit 0
fi

if is_valid_onnx "$OUTPUT_FILE" && ! is_likely_image_only "$OUTPUT_FILE"; then
  echo "NOTE: $OUTPUT_FILE is large (likely full CLIP). Server will use the image output automatically."
  echo "      For a smaller image-only model, install torch and re-run: pip3 install torch transformers pillow && $0"
  exit 0
fi

echo "Setting up CLIP ViT-B/32 image-only encoder..."
echo "Output: $OUTPUT_FILE"
echo ""

# Try local export (image-only, single output). Requires: torch, transformers, pillow
if ! python3 -c "import torch; from transformers import CLIPModel" 2>/dev/null; then
  echo "Installing Python deps (torch, transformers, pillow)..."
  pip3 install --quiet torch transformers pillow 2>/dev/null || pip install --quiet torch transformers pillow 2>/dev/null || true
fi

if python3 "$SCRIPT_DIR/export_clip_image_encoder_onnx.py" --out "$OUTPUT_FILE"; then
  if is_valid_onnx "$OUTPUT_FILE"; then
    echo "Done. Image-only encoder saved to $OUTPUT_FILE"
    exit 0
  fi
fi

# Fallback: download full CLIP ONNX from Hugging Face (server will use image output automatically)
echo "Trying fallback download (full CLIP; server will use image output)..."
HF_URL="https://huggingface.co/sayantan47/clip-vit-b32-onnx/resolve/main/onnx/model.onnx"
if command -v curl >/dev/null 2>&1; then
  if curl -sSL -L -o "$OUTPUT_FILE" --connect-timeout 15 --max-time 600 "$HF_URL"; then
    if is_valid_onnx "$OUTPUT_FILE"; then
      echo "Downloaded full CLIP ONNX to $OUTPUT_FILE (server will select image output)."
      exit 0
    fi
    rm -f "$OUTPUT_FILE"
  fi
elif command -v wget >/dev/null 2>&1; then
  if wget -q -O "$OUTPUT_FILE" --timeout=600 "$HF_URL"; then
    if is_valid_onnx "$OUTPUT_FILE"; then
      echo "Downloaded full CLIP ONNX to $OUTPUT_FILE (server will select image output)."
      exit 0
    fi
    rm -f "$OUTPUT_FILE"
  fi
fi

echo ""
echo "Setup failed. To get the recommended image-only encoder:"
echo "  pip3 install torch transformers pillow"
echo "  $0"
exit 1
