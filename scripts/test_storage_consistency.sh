#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

UPLOAD_ROOT="${UPLOAD_ROOT:-$SCRIPT_DIR/../webroot/uploads}"
CHECK_SCRIPT="$SCRIPT_DIR/check_storage_consistency.sh"
TEST_ID="$(date +%s)-$$"
TMP_FILE="$(make_demo_file)"
TOKEN=""
FILE_ID=""
PURGED_FILE_ID=""
ORPHAN_NAME="storage-orphan-$TEST_ID.tmp"

cleanup() {
    rm -f "$TMP_FILE"
    rm -f "$UPLOAD_ROOT/$ORPHAN_NAME"
    if [ -n "$TOKEN" ] && [ -n "$FILE_ID" ]; then
        curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID" -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1 || true
        curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID/permanent" -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

fail() {
    echo "$1"
    exit 1
}

[ -x "$CHECK_SCRIPT" ] || fail "storage consistency checker is not executable: $CHECK_SCRIPT"

wait_for_server
register_user

LOGIN_INFO="$(require_token)"
TOKEN="$(printf '%s\n' "$LOGIN_INFO" | sed -n '2p')"

UPLOAD_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Expect:" \
    -F "file=@$TMP_FILE;type=text/plain" \
    -F "filename=storage-consistency-$TEST_ID.txt" \
    -F "is_public=false")"
FILE_ID="$(extract_file_id "$UPLOAD_RESPONSE")"
if [ -z "$FILE_ID" ]; then
    fail "upload failed: $UPLOAD_RESPONSE"
fi

CLEAN_OUTPUT="$("$CHECK_SCRIPT" --dry-run)"
case "$CLEAN_OUTPUT" in
    *"storage consistency ok"*) ;;
    *) fail "expected clean storage before orphan injection: $CLEAN_OUTPUT" ;;
esac

mkdir -p "$UPLOAD_ROOT"
printf 'orphan %s\n' "$TEST_ID" > "$UPLOAD_ROOT/$ORPHAN_NAME"

set +e
DRY_RUN_OUTPUT="$("$CHECK_SCRIPT" --dry-run 2>&1)"
DRY_RUN_STATUS="$?"
set -e

if [ "$DRY_RUN_STATUS" -eq 0 ]; then
    fail "dry-run should report injected orphan, got success: $DRY_RUN_OUTPUT"
fi
case "$DRY_RUN_OUTPUT" in
    *"$ORPHAN_NAME"*) ;;
    *) fail "dry-run did not report injected orphan: $DRY_RUN_OUTPUT" ;;
esac

FIX_OUTPUT="$("$CHECK_SCRIPT" --fix-orphans)"
case "$FIX_OUTPUT" in
    *"storage consistency ok"*) ;;
    *) fail "fix-orphans should restore clean storage: $FIX_OUTPUT" ;;
esac
[ ! -e "$UPLOAD_ROOT/$ORPHAN_NAME" ] || fail "orphan file still exists after fix: $UPLOAD_ROOT/$ORPHAN_NAME"

DELETE_RESPONSE="$(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID" -H "Authorization: Bearer $TOKEN")"
case "$DELETE_RESPONSE" in
    *"file moved to recycle bin"*) ;;
    *) fail "soft delete failed: $DELETE_RESPONSE" ;;
esac

PURGE_RESPONSE="$(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID/permanent" -H "Authorization: Bearer $TOKEN")"
case "$PURGE_RESPONSE" in
    *"file permanently deleted"*) ;;
    *) fail "permanent delete failed: $PURGE_RESPONSE" ;;
esac
PURGED_FILE_ID="$FILE_ID"
FILE_ID=""

FINAL_OUTPUT="$("$CHECK_SCRIPT" --dry-run)"
case "$FINAL_OUTPUT" in
    *"storage consistency ok"*) ;;
    *) fail "expected clean storage after purge: $FINAL_OUTPUT" ;;
esac

echo "storage consistency ok: orphan=$ORPHAN_NAME file_id=$PURGED_FILE_ID"
