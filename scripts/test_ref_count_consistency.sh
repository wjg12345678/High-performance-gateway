#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="${USER_NAME:-refcount_$(date +%s)_$$}"
PASSWORD="${PASSWORD:-123456}"

# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

DB_HOST="${TWS_DB_HOST:-127.0.0.1}"
DB_PORT="${TWS_DB_PORT:-3306}"
DB_USER="${TWS_DB_USER:-root}"
DB_PASSWORD="${TWS_DB_PASSWORD:-}"
DB_NAME="${TWS_DB_NAME:-qgydb}"
UPLOAD_ROOT="${UPLOAD_ROOT:-$SCRIPT_DIR/../webroot/uploads}"

MYSQL_PWD="$DB_PASSWORD"
export MYSQL_PWD

TOKEN=""
FILE_ID_1=""
FILE_ID_2=""
TMP_FILE="$(mktemp)"
TEST_ID="$(date +%s)-$$"

cleanup() {
    rm -f "$TMP_FILE"
    if [ -n "$TOKEN" ]; then
        for file_id in $FILE_ID_1 $FILE_ID_2; do
            [ -n "$file_id" ] || continue
            curl -sS -X DELETE "$BASE_URL/api/drive/files/$file_id" -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1 || true
            curl -sS -X DELETE "$BASE_URL/api/drive/files/$file_id/permanent" -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1 || true
        done
    fi
}
trap cleanup EXIT INT TERM

fail() {
    echo "$1"
    exit 1
}

mysql_base() {
    mysql \
        --protocol=tcp \
        -h "$DB_HOST" \
        -P "$DB_PORT" \
        -u "$DB_USER" \
        --default-character-set=utf8mb4 \
        "$DB_NAME" \
        "$@"
}

mysql_scalar() {
    mysql_base -N -s -e "$1"
}

physical_row_for_file() {
    mysql_scalar "SELECT f.physical_id, p.ref_count, p.stored_name, p.sha256 FROM files f JOIN physical_files p ON p.id=f.physical_id WHERE f.id=$1"
}

ref_count_for_physical() {
    mysql_scalar "SELECT ref_count FROM physical_files WHERE id=$1"
}

physical_count() {
    mysql_scalar "SELECT COUNT(*) FROM physical_files WHERE id=$1"
}

assert_ref_count() {
    physical_id="$1"
    expected="$2"
    actual="$(ref_count_for_physical "$physical_id")"
    if [ "$actual" != "$expected" ]; then
        fail "physical_id=$physical_id ref_count expected=$expected actual=$actual"
    fi
}

assert_disk_exists() {
    stored_name="$1"
    if [ -d "$UPLOAD_ROOT" ] && [ ! -f "$UPLOAD_ROOT/$stored_name" ]; then
        fail "expected disk file to exist: $UPLOAD_ROOT/$stored_name"
    fi
}

assert_disk_absent() {
    stored_name="$1"
    if [ -d "$UPLOAD_ROOT" ] && [ -e "$UPLOAD_ROOT/$stored_name" ]; then
        fail "expected disk file to be removed: $UPLOAD_ROOT/$stored_name"
    fi
}

upload_copy() {
    filename="$1"
    response="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Expect:" \
        -F "file=@$TMP_FILE;type=text/plain" \
        -F "filename=$filename" \
        -F "is_public=false")"
    file_id="$(extract_file_id "$response")"
    if [ -z "$file_id" ]; then
        fail "upload failed: $response"
    fi
    printf '%s\n' "$response"
    printf '%s\n' "$file_id"
}

if ! command -v mysql >/dev/null 2>&1; then
    fail "mysql client is required for ref_count consistency assertions"
fi

if ! mysql_base -e "SELECT 1" >/dev/null 2>&1; then
    fail "cannot connect to MySQL host=$DB_HOST port=$DB_PORT db=$DB_NAME user=$DB_USER"
fi

printf 'atlas ref_count consistency %s\n' "$TEST_ID" > "$TMP_FILE"

wait_for_server
register_user

LOGIN_INFO="$(require_token)"
TOKEN="$(printf '%s\n' "$LOGIN_INFO" | sed -n '2p')"

