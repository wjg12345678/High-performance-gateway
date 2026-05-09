#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

wait_for_server
register_user

LOGIN_INFO="$(require_token)"
LOGIN_RESPONSE="$(printf '%s\n' "$LOGIN_INFO" | sed -n '1p')"
TOKEN="$(printf '%s\n' "$LOGIN_INFO" | sed -n '2p')"

TMP_FILE="$(make_demo_file)"
trap 'rm -f "$TMP_FILE"' EXIT INT TERM

FILE_SIZE="$(wc -c < "$TMP_FILE" | tr -d ' ')"
PREFLIGHT_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/private/files/preflight" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"size\":$FILE_SIZE,\"folder_id\":0}")"
if ! printf '%s' "$PREFLIGHT_RESPONSE" | assert_json || ! printf '%s' "$PREFLIGHT_RESPONSE" | grep -q '"allowed":true'; then
    echo "preflight failed: $PREFLIGHT_RESPONSE"
    exit 1
fi

UPLOAD_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/private/files" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Expect:" \
    -F "file=@$TMP_FILE;type=text/plain" \
    -F "filename=demo.txt" \
    -F "is_public=false")"
FILE_ID="$(extract_file_id "$UPLOAD_RESPONSE")"
if [ -z "$FILE_ID" ]; then
    echo "upload failed: $UPLOAD_RESPONSE"
    exit 1
fi

echo "login: $LOGIN_RESPONSE"
echo "preflight: $PREFLIGHT_RESPONSE"
echo "upload: $UPLOAD_RESPONSE"
LIST_RESPONSE="$(curl -sS "$BASE_URL/api/private/files?limit=10" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$LIST_RESPONSE" | assert_json; then
    echo "invalid list json: $LIST_RESPONSE"
    exit 1
fi
echo "list page1: $LIST_RESPONSE"
echo "download headers/body:"
curl -i -sS "$BASE_URL/api/private/files/$FILE_ID/download" -H "Authorization: Bearer $TOKEN"
echo
echo "delete: $(curl -sS -X DELETE "$BASE_URL/api/private/files/$FILE_ID" -H "Authorization: Bearer $TOKEN")"
TRASH_RESPONSE="$(curl -sS "$BASE_URL/api/private/files?include_deleted=1&limit=10" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$TRASH_RESPONSE" | assert_json; then
    echo "invalid trash json: $TRASH_RESPONSE"
    exit 1
fi
echo "trash: $TRASH_RESPONSE"
echo "restore: $(curl -sS -X POST "$BASE_URL/api/private/files/$FILE_ID/restore" -H "Authorization: Bearer $TOKEN")"
RESTORED_RESPONSE="$(curl -sS "$BASE_URL/api/private/files?limit=10" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$RESTORED_RESPONSE" | assert_json; then
    echo "invalid restored list json: $RESTORED_RESPONSE"
    exit 1
fi
echo "active after restore: $RESTORED_RESPONSE"
echo "delete again: $(curl -sS -X DELETE "$BASE_URL/api/private/files/$FILE_ID" -H "Authorization: Bearer $TOKEN")"
echo "logout: $(curl -sS -X POST "$BASE_URL/api/private/logout" -H "Authorization: Bearer $TOKEN")"
