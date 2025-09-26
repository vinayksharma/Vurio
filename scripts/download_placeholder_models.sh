#!/usr/bin/env bash
set -euo pipefail

# Download placeholder ONNX models for testing
# These are small placeholder files that allow the build to complete
# For production use, replace with actual CLIP ONNX models

MODELS_DIR="models"
mkdir -p "$MODELS_DIR"

echo "Creating placeholder ONNX models for testing..."

# Create a minimal ONNX file (just a header)
cat > "$MODELS_DIR/clip-image-vitb32.onnx" << 'EOF'
ONNX placeholder model for testing
Replace with actual CLIP ViT-B/32 ONNX model for production use
EOF

cat > "$MODELS_DIR/clip-RN50.onnx" << 'EOF'
ONNX placeholder model for testing
Replace with actual CLIP RN50 ONNX model for production use
EOF

echo "Placeholder models created in $MODELS_DIR/"
echo "For production use, download real CLIP ONNX models:"
echo "  - CLIP ViT-B/32: https://huggingface.co/microsoft/clip-vit-base-patch32"
echo "  - CLIP RN50: https://huggingface.co/microsoft/clip-rn50"
echo ""
echo "Or use the provided scripts:"
echo "  python3 scripts/fetch_clip_from_hub.py"
echo "  ./scripts/fetch_clip_rn50_onnx.sh <MODEL_URL>"
