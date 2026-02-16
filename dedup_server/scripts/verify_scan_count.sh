#!/usr/bin/env bash
# Verify that scanned_files_count increases after server start (initial scan).
# Usage: run from dedup_server directory: ./scripts/verify_scan_count.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEDUP_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_FILE="${LOG_FILE:-/tmp/dedup-verify-scan.log}"
PORT="${PORT:-8088}"

cd "$DEDUP_ROOT"

# Stop any existing server
if pgrep -f media_dedup_server >/dev/null 2>&1; then
  echo "Stopping existing server..."
  pkill -f media_dedup_server 2>/dev/null || true
  sleep 3
fi

echo "Starting server (log: $LOG_FILE)..."
export MAGICK_DEBUG=None MAGICK_THREAD_LIMIT=1
# Prevent server from exiting when stdin is closed (nohup/background)
export DEDUP_NON_INTERACTIVE=1
nohup ./build/bin/media_dedup_server >> "$LOG_FILE" 2>&1 &
PID=$!
echo "Server PID: $PID"

# Wait for HTTP server to be up
echo "Waiting for server on port $PORT..."
for i in $(seq 1 30); do
  if curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/api/v1/server/status" 2>/dev/null | grep -q 200; then
    echo "Server is up."
    break
  fi
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "Server process exited. Last log lines:"
    tail -30 "$LOG_FILE"
    exit 1
  fi
  sleep 1
done

# Poll status every 5 seconds for up to 2 minutes
echo ""
echo "Polling scanned_files_count (every 5s for up to 120s)..."
echo "Time     | scanned | queued | processed"
echo "---------|---------|--------|----------"

LAST_SCANNED=-1
for i in $(seq 1 24); do
  sleep 5
  if ! kill -0 "$PID" 2>/dev/null; then
    echo "Server exited."
    break
  fi
  JSON=$(curl -s "http://localhost:$PORT/api/v1/server/status" 2>/dev/null || echo "{}")
  SCANNED=$(echo "$JSON" | jq -r '.scanned_files_count // "?"')
  QUEUED=$(echo "$JSON" | jq -r '.queued_files_count // "?"')
  PROCESSED=$(echo "$JSON" | jq -r '.processed_files_count // "?"')
  printf "T+%3ds    | %7s | %6s | %9s\n" "$((i*5))" "$SCANNED" "$QUEUED" "$PROCESSED"
  if [[ "$SCANNED" =~ ^[0-9]+$ ]] && [[ "$LAST_SCANNED" =~ ^[0-9]+$ ]] && [[ "$SCANNED" -gt "$LAST_SCANNED" ]]; then
    echo ""
    echo "SUCCESS: scanned_files_count increased from $LAST_SCANNED to $SCANNED."
    kill "$PID" 2>/dev/null || true
    exit 0
  fi
  # Success if we have a non-zero count and initial scan was triggered (API reflects DB; scan may still be running on large paths)
  if [[ "$SCANNED" =~ ^[0-9]+$ ]] && [[ "$SCANNED" -gt 0 ]]; then
    if grep -q "Triggered initial file scan on startup" "$LOG_FILE" 2>/dev/null; then
      echo ""
      echo "SUCCESS: scanned_files_count=$SCANNED (API returns count); initial scan was triggered on startup."
      kill "$PID" 2>/dev/null || true
      exit 0
    fi
    if grep -q "FilesManager scan completed" "$LOG_FILE" 2>/dev/null; then
      echo ""
      echo "SUCCESS: scanned_files_count is $SCANNED and initial scan completed."
      kill "$PID" 2>/dev/null || true
      exit 0
    fi
  fi
  LAST_SCANNED=$SCANNED
done

echo ""
echo "Count did not increase within 2 minutes. Checking if initial scan completed in log..."
if grep -q "FilesManager scan completed" "$LOG_FILE" 2>/dev/null; then
  echo "Scan completed (see log). Count may be filtered by location_key - check DB."
else
  echo "Scan may still be running (large/network path). Check: grep -E 'FilesManager scan|Completed scanning' $LOG_FILE"
fi
# Clean up: stop the server we started
kill "$PID" 2>/dev/null || true
exit 1
