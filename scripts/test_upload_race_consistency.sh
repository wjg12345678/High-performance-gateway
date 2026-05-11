#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="${USER_NAME:-uploadrace_$(date +%s)_$$}"
PASSWORD="${PASSWORD:-123456}"

# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

DB_HOST="${TWS_DB_HOST:-127.0.0.1}"
DB_PORT="${TWS_DB_PORT:-3306}"
DB_USER="${TWS_DB_USER:-root}"
DB_PASSWORD="${TWS_DB_PASSWORD:-}"
DB_NAME="${TWS_DB_NAME:-qgydb}"
UPLOAD_ROOT="${UPLOAD_ROOT:-$SCRIPT_DIR/../webroot/uploads}"
PARALLEL="${PARALLEL:-12}"

MYSQL_PWD="$DB_PASSWORD"
export MYSQL_PWD

TOKEN=""
TMP_FILE="$(mktemp)"
WORK_DIR="$(mktemp -d)"
FILE_IDS_FILE="$WORK_DIR/file_ids"
TEST_ID="$(date +%s)-$$"
PHYSICAL_ID=""
STORED_NAME=""
SHA256=""

cleanup() {
    rm -f "$TMP_FILE"
    if [ -n "$TOKEN" ] && [ -f "$FILE_IDS_FILE" ]; then
        while IFS= read -r file_id; do
            [ -n "$file_id" ] || continue
            curl -sS -X DELETE "$BASE_URL/api/drive/files/$file_id" -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1 || true
            curl -sS -X DELETE "$BASE_URL/api/drive/files/$file_id/permanent" -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1 || true
        done < "$FILE_IDS_FILE"
    fi
    rm -rf "$WORK_DIR"
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

upload_once() {
    index="$1"
    body="$WORK_DIR/upload_body_$index"
    status_file="$WORK_DIR/upload_status_$index"
    error_file="$WORK_DIR/upload_error_$index"

    if status="$(curl -sS -o "$body" -w '%{http_code}' -X POST "$BASE_URL/api/drive/files/upload" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Expect:" \
        -F "file=@$TMP_FILE;type=text/plain" \
        -F "filename=upload-race-$TEST_ID-$index.txt" \
        -F "is_public=false" 2>"$error_file")"; then
        printf '%s\n' "$status" > "$status_file"
    else
        printf '000\n' > "$status_file"
    fi
}

delete_once() {
    phase="$1"
    index="$2"
    file_id="$3"
    body="$WORK_DIR/${phase}_body_$index"
    status_file="$WORK_DIR/${phase}_status_$index"
    error_file="$WORK_DIR/${phase}_error_$index"
    url="$BASE_URL/api/drive/files/$file_id"
    if [ "$phase" = "purge" ]; then
        url="$url/permanent"
    fi

    if status="$(curl -sS -o "$body" -w '%{http_code}' -X DELETE "$url" \
        -H "Authorization: Bearer $TOKEN" 2>"$error_file")"; then
        printf '%s\n' "$status" > "$status_file"
    else
        printf '000\n' > "$status_file"
    fi
}

expect_status_and_body() {
    phase="$1"
    index="$2"
    expected_text="$3"
    status_file="$WORK_DIR/${phase}_status_$index"
    body_file="$WORK_DIR/${phase}_body_$index"
    error_file="$WORK_DIR/${phase}_error_$index"
    status="$(cat "$status_file" 2>/dev/null || printf 'missing')"
    body="$(cat "$body_file" 2>/dev/null || true)"

    if [ "$status" != "200" ]; then
        fail "$phase request $index failed: status=$status body=$body error=$(cat "$error_file" 2>/dev/null || true)"
    fi
    case "$body" in
        *"$expected_text"*) ;;
        *) fail "$phase request $index unexpected body: $body" ;;
    esac
}

if ! command -v mysql >/dev/null 2>&1; then
    fail "mysql client is required for upload race assertions"
fi

if ! mysql_base -e "SELECT 1" >/dev/null 2>&1; then
    fail "cannot connect to MySQL host=$DB_HOST port=$DB_PORT db=$DB_NAME user=$DB_USER"
fi

: > "$FILE_IDS_FILE"
printf 'atlas upload race consistency %s\n' "$TEST_ID" > "$TMP_FILE"

wait_for_server
register_user

LOGIN_INFO="$(require_token)"
TOKEN="$(printf '%s\n' "$LOGIN_INFO" | sed -n '2p')"

i=1
while [ "$i" -le "$PARALLEL" ]; do
    upload_once "$i" &
    i=$((i + 1))
done
wait

