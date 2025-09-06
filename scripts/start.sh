#!/bin/bash
set -e

print() { echo -e "[INFO] $1"; }
err() { echo -e "[ERROR] $1" >&2; }

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

print "Building (if needed)..."
if [ ! -f build/bin/media_dedup_server ]; then
  ./build.sh
fi

mkdir -p logs data config

# Derive host/port from config/config.yaml if present
HOST="localhost"
PORT=8080
if [ -f config/config.yaml ]; then
  h=$(grep -E '^[[:space:]]*server\.host[[:space:]]*:' config/config.yaml | head -n1 | awk -F: '{print $2}' | tr -d ' "') || true
  p=$(grep -E '^[[:space:]]*server\.port[[:space:]]*:' config/config.yaml | head -n1 | awk -F: '{print $2}' | tr -d ' "') || true
  [ -n "$h" ] && HOST="$h"
  [[ "$HOST" == "0.0.0.0" ]] && HOST="localhost"
  [[ "$p" =~ ^[0-9]+$ ]] && PORT="$p"
fi

print "OpenAPI: http://${HOST}:${PORT}/api/openapi.json"
print "Starting server... (Ctrl+C to stop)"
exec ./build/bin/media_dedup_server



