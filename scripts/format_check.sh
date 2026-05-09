#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-check}"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format is required" >&2
    exit 127
fi

mapfile -t FILES < <(find "$ROOT_DIR/app" "$ROOT_DIR/http" "$ROOT_DIR/infra" "$ROOT_DIR/repo" "$ROOT_DIR/service" "$ROOT_DIR/tests" \
    -type f \( -name '*.cpp' -o -name '*.h' \) | sort)

if [ "$MODE" = "fix" ]; then
    clang-format -i "${FILES[@]}"
    exit 0
fi

if [ "$MODE" != "check" ]; then
    echo "usage: $0 [check|fix]" >&2
    exit 2
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT
failed=0
for file in "${FILES[@]}"; do
    formatted="$tmp_dir/$(basename "$file")"
    clang-format "$file" > "$formatted"
    if ! diff -u "$file" "$formatted" >/dev/null; then
        echo "needs formatting: ${file#$ROOT_DIR/}" >&2
        failed=1
    fi
done
exit "$failed"
