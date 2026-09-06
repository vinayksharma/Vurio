#!/bin/bash
# Full reset: clear duplicate data AND image embeddings, then mark all scanned files
# for re-processing. Use this after fixing the embedding model (e.g. ONNX output selection)
# so that the server recomputes embeddings and duplicate groups from scratch.
#
# Usage: ./scripts/reset_embeddings_and_duplicates.sh

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
DB_PATH="$PROJECT_ROOT/data/dedup_server.db"

echo "=========================================="
echo "  Full reset: embeddings + duplicates"
echo "=========================================="
echo ""

if [ ! -f "$DB_PATH" ]; then
    echo "[ERROR] Database not found at: $DB_PATH"
    exit 1
fi

echo "[INFO] Database path: $DB_PATH"
echo ""
echo "[WARNING] This will:"
echo "  - Delete all duplicate groups and members"
echo "  - Delete all image embeddings (image_artifacts)"
echo "  - Mark all scanned files for re-processing (processed=0)"
echo "  - Server will re-embed and re-run duplicate detection"
echo ""
read -p "Continue? (yes/no): " confirmation

if [ "$confirmation" != "yes" ]; then
    echo "[INFO] Cancelled"
    exit 0
fi

echo ""
echo "[INFO] Resetting..."

sqlite3 "$DB_PATH" <<EOF
-- Duplicate data
DELETE FROM duplicate_members;
DELETE FROM duplicate_groups;
DELETE FROM duplicate_processing_checkpoint;
-- Embeddings (so they are recomputed with correct ONNX output)
DELETE FROM image_artifacts;
-- Mark all scanned files for re-processing
UPDATE scanned_files SET processed = 0;
VACUUM;
EOF

echo ""
echo "[SUCCESS] Full reset done."
echo ""
echo "Next steps:"
echo "  1. Stop the server if it is running (e.g. ./scripts/run.sh stop, or ../killall for every instance)"
echo "  2. Rebuild if you changed code: cmake --build build"
echo "  3. Start the server; it will re-process files and recompute embeddings"
echo "  4. Check logs for: 'ONNX model outputs' and 'Using output index ... name=...'"
echo ""
