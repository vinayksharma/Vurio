#!/bin/zsh

set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# Nothing is killed on the way in: several UI instances (one per worktree) are
# meant to run at once, and the pkill patterns this used to run matched every
# one of them, not just this directory's. Use ../killall at the repo root to
# sweep every server and UI when something has been left behind.
echo "Building application..."
cd "$APP_DIR"

# Check if bun is available
if command -v bun &> /dev/null; then
  echo "Using Bun..."
  bun run bun:build
  echo "Starting application..."
  NODE_ENV=production bun run electron
else
  echo "Using npm..."
  npm run build
  echo "Starting application..."
  NODE_ENV=production npm run electron
fi
