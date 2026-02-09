#!/usr/bin/env bash
# Start the Media Deduplication Server in a new Terminal window.
# Use this when you want the server to run in its own terminal (e.g. from restart script).

set -euo pipefail

DEDUP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [[ ! -x "$DEDUP_ROOT/build/bin/media_dedup_server" ]]; then
  echo "Server binary not found. Build first: cd $DEDUP_ROOT && ./build.sh" >&2
  exit 1
fi

# macOS: open a new Terminal window and run ./start from dedup_server
case "$(uname -s)" in
  Darwin)
    osascript -e 'tell application "Terminal" to do script "cd \"'"$DEDUP_ROOT"'\" && ./start"'
    ;;
  *)
    echo "Opening in new terminal is only supported on macOS. Run: cd $DEDUP_ROOT && ./start" >&2
    exit 1
    ;;
esac
