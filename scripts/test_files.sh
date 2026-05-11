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
PREFLIGHT_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/preflight" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"size\":$FILE_SIZE,\"folder_id\":0}")"
if ! printf '%s' "$PREFLIGHT_RESPONSE" | assert_json || ! printf '%s' "$PREFLIGHT_RESPONSE" | grep -q '"allowed":true'; then
    echo "preflight failed: $PREFLIGHT_RESPONSE"
    exit 1
fi

UPLOAD_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
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
LIST_RESPONSE="$(curl -sS "$BASE_URL/api/drive/items?folder_id=0" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$LIST_RESPONSE" | assert_json; then
    echo "invalid list json: $LIST_RESPONSE"
    exit 1
fi
echo "list page1: $LIST_RESPONSE"
echo "download headers/body:"
curl -i -sS "$BASE_URL/api/drive/files/$FILE_ID/download" -H "Authorization: Bearer $TOKEN"
echo
echo "delete: $(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID" -H "Authorization: Bearer $TOKEN")"
AFTER_DELETE_RESPONSE="$(curl -sS "$BASE_URL/api/drive/items?folder_id=0" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$AFTER_DELETE_RESPONSE" | assert_json; then
    echo "invalid post-delete list json: $AFTER_DELETE_RESPONSE"
    exit 1
fi
echo "active after delete: $AFTER_DELETE_RESPONSE"
TRASH_RESPONSE="$(curl -sS "$BASE_URL/api/drive/trash" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$TRASH_RESPONSE" | assert_json || ! printf '%s' "$TRASH_RESPONSE" | grep -q "\"id\":$FILE_ID"; then
    echo "trash should contain deleted file: $TRASH_RESPONSE"
    exit 1
fi
echo "trash after delete: $TRASH_RESPONSE"
RESTORE_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/$FILE_ID/restore" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$RESTORE_RESPONSE" | assert_json || ! printf '%s' "$RESTORE_RESPONSE" | grep -q '"file restored"'; then
    echo "restore failed: $RESTORE_RESPONSE"
    exit 1
fi
echo "restore: $RESTORE_RESPONSE"
RESTORED_LIST_RESPONSE="$(curl -sS "$BASE_URL/api/drive/items?folder_id=0" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$RESTORED_LIST_RESPONSE" | assert_json || ! printf '%s' "$RESTORED_LIST_RESPONSE" | grep -q "\"id\":$FILE_ID"; then
    echo "restored file should be active: $RESTORED_LIST_RESPONSE"
    exit 1
fi
echo "active after restore: $RESTORED_LIST_RESPONSE"
echo "delete again: $(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID" -H "Authorization: Bearer $TOKEN")"
PURGE_RESPONSE="$(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID/permanent" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$PURGE_RESPONSE" | assert_json || ! printf '%s' "$PURGE_RESPONSE" | grep -q '"file permanently deleted"'; then
    echo "permanent delete failed: $PURGE_RESPONSE"
    exit 1
fi
echo "permanent delete: $PURGE_RESPONSE"
TRASH_AFTER_PURGE="$(curl -sS "$BASE_URL/api/drive/trash" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$TRASH_AFTER_PURGE" | assert_json; then
    echo "invalid trash-after-purge json: $TRASH_AFTER_PURGE"
    exit 1
fi
if printf '%s' "$TRASH_AFTER_PURGE" | grep -q "\"id\":$FILE_ID"; then
    echo "purged file should not remain in trash: $TRASH_AFTER_PURGE"
    exit 1
fi
echo "trash after purge: $TRASH_AFTER_PURGE"

EMPTY_UPLOAD_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Expect:" \
    -F "file=@$TMP_FILE;type=text/plain" \
    -F "filename=empty-trash-demo.txt" \
    -F "is_public=false")"
EMPTY_FILE_ID="$(extract_file_id "$EMPTY_UPLOAD_RESPONSE")"
if [ -z "$EMPTY_FILE_ID" ]; then
    echo "empty-trash upload failed: $EMPTY_UPLOAD_RESPONSE"
    exit 1
fi
echo "empty-trash upload: $EMPTY_UPLOAD_RESPONSE"
echo "empty-trash delete: $(curl -sS -X DELETE "$BASE_URL/api/drive/files/$EMPTY_FILE_ID" -H "Authorization: Bearer $TOKEN")"
EMPTY_TRASH_RESPONSE="$(curl -sS -X DELETE "$BASE_URL/api/drive/trash" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$EMPTY_TRASH_RESPONSE" | assert_json || ! printf '%s' "$EMPTY_TRASH_RESPONSE" | grep -q '"trash emptied"'; then
    echo "empty trash failed: $EMPTY_TRASH_RESPONSE"
    exit 1
fi
echo "empty trash: $EMPTY_TRASH_RESPONSE"
TRASH_AFTER_EMPTY="$(curl -sS "$BASE_URL/api/drive/trash" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$TRASH_AFTER_EMPTY" | assert_json; then
    echo "invalid trash-after-empty json: $TRASH_AFTER_EMPTY"
    exit 1
fi
if printf '%s' "$TRASH_AFTER_EMPTY" | grep -q "\"id\":$EMPTY_FILE_ID"; then
    echo "emptied file should not remain in trash: $TRASH_AFTER_EMPTY"
    exit 1
fi
echo "trash after empty: $TRASH_AFTER_EMPTY"
echo "logout: $(curl -sS -X POST "$BASE_URL/api/private/logout" -H "Authorization: Bearer $TOKEN")"