i=1
while [ "$i" -le "$PARALLEL" ]; do
    status="$(cat "$WORK_DIR/upload_status_$i" 2>/dev/null || printf 'missing')"
    body="$(cat "$WORK_DIR/upload_body_$i" 2>/dev/null || true)"
    if [ "$status" != "200" ]; then
        fail "upload $i failed: status=$status body=$body error=$(cat "$WORK_DIR/upload_error_$i" 2>/dev/null || true)"
    fi
    if ! printf '%s' "$body" | assert_json; then
        fail "upload $i returned invalid json: $body"
    fi
    file_id="$(extract_file_id "$body")"
    [ -n "$file_id" ] || fail "upload $i missing file id: $body"
    printf '%s\n' "$file_id" >> "$FILE_IDS_FILE"
    printf '%s\n' "$file_id" > "$WORK_DIR/file_id_$i"
    i=$((i + 1))
done

uploaded_count="$(wc -l < "$FILE_IDS_FILE" | tr -d ' ')"
[ "$uploaded_count" = "$PARALLEL" ] || fail "expected $PARALLEL uploaded files, got $uploaded_count"

FILE_IDS_CSV="$(paste -sd, "$FILE_IDS_FILE")"
ROW="$(mysql_scalar "SELECT COUNT(DISTINCT f.physical_id), MIN(f.physical_id), MIN(p.ref_count), MIN(p.stored_name), MIN(p.sha256), COUNT(*) FROM files f JOIN physical_files p ON p.id=f.physical_id WHERE f.id IN ($FILE_IDS_CSV)")"
set -- $ROW
DISTINCT_PHYSICAL="$1"
PHYSICAL_ID="$2"
REF_COUNT="$3"
STORED_NAME="$4"
SHA256="$5"
FILE_ROW_COUNT="$6"

[ "$DISTINCT_PHYSICAL" = "1" ] || fail "concurrent uploads should use one physical file, got $DISTINCT_PHYSICAL"
[ "$FILE_ROW_COUNT" = "$PARALLEL" ] || fail "expected $PARALLEL file rows, got $FILE_ROW_COUNT"
[ "$REF_COUNT" = "$PARALLEL" ] || fail "physical_id=$PHYSICAL_ID ref_count expected=$PARALLEL actual=$REF_COUNT"

PHYSICAL_BY_SHA="$(mysql_scalar "SELECT COUNT(*) FROM physical_files WHERE sha256='$SHA256'")"
[ "$PHYSICAL_BY_SHA" = "1" ] || fail "sha256=$SHA256 should have one physical row, got $PHYSICAL_BY_SHA"
ACTUAL_REFS="$(mysql_scalar "SELECT COUNT(*) FROM files WHERE physical_id=$PHYSICAL_ID")"
[ "$ACTUAL_REFS" = "$PARALLEL" ] || fail "physical_id=$PHYSICAL_ID actual refs expected=$PARALLEL actual=$ACTUAL_REFS"
assert_disk_exists "$STORED_NAME"

i=1
while [ "$i" -le "$PARALLEL" ]; do
    file_id="$(cat "$WORK_DIR/file_id_$i")"
    delete_once "delete" "$i" "$file_id" &
    i=$((i + 1))
done
wait

i=1
while [ "$i" -le "$PARALLEL" ]; do
    expect_status_and_body "delete" "$i" "file moved to recycle bin"
    i=$((i + 1))
done

REF_AFTER_SOFT_DELETE="$(mysql_scalar "SELECT ref_count FROM physical_files WHERE id=$PHYSICAL_ID")"
[ "$REF_AFTER_SOFT_DELETE" = "$PARALLEL" ] || fail "soft delete should not change ref_count, expected=$PARALLEL actual=$REF_AFTER_SOFT_DELETE"
DELETED_ROWS="$(mysql_scalar "SELECT COUNT(*) FROM files WHERE id IN ($FILE_IDS_CSV) AND deleted_at IS NOT NULL")"
[ "$DELETED_ROWS" = "$PARALLEL" ] || fail "expected $PARALLEL soft-deleted rows, got $DELETED_ROWS"
assert_disk_exists "$STORED_NAME"

i=1
while [ "$i" -le "$PARALLEL" ]; do
    file_id="$(cat "$WORK_DIR/file_id_$i")"
    delete_once "purge" "$i" "$file_id" &
    i=$((i + 1))
done
wait

i=1
while [ "$i" -le "$PARALLEL" ]; do
    expect_status_and_body "purge" "$i" "file permanently deleted"
    i=$((i + 1))
done

REMAINING_FILE_ROWS="$(mysql_scalar "SELECT COUNT(*) FROM files WHERE id IN ($FILE_IDS_CSV)")"
[ "$REMAINING_FILE_ROWS" = "0" ] || fail "file rows should be deleted after concurrent purge, remaining=$REMAINING_FILE_ROWS"
REMAINING_PHYSICAL_ROWS="$(mysql_scalar "SELECT COUNT(*) FROM physical_files WHERE id=$PHYSICAL_ID")"
[ "$REMAINING_PHYSICAL_ROWS" = "0" ] || fail "physical row should be deleted after final purge, remaining=$REMAINING_PHYSICAL_ROWS"
assert_disk_absent "$STORED_NAME"

: > "$FILE_IDS_FILE"

echo "upload race consistency ok: parallel=$PARALLEL physical_id=$PHYSICAL_ID sha256=$SHA256"
