#!/usr/bin/env bash
# Run dedup_server and tail its logs (for debugging processing/ONNX failures).
# Log file: /tmp/dedup-server.log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEDUP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_FILE="${LOG_FILE:-/tmp/dedup-server.log}"

cd "$DEDUP_ROOT"

# Stop any existing instance
if pgrep -f media_dedup_server >/dev/null 2>&1; then
  echo "Stopping existing dedup_server..."
  "$SCRIPT_DIR/stopall" 2>/dev/null || true
  sleep 2
fi

echo "Starting dedup_server (logs also written to: $LOG_FILE)"
echo "Press Ctrl+C to stop server and tail."
echo ""

export MAGICK_DEBUG="None"
export MAGICK_THREAD_LIMIT="1"
./build/bin/media_dedup_server 2>&1 | tee "$LOG_FILE"
