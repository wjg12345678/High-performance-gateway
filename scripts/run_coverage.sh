#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-coverage}"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DATLAS_ENABLE_COVERAGE=ON
cmake --build "$BUILD_DIR" --target atlas-unit-tests --parallel "${BUILD_PARALLEL:-2}"
ctest --test-dir "$BUILD_DIR" --output-on-failure

if command -v gcovr >/dev/null 2>&1; then
    gcovr --root "$ROOT_DIR" --filter "$ROOT_DIR/http/core" --filter "$ROOT_DIR/http/files" --print-summary --html-details "$BUILD_DIR/coverage.html"
    echo "coverage html: $BUILD_DIR/coverage.html"
elif command -v lcov >/dev/null 2>&1; then
    lcov --capture --directory "$BUILD_DIR" --output-file "$BUILD_DIR/coverage.info"
    lcov --summary "$BUILD_DIR/coverage.info"
else
    echo "coverage data generated in $BUILD_DIR; install gcovr or lcov for reports" >&2
fi
