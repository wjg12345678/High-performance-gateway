#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

QUOTA_TEST_PORT="${QUOTA_TEST_PORT:-19006}"
BASE_URL="${BASE_URL:-http://127.0.0.1:$QUOTA_TEST_PORT}"
USER_NAME="${USER_NAME:-quotarace_$(date +%s)_$$}"
PASSWORD="${PASSWORD:-123456}"

# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

DB_HOST="${TWS_DB_HOST:-127.0.0.1}"
DB_PORT="${TWS_DB_PORT:-3306}"
DB_USER="${TWS_DB_USER:-root}"
DB_PASSWORD="${TWS_DB_PASSWORD:-}"
DB_NAME="${TWS_DB_NAME:-qgydb}"
UPLOAD_ROOT="${UPLOAD_ROOT:-$SCRIPT_DIR/../webroot/uploads}"

SERVER_BIN="${SERVER_BIN:-$REPO_ROOT/build/server}"
QUOTA_TEST_START_SERVER="${QUOTA_TEST_START_SERVER:-1}"
QUOTA_BYTES="${QUOTA_BYTES:-100}"
UPLOAD_MAX_BYTES="${UPLOAD_MAX_BYTES:-1048576}"
PARALLEL="${PARALLEL:-10}"
FILE_SIZE_BYTES="${FILE_SIZE_BYTES:-30}"

MYSQL_PWD="$DB_PASSWORD"
export MYSQL_PWD

WORK_DIR="$(mktemp -d)"
SERVER_PID=""
SERVER_LOG="$WORK_DIR/quota-server.log"
FILE_IDS_FILE="$WORK_DIR/file_ids"
TEST_ID="$(date +%s)-$$"
TOKEN=""

cleanup() {
    if [ -f "$FILE_IDS_FILE" ]; then
        while IFS= read -r file_id; do
            [ -n "$file_id" ] || continue
            curl -sS -X DELETE "$BASE_URL/api/drive/files/$file_id" -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1 || true
            curl -sS -X DELETE "$BASE_URL/api/drive/files/$file_id/permanent" -H "Authorization: Bearer $TOKEN" >/dev/null 2>&1 || true
        done < "$FILE_IDS_FILE"
    fi
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" >/dev/null 2>&1; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT INT TERM

fail() {
    echo "$1"
    if [ -s "$SERVER_LOG" ]; then
        echo "quota test server log:"
        tail -80 "$SERVER_LOG"
    fi
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

wait_for_quota_server() {
    i=0
    while [ "$i" -lt 30 ]; do
        if curl -sS "$BASE_URL/healthz" >/dev/null 2>&1; then
            return 0
        fi
        if [ -n "$SERVER_PID" ] && ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
            fail "quota test server exited before becoming ready: $BASE_URL"
        fi
        sleep 1
        i=$((i + 1))
    done
    fail "quota test server is not ready: $BASE_URL"
}

start_quota_server() {
    if [ "$QUOTA_TEST_START_SERVER" != "1" ]; then
        return 0
    fi

    [ -x "$SERVER_BIN" ] || fail "server binary is not executable: $SERVER_BIN"

    (
        cd "$REPO_ROOT"
        TWS_PORT="$QUOTA_TEST_PORT" \
        TWS_USER_STORAGE_QUOTA_BYTES="$QUOTA_BYTES" \
        TWS_UPLOAD_MAX_BYTES="$UPLOAD_MAX_BYTES" \
        TWS_PID_FILE="$WORK_DIR/quota-server.pid" \
        "$SERVER_BIN"
    ) > "$SERVER_LOG" 2>&1 &
    SERVER_PID="$!"
}

make_upload_files() {
    python3 - "$WORK_DIR" "$PARALLEL" "$FILE_SIZE_BYTES" "$TEST_ID" <<'PY'
import pathlib
import sys

work_dir = pathlib.Path(sys.argv[1])
parallel = int(sys.argv[2])
size = int(sys.argv[3])
test_id = sys.argv[4]

for index in range(1, parallel + 1):
    prefix = f"quota-race {test_id} {index:03d} "
    if len(prefix.encode()) > size:
        raise SystemExit("FILE_SIZE_BYTES is too small for generated prefix")
    data = (prefix + ("x" * size)).encode()[:size]
    (work_dir / f"upload_{index}.bin").write_bytes(data)
PY
}

upload_once() {
    index="$1"
    input_file="$WORK_DIR/upload_$index.bin"
    body="$WORK_DIR/upload_body_$index"
    status_file="$WORK_DIR/upload_status_$index"
    error_file="$WORK_DIR/upload_error_$index"

    if status="$(curl -sS -o "$body" -w '%{http_code}' -X POST "$BASE_URL/api/drive/files/upload" \
        -H "Authorization: Bearer $TOKEN" \
        -H "Expect:" \
        -F "file=@$input_file;type=application/octet-stream" \
        -F "filename=quota-race-$TEST_ID-$index.bin" \
        -F "is_public=false" 2>"$error_file")"; then
        printf '%s\n' "$status" > "$status_file"
    else
        printf '000\n' > "$status_file"
    fi
}

delete_uploaded_files() {
    if [ ! -f "$FILE_IDS_FILE" ]; then
        return 0
    fi
    while IFS= read -r file_id; do
        [ -n "$file_id" ] || continue
        curl -sS -X DELETE "$BASE_URL/api/drive/files/$file_id" -H "Authorization: Bearer $TOKEN" >/dev/null
        curl -sS -X DELETE "$BASE_URL/api/drive/files/$file_id/permanent" -H "Authorization: Bearer $TOKEN" >/dev/null
    done < "$FILE_IDS_FILE"
    : > "$FILE_IDS_FILE"
}

if ! command -v mysql >/dev/null 2>&1; then
    fail "mysql client is required for upload quota race assertions"
fi
if ! command -v python3 >/dev/null 2>&1; then
    fail "python3 is required to create deterministic quota test files"
fi
if ! mysql_base -e "SELECT 1" >/dev/null 2>&1; then
    fail "cannot connect to MySQL host=$DB_HOST port=$DB_PORT db=$DB_NAME user=$DB_USER"
fi
if [ "$FILE_SIZE_BYTES" -le 0 ] || [ "$QUOTA_BYTES" -le 0 ] || [ "$PARALLEL" -le 0 ]; then
    fail "QUOTA_BYTES, FILE_SIZE_BYTES and PARALLEL must be positive"
fi

EXPECTED_MAX_SUCCESS="$((QUOTA_BYTES / FILE_SIZE_BYTES))"
if [ "$EXPECTED_MAX_SUCCESS" -le 0 ]; then
    fail "quota must allow at least one test upload: quota=$QUOTA_BYTES file_size=$FILE_SIZE_BYTES"
fi
if [ "$PARALLEL" -le "$EXPECTED_MAX_SUCCESS" ]; then
    fail "PARALLEL must exceed quota capacity to exercise rejection: parallel=$PARALLEL max_success=$EXPECTED_MAX_SUCCESS"
fi

: > "$FILE_IDS_FILE"
make_upload_files
start_quota_server
wait_for_quota_server
register_user

LOGIN_INFO="$(require_token)"
TOKEN="$(printf '%s\n' "$LOGIN_INFO" | sed -n '2p')"

i=1
upload_pids=""
while [ "$i" -le "$PARALLEL" ]; do
    upload_once "$i" &
    upload_pids="$upload_pids $!"
    i=$((i + 1))
done
for pid in $upload_pids; do
    wait "$pid"
done

success_count=0
quota_reject_count=0
unexpected_count=0

i=1
while [ "$i" -le "$PARALLEL" ]; do
    status="$(cat "$WORK_DIR/upload_status_$i" 2>/dev/null || printf 'missing')"
    body="$(cat "$WORK_DIR/upload_body_$i" 2>/dev/null || true)"
    case "$status" in
        200)
            if ! printf '%s' "$body" | assert_json; then
                fail "upload $i returned invalid json: $body"
            fi
            file_id="$(extract_file_id "$body")"
            [ -n "$file_id" ] || fail "upload $i missing file id: $body"
            printf '%s\n' "$file_id" >> "$FILE_IDS_FILE"
            success_count=$((success_count + 1))
            ;;
        409)
            case "$body" in
                *"user storage quota exceeded"*) quota_reject_count=$((quota_reject_count + 1)) ;;
                *)
                    unexpected_count=$((unexpected_count + 1))
                    echo "unexpected 409 body for upload $i: $body" > "$WORK_DIR/unexpected_$i"
                    ;;
            esac
            ;;
        *)
            unexpected_count=$((unexpected_count + 1))
            echo "unexpected status for upload $i: status=$status body=$body error=$(cat "$WORK_DIR/upload_error_$i" 2>/dev/null || true)" > "$WORK_DIR/unexpected_$i"
            ;;
    esac
    i=$((i + 1))
