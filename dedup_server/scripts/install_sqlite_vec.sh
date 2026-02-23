#!/usr/bin/env bash
# Download and install sqlite-vec loadable extension for media_dedup_server.
# Usage: run from repo root or dedup_server: ./scripts/install_sqlite_vec.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEDUP_SERVER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXT_DIR="$DEDUP_SERVER_DIR/extensions"
ARCH=$(uname -m)
OS=$(uname -s | tr '[:upper:]' '[:lower:]')

# Map uname to release asset suffix
case "$OS" in
  darwin)
    PLATFORM="macos"
    ;;
  linux)
    PLATFORM="linux"
    ;;
  *)
    echo "Unsupported OS: $OS"
    exit 1
    ;;
esac

case "$ARCH" in
  arm64|aarch64)
    ARCH_SUFFIX="aarch64"
    ;;
  x86_64|amd64)
    ARCH_SUFFIX="x86_64"
    ;;
  *)
    echo "Unsupported arch: $ARCH"
    exit 1
    ;;
esac

VERSION="v0.1.7-alpha.10"
TARBALL="sqlite-vec-0.1.7-alpha.10-loadable-${PLATFORM}-${ARCH_SUFFIX}.tar.gz"
URL="https://github.com/asg017/sqlite-vec/releases/download/${VERSION}/${TARBALL}"

mkdir -p "$EXT_DIR"
cd "$EXT_DIR"

echo "Downloading sqlite-vec ${VERSION} (${PLATFORM}-${ARCH_SUFFIX})..."
curl -sL -o "$TARBALL" "$URL"

echo "Extracting..."
tar -xzf "$TARBALL"
rm -f "$TARBALL"

# Find the loadable .dylib or .so
if [ -f "vec0.dylib" ]; then
  EXT_PATH="$EXT_DIR/vec0.dylib"
elif [ -f "vec0.so" ]; then
  EXT_PATH="$EXT_DIR/vec0.so"
else
  # Some releases extract into a subdir
  for f in vec0.dylib vec0.so; do
    if find . -name "$f" -type f | head -1; then
      EXT_PATH="$(cd "$EXT_DIR" && find . -name "$f" -type f | head -1)"
      EXT_PATH="$EXT_DIR/${EXT_PATH#./}"
      break
    fi
  done
fi

if [ -z "$EXT_PATH" ] || [ ! -f "$EXT_PATH" ]; then
  echo "Extension binary not found after extract. Contents of $EXT_DIR:"
  ls -la "$EXT_DIR"
  exit 1
fi

echo "Installed sqlite-vec at: $EXT_PATH"

# Output path for config (absolute)
echo ""
echo "Add to config (config/config.yaml):"
echo "  database.vector.enabled: true"
echo "  database.vector.extensionPath: \"$EXT_PATH\""
echo ""
