#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "[1/4] auth"
"$SCRIPT_DIR/test_auth.sh"
echo

echo "[2/4] private-api"
"$SCRIPT_DIR/test_private_api.sh"
echo

echo "[3/4] files"
"$SCRIPT_DIR/test_files.sh"
echo

echo "[4/4] chunked-api"
"$SCRIPT_DIR/test_chunked_api.sh"
