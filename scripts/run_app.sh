#!/bin/zsh

set -euo pipefail

SERVER_URL="http://localhost:8088/swagger.json"
SERVER_RUNNER="/Users/vinaysharma/developer/dedup-server/run.sh"
APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"

is_server_up() {
  curl -s --max-time 2 "$SERVER_URL" >/dev/null 2>&1
}

echo "Checking server..."
if ! is_server_up; then
  echo "Server not running. Starting server..."
  if [ -x "$SERVER_RUNNER" ]; then
    nohup "$SERVER_RUNNER" -s >/tmp/dedup-server.log 2>&1 &
    echo $! > /tmp/dedup-server.pid
  else
    echo "Server run script not found or not executable: $SERVER_RUNNER"
    exit 1
  fi
  echo "Waiting for server to come online..."
  for i in {1..20}; do
    if is_server_up; then
      echo "Server is online."
      break
    fi
    sleep 0.5
  done
fi

echo "Starting Electron app..."
cd "$APP_DIR"

# Check if bun is available
if command -v bun &> /dev/null; then
  echo "Using Bun..."
  bun run bun:build 2>&1 | tee /tmp/dedup-app-build.log
  bun run electron 2>&1 | tee /tmp/dedup-app.log
else
  echo "Using npm..."
  npm run build 2>&1 | tee /tmp/dedup-app-build.log
  npm run electron 2>&1 | tee /tmp/dedup-app.log
fi
