#!/usr/bin/env bash
set -euo pipefail

# Ensure ONNX models exist: try to download real CLIP model first, fall back to placeholders.
# Config uses models/clip-image-vitb32.onnx by default.

MODELS_DIR="models"
VITB32_PATH="$MODELS_DIR/clip-image-vitb32.onnx"
RN50_PATH="$MODELS_DIR/clip-RN50.onnx"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Run from project root (parent of scripts/)
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

mkdir -p "$MODELS_DIR"
cd "$ROOT_DIR"

# Return true if path looks like a real ONNX model (not our placeholder)
is_real_onnx() {
    local path="$1"
    local size
    [[ -f "$path" ]] || return 1
    size=$(wc -c < "$path" 2>/dev/null || echo 0)
    [[ "$size" -gt 100000 ]] || return 1
    ! head -c 20 "$path" | grep -q "ONNX placeholder"
}

if is_real_onnx "$VITB32_PATH"; then
    echo "Using existing model: $VITB32_PATH"
else
    echo "Downloading CLIP ViT-B/32 ONNX model..."
    if python3 "$SCRIPT_DIR/download_clip_model.py" 2>/dev/null; then
        if is_real_onnx "$VITB32_PATH"; then
            echo "Downloaded real model to $VITB32_PATH"
        else
            echo "Download produced invalid file, creating placeholder"
            create_placeholder_vitb32=1
        fi
    else
        echo "Download failed or Python/requests not available, creating placeholder"
        create_placeholder_vitb32=1
    fi
    if [[ -n "${create_placeholder_vitb32:-}" ]]; then
        cat > "$VITB32_PATH" << 'EOF'
ONNX placeholder model for testing
Replace with actual CLIP ViT-B/32 ONNX model for production use
EOF
    fi
fi

# clip-RN50: use placeholder if missing (no auto-download URL in tree)
if [[ ! -f "$RN50_PATH" ]] || head -c 20 "$RN50_PATH" | grep -q "ONNX placeholder"; then
    echo "Creating placeholder for $RN50_PATH (run ./scripts/fetch_clip_rn50_onnx.sh <URL> for real model)"
    cat > "$RN50_PATH" << 'EOF'
ONNX placeholder model for testing
Replace with actual CLIP RN50 ONNX model for production use
EOF
fi

echo "Models ready in $MODELS_DIR/"
