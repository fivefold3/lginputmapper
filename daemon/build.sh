#!/bin/sh
# Cross-compiles lginputmapperd as fully static binaries for 32-bit and 64-bit ARM
# using zig cc (no Linux toolchain needed). Output: dist/daemon/lginputmapperd-<arch>
set -eu
OUT="${1:-dist/daemon}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
cd "$(dirname "$0")"
VERSION=$(node -p "require('$(cd .. && pwd)/package.json').version" 2>/dev/null || echo 0.0.0)
CFLAGS="-std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -static -s -fno-asynchronous-unwind-tables -DLGINPUTMAPPERD_VERSION=\"$VERSION\""
SRC="lginputmapperd.c vendor/cJSON.c"
echo "building arm (32-bit, soft-float ABI, static musl)"
zig cc -target arm-linux-musleabi -mcpu=baseline $CFLAGS -o "$OUT/lginputmapperd-arm" $SRC
echo "building aarch64 (static musl)"
zig cc -target aarch64-linux-musl $CFLAGS -o "$OUT/lginputmapperd-aarch64" $SRC
ls -la "$OUT"
