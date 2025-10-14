#!/usr/bin/env bash
set -euo pipefail

# Fetch a CLIP RN50 ONNX model into the local models/ directory
# Usage:
#   scripts/fetch_clip_rn50_onnx.sh <MODEL_URL> [<DEST_PATH>]
# Example:
#   scripts/fetch_clip_rn50_onnx.sh https://example.com/clip-RN50.onnx

if [[ ${1:-} == "" ]]; then
  echo "ERROR: MODEL_URL is required"
  echo "Usage: $0 <MODEL_URL> [<DEST_PATH>]"
  exit 1
fi

MODEL_URL="$1"
DEST_PATH="${2:-models/clip-RN50.onnx}"

mkdir -p "$(dirname "$DEST_PATH")"

echo "Downloading CLIP RN50 ONNX from: $MODEL_URL"
if command -v curl >/dev/null 2>&1; then
  curl -L "$MODEL_URL" -o "$DEST_PATH"
elif command -v wget >/dev/null 2>&1; then
  wget -O "$DEST_PATH" "$MODEL_URL"
else
  echo "ERROR: Neither curl nor wget is available. Please install one of them."
  exit 2
fi

if [[ ! -s "$DEST_PATH" ]]; then
  echo "ERROR: Download failed or empty file at $DEST_PATH"
  exit 3
fi

echo "Model downloaded to: $DEST_PATH"
echo "Set config property media.image.quality.onnx.modelPath to: $DEST_PATH"













