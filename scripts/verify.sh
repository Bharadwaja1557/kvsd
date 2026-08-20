#!/usr/bin/env bash
# Clean configure + build + full test suite in one command. Mirrors what CI runs,
# so "verify.sh passed locally" and "CI is green" mean the same thing.
set -euo pipefail

cd "$(dirname "$0")/.."

BUILD_DIR=${BUILD_DIR:-build}

rm -rf "$BUILD_DIR"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DKVSD_WERROR=ON
cmake --build "$BUILD_DIR" -j"$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu )"
ctest --test-dir "$BUILD_DIR" --output-on-failure