UPLOAD_INFO_1="$(upload_copy "refcount-a-$TEST_ID.txt")"
UPLOAD_RESPONSE_1="$(printf '%s\n' "$UPLOAD_INFO_1" | sed -n '1p')"
FILE_ID_1="$(printf '%s\n' "$UPLOAD_INFO_1" | sed -n '2p')"
ROW_1="$(physical_row_for_file "$FILE_ID_1")"
[ -n "$ROW_1" ] || fail "missing physical row for first upload: file_id=$FILE_ID_1 response=$UPLOAD_RESPONSE_1"
set -- $ROW_1
PHYSICAL_ID="$1"
REF_COUNT="$2"
STORED_NAME="$3"
SHA256="$4"
[ "$REF_COUNT" = "1" ] || fail "first upload should set ref_count=1, got $REF_COUNT"
assert_disk_exists "$STORED_NAME"

UPLOAD_INFO_2="$(upload_copy "refcount-b-$TEST_ID.txt")"
UPLOAD_RESPONSE_2="$(printf '%s\n' "$UPLOAD_INFO_2" | sed -n '1p')"
FILE_ID_2="$(printf '%s\n' "$UPLOAD_INFO_2" | sed -n '2p')"
case "$UPLOAD_RESPONSE_2" in
    *"\"deduplicated\":true"*) ;;
    *) fail "second upload should reuse existing physical file: $UPLOAD_RESPONSE_2" ;;
esac

ROW_2="$(physical_row_for_file "$FILE_ID_2")"
[ -n "$ROW_2" ] || fail "missing physical row for second upload: file_id=$FILE_ID_2 response=$UPLOAD_RESPONSE_2"
set -- $ROW_2
PHYSICAL_ID_2="$1"
REF_COUNT_2="$2"
STORED_NAME_2="$3"
SHA256_2="$4"
[ "$PHYSICAL_ID_2" = "$PHYSICAL_ID" ] || fail "dedup uploads used different physical ids: $PHYSICAL_ID vs $PHYSICAL_ID_2"
[ "$STORED_NAME_2" = "$STORED_NAME" ] || fail "dedup uploads used different stored files: $STORED_NAME vs $STORED_NAME_2"
[ "$SHA256_2" = "$SHA256" ] || fail "dedup uploads used different sha256: $SHA256 vs $SHA256_2"
[ "$REF_COUNT_2" = "2" ] || fail "second upload should set ref_count=2, got $REF_COUNT_2"
assert_disk_exists "$STORED_NAME"

SOFT_DELETE_1="$(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID_1" -H "Authorization: Bearer $TOKEN")"
case "$SOFT_DELETE_1" in
    *"file moved to recycle bin"*) ;;
    *) fail "soft delete first file failed: $SOFT_DELETE_1" ;;
esac
assert_ref_count "$PHYSICAL_ID" 2
assert_disk_exists "$STORED_NAME"

PURGE_1="$(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID_1/permanent" -H "Authorization: Bearer $TOKEN")"
case "$PURGE_1" in
    *"file permanently deleted"*) ;;
    *) fail "permanent delete first file failed: $PURGE_1" ;;
esac
FILE_ID_1=""
assert_ref_count "$PHYSICAL_ID" 1
assert_disk_exists "$STORED_NAME"

SOFT_DELETE_2="$(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID_2" -H "Authorization: Bearer $TOKEN")"
case "$SOFT_DELETE_2" in
    *"file moved to recycle bin"*) ;;
    *) fail "soft delete second file failed: $SOFT_DELETE_2" ;;
esac
assert_ref_count "$PHYSICAL_ID" 1

PURGE_2="$(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID_2/permanent" -H "Authorization: Bearer $TOKEN")"
case "$PURGE_2" in
    *"file permanently deleted"*) ;;
    *) fail "permanent delete second file failed: $PURGE_2" ;;
esac
FILE_ID_2=""

COUNT_AFTER_PURGE="$(physical_count "$PHYSICAL_ID")"
[ "$COUNT_AFTER_PURGE" = "0" ] || fail "physical row should be deleted after final purge, count=$COUNT_AFTER_PURGE"
assert_disk_absent "$STORED_NAME"

echo "ref_count consistency ok: physical_id=$PHYSICAL_ID sha256=$SHA256"
