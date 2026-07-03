#!/bin/bash
# Regenerates the checked-in Swift gRPC client code from dedup.proto.
#
# Requires the grpc-swift v1 generator (protoc-gen-grpc-swift); the v2
# generator (protoc-gen-grpc-swift-2) emits code that does not compile
# against the grpc-swift 1.x dependency pinned in swift_ui/Package.swift.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROTO_DIR="$REPO_ROOT/dedup_server/src/api"
OUT_DIR="$REPO_ROOT/swift_ui/Sources/swift_ui/Generated"

protoc -I "$PROTO_DIR" dedup.proto \
  --swift_out="$OUT_DIR" \
  --plugin=protoc-gen-grpc-swift=/usr/local/bin/protoc-gen-grpc-swift \
  --grpc-swift_out="$OUT_DIR"

echo "Regenerated: $OUT_DIR/dedup.pb.swift, $OUT_DIR/dedup.grpc.swift"
