#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "[1/10] auth"
"$SCRIPT_DIR/test_auth.sh"
echo

echo "[2/10] private-api"
"$SCRIPT_DIR/test_private_api.sh"
echo

echo "[3/10] files"
"$SCRIPT_DIR/test_files.sh"
echo

echo "[4/10] drive"
"$SCRIPT_DIR/test_drive.sh"
echo

echo "[5/10] ref-count"
"$SCRIPT_DIR/test_ref_count_consistency.sh"
echo

echo "[6/10] upload-race"
"$SCRIPT_DIR/test_upload_race_consistency.sh"
echo

echo "[7/10] storage-consistency"
"$SCRIPT_DIR/test_storage_consistency.sh"
echo

echo "[8/10] share"
"$SCRIPT_DIR/test_share.sh"
echo

echo "[9/10] share-race"
"$SCRIPT_DIR/test_share_race.sh"
echo

echo "[10/10] chunked-api"
"$SCRIPT_DIR/test_chunked_api.sh"
