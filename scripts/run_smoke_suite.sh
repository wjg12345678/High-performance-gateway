#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "[1/6] auth"
"$SCRIPT_DIR/test_auth.sh"
echo

echo "[2/6] private-api"
"$SCRIPT_DIR/test_private_api.sh"
echo

echo "[3/6] files"
"$SCRIPT_DIR/test_files.sh"
echo

echo "[4/6] drive"
"$SCRIPT_DIR/test_drive.sh"
echo

echo "[5/6] share"
"$SCRIPT_DIR/test_share.sh"
echo

echo "[6/6] chunked-api"
"$SCRIPT_DIR/test_chunked_api.sh"