done

if [ "$unexpected_count" -ne 0 ]; then
    cat "$WORK_DIR"/unexpected_* 2>/dev/null || true
    fail "unexpected upload responses: count=$unexpected_count"
fi
if [ "$success_count" -ne "$EXPECTED_MAX_SUCCESS" ]; then
    fail "expected exactly $EXPECTED_MAX_SUCCESS successful uploads, got $success_count"
fi
expected_rejects="$((PARALLEL - EXPECTED_MAX_SUCCESS))"
if [ "$quota_reject_count" -ne "$expected_rejects" ]; then
    fail "expected $expected_rejects quota rejections, got $quota_reject_count"
fi

USER_ID="$(mysql_scalar "SELECT id FROM users WHERE username='$USER_NAME' AND disabled_at IS NULL LIMIT 1")"
[ -n "$USER_ID" ] || fail "missing test user row: $USER_NAME"
USED_BYTES="$(mysql_scalar "SELECT COALESCE(SUM(file_size), 0) FROM files WHERE user_id=$USER_ID")"
EXPECTED_USED="$((EXPECTED_MAX_SUCCESS * FILE_SIZE_BYTES))"
if [ "$USED_BYTES" -ne "$EXPECTED_USED" ]; then
    fail "expected used bytes $EXPECTED_USED, got $USED_BYTES"
fi
if [ "$USED_BYTES" -gt "$QUOTA_BYTES" ]; then
    fail "quota exceeded after race: used=$USED_BYTES quota=$QUOTA_BYTES"
fi

delete_uploaded_files

USED_AFTER_CLEANUP="$(mysql_scalar "SELECT COALESCE(SUM(file_size), 0) FROM files WHERE user_id=$USER_ID")"
[ "$USED_AFTER_CLEANUP" = "0" ] || fail "expected used bytes 0 after cleanup, got $USED_AFTER_CLEANUP"

if [ -x "$SCRIPT_DIR/check_storage_consistency.sh" ]; then
    "$SCRIPT_DIR/check_storage_consistency.sh" --dry-run --quiet >/dev/null
fi

echo "upload quota race ok: parallel=$PARALLEL file_size=$FILE_SIZE_BYTES quota=$QUOTA_BYTES success=$success_count rejected=$quota_reject_count used=$USED_BYTES"
