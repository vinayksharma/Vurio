#!/usr/bin/env bash
# Run dedup_server and tail its logs (for debugging processing/ONNX failures).
# Log file: /tmp/dedup-server.log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEDUP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_FILE="${LOG_FILE:-/tmp/dedup-server.log}"

cd "$DEDUP_ROOT"

# This debug runner attaches to the terminal and expects to own the machine,
# so it clears every instance first. ../../killall is the machine-wide sweep
# (./restart no longer kills anything, so strays are not cleared for us).
if pgrep -f media_dedup_server >/dev/null 2>&1; then
  echo "Stopping existing dedup_server instances..."
  "$DEDUP_ROOT/../killall" 2>/dev/null || true
  sleep 2
fi

echo "Starting dedup_server (logs also written to: $LOG_FILE)"
echo "Press Ctrl+C to stop server and tail."
echo ""

export MAGICK_DEBUG="None"
export MAGICK_THREAD_LIMIT="1"
./build/bin/media_dedup_server 2>&1 | tee "$LOG_FILE"
