#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FAILURE_TEST_PORT="${FAILURE_TEST_PORT:-19007}"
BASE_URL="${BASE_URL:-http://127.0.0.1:$FAILURE_TEST_PORT}"
USER_NAME="${USER_NAME:-failupload_$(date +%s)_$$}"
PASSWORD="${PASSWORD:-123456}"

# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

DB_HOST="${TWS_DB_HOST:-127.0.0.1}"
DB_PORT="${TWS_DB_PORT:-3306}"
DB_USER="${TWS_DB_USER:-root}"
DB_PASSWORD="${TWS_DB_PASSWORD:-}"
DB_NAME="${TWS_DB_NAME:-qgydb}"

SERVER_BIN="${SERVER_BIN:-$REPO_ROOT/build/server}"
FAILURE_TEST_START_SERVER="${FAILURE_TEST_START_SERVER:-1}"

MYSQL_PWD="$DB_PASSWORD"
export MYSQL_PWD

WORK_DIR="$(mktemp -d)"
SERVER_PID=""
SERVER_LOG="$WORK_DIR/failure-server.log"
UPLOAD_FILE="$WORK_DIR/failure-upload.txt"
TEST_ID="$(date +%s)-$$"
TOKEN=""

cleanup() {
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
        echo "failure test server log:"
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

wait_for_failure_server() {
    i=0
    while [ "$i" -lt 30 ]; do
        if curl -sS "$BASE_URL/healthz" >/dev/null 2>&1; then
            return 0
        fi
        if [ -n "$SERVER_PID" ] && ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
            fail "failure test server exited before becoming ready: $BASE_URL"
        fi
        sleep 1
        i=$((i + 1))
    done
    fail "failure test server is not ready: $BASE_URL"
}

start_failure_server() {
    if [ "$FAILURE_TEST_START_SERVER" != "1" ]; then
        return 0
    fi

    [ -x "$SERVER_BIN" ] || fail "server binary is not executable: $SERVER_BIN"

    (
        cd "$REPO_ROOT"
        TWS_PORT="$FAILURE_TEST_PORT" \
        TWS_TEST_FAIL_UPLOAD_BEFORE_COMMIT=1 \
        TWS_PID_FILE="$WORK_DIR/failure-server.pid" \
        "$SERVER_BIN"
    ) > "$SERVER_LOG" 2>&1 &
    SERVER_PID="$!"
}

if ! command -v mysql >/dev/null 2>&1; then
    fail "mysql client is required for upload failure cleanup assertions"
fi
if ! command -v sha256sum >/dev/null 2>&1; then
    fail "sha256sum is required for upload failure cleanup assertions"
fi
if ! mysql_base -e "SELECT 1" >/dev/null 2>&1; then
    fail "cannot connect to MySQL host=$DB_HOST port=$DB_PORT db=$DB_NAME user=$DB_USER"
fi

printf 'atlas upload failure cleanup %s\n' "$TEST_ID" > "$UPLOAD_FILE"
SHA256="$(sha256sum "$UPLOAD_FILE" | awk '{ print $1 }')"

start_failure_server
wait_for_failure_server
register_user

LOGIN_INFO="$(require_token)"
TOKEN="$(printf '%s\n' "$LOGIN_INFO" | sed -n '2p')"

if [ -x "$SCRIPT_DIR/check_storage_consistency.sh" ]; then
    "$SCRIPT_DIR/check_storage_consistency.sh" --dry-run --quiet >/dev/null
fi

STATUS_BODY="$WORK_DIR/upload-response.json"
STATUS="$(curl -sS -o "$STATUS_BODY" -w '%{http_code}' -X POST "$BASE_URL/api/drive/files/upload" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Expect:" \
    -F "file=@$UPLOAD_FILE;type=text/plain" \
    -F "filename=failure-cleanup-$TEST_ID.txt" \
    -F "is_public=false")"
BODY="$(cat "$STATUS_BODY")"

if [ "$STATUS" -ne 409 ]; then
    fail "expected failpoint upload to return 409, got status=$STATUS body=$BODY"
fi
case "$BODY" in
    *"test failpoint before upload commit"*) ;;
    *) fail "unexpected failpoint response body: $BODY" ;;
esac

FILE_ROWS="$(mysql_scalar "SELECT COUNT(*) FROM files WHERE content_sha256='$SHA256'")"
[ "$FILE_ROWS" = "0" ] || fail "files row should be rolled back for sha=$SHA256, count=$FILE_ROWS"

PHYSICAL_ROWS="$(mysql_scalar "SELECT COUNT(*) FROM physical_files WHERE sha256='$SHA256'")"
[ "$PHYSICAL_ROWS" = "0" ] || fail "physical_files row should be rolled back for sha=$SHA256, count=$PHYSICAL_ROWS"

if [ -x "$SCRIPT_DIR/check_storage_consistency.sh" ]; then
    "$SCRIPT_DIR/check_storage_consistency.sh" --dry-run --quiet >/dev/null
fi

echo "upload failure cleanup ok: sha256=$SHA256 status=$STATUS"
