#!/bin/bash
# build.sh - canonical build for odm-git.
# ALWAYS build with this (not bare `make`) so EIPScanner is rebuilt from source
# and installed BEFORE the EPICS app links against it. This prevents the
# stale-library trap (a committed/old libEIPScannerS.a drifting from source),
# which nearly shipped a Path-A-less EIPScanner to production (2026-09).
set -euo pipefail
TOP="$(cd "$(dirname "$0")" && pwd)"

echo "=== [1/3] Building EIPScanner (private fork) from source ==="
cd "$TOP/EIPScanner"
# -S/-B regenerate the cache in THIS tree every time (prevents cross-tree
# CMakeCache cross-wiring, which also bit us).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$TOP/EIPScanner/install"
cmake --build build
cmake --install build

echo "=== [2/3] EIPScanner provenance ==="
ls -la "$TOP/EIPScanner/install/lib/libEIPScannerS.a"
# Sanity: Path A must be in the source that was just built:
if grep -q '"0.0.0.0", EIP_DEFAULT_IMPLICIT_PORT' \
     "$TOP/EIPScanner/src/ConnectionManager.cpp"; then
    echo "  Path A (shared 0.0.0.0:2222 socket) present in source. OK."
else
    echo "  WARNING: Path A NOT found in ConnectionManager.cpp source!" >&2
fi

echo "=== [3/3] Building EPICS app ==="
cd "$TOP"
make "$@"

echo "=== Build complete. ==="
