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
trap 'rm -f "$TMP_FILE" "$TMP_FILE.download"' EXIT INT TERM

echo "login: $LOGIN_RESPONSE"

ROOT_LIST="$(curl -sS "$BASE_URL/api/drive/items?folder_id=0" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$ROOT_LIST" | assert_json; then
    echo "invalid root list json: $ROOT_LIST"
    exit 1
fi
echo "root list: $ROOT_LIST"

FOLDER_NAME="drive-$(date +%s)-$$"
FOLDER_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/folders" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"name\":\"$FOLDER_NAME\",\"parent_id\":0}")"
FOLDER_ID="$(printf '%s' "$FOLDER_RESPONSE" | sed -n 's/.*"folder":{"id":\([0-9][0-9]*\).*/\1/p')"
if [ -z "$FOLDER_ID" ]; then
    echo "folder create failed: $FOLDER_RESPONSE"
    exit 1
fi
echo "create folder: $FOLDER_RESPONSE"

EMPTY_FOLDER_NAME="empty-$(date +%s)-$$"
EMPTY_FOLDER_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/folders" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"name\":\"$EMPTY_FOLDER_NAME\",\"parent_id\":0}")"
EMPTY_FOLDER_ID="$(printf '%s' "$EMPTY_FOLDER_RESPONSE" | sed -n 's/.*"folder":{"id":\([0-9][0-9]*\).*/\1/p')"
if [ -z "$EMPTY_FOLDER_ID" ]; then
    echo "empty folder create failed: $EMPTY_FOLDER_RESPONSE"
    exit 1
fi
echo "delete empty folder: $(curl -sS -X DELETE "$BASE_URL/api/drive/folders/$EMPTY_FOLDER_ID" -H "Authorization: Bearer $TOKEN")"

UPLOAD_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Expect:" \
    -F "file=@$TMP_FILE;type=text/plain" \
    -F "filename=resume.txt" \
    -F "folder_id=$FOLDER_ID")"
FILE_ID="$(extract_file_id "$UPLOAD_RESPONSE")"
if [ -z "$FILE_ID" ]; then
    echo "upload failed: $UPLOAD_RESPONSE"
    exit 1
fi
echo "upload: $UPLOAD_RESPONSE"

DEDUP_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Expect:" \
    -F "file=@$TMP_FILE;type=text/plain" \
    -F "filename=resume-copy.txt" \
    -F "folder_id=$FOLDER_ID")"
DEDUP_FILE_ID="$(extract_file_id "$DEDUP_RESPONSE")"
if [ -z "$DEDUP_FILE_ID" ]; then
    echo "dedup upload failed: $DEDUP_RESPONSE"
    exit 1
fi
case "$DEDUP_RESPONSE" in
    *"\"deduplicated\":true"*) echo "dedup upload: $DEDUP_RESPONSE" ;;
    *) echo "dedup upload did not hit sha256 reuse: $DEDUP_RESPONSE"; exit 1 ;;
esac

FOLDER_LIST="$(curl -sS "$BASE_URL/api/drive/items?folder_id=$FOLDER_ID" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$FOLDER_LIST" | assert_json; then
    echo "invalid folder list json: $FOLDER_LIST"
    exit 1
fi
echo "folder list: $FOLDER_LIST"

NON_EMPTY_DELETE="$(curl -sS -X DELETE "$BASE_URL/api/drive/folders/$FOLDER_ID" -H "Authorization: Bearer $TOKEN")"
case "$NON_EMPTY_DELETE" in
    *"folder is not empty"*) echo "non-empty folder delete blocked: $NON_EMPTY_DELETE" ;;
    *) echo "non-empty folder delete should be blocked: $NON_EMPTY_DELETE"; exit 1 ;;
esac

echo "download headers:"
curl -f -sS -D - "$BASE_URL/api/drive/files/$FILE_ID/download" \
    -H "Authorization: Bearer $TOKEN" \
    -o "$TMP_FILE.download" | sed -n '1,8p'
if ! cmp -s "$TMP_FILE" "$TMP_FILE.download"; then
    echo "downloaded file differs from uploaded file"
    exit 1
fi

echo "delete: $(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID" -H "Authorization: Bearer $TOKEN")"
echo "delete dedup copy: $(curl -sS -X DELETE "$BASE_URL/api/drive/files/$DEDUP_FILE_ID" -H "Authorization: Bearer $TOKEN")"
AFTER_DELETE="$(curl -sS "$BASE_URL/api/drive/items?folder_id=$FOLDER_ID" -H "Authorization: Bearer $TOKEN")"
if ! printf '%s' "$AFTER_DELETE" | assert_json; then
    echo "invalid after-delete json: $AFTER_DELETE"
    exit 1
fi
echo "after delete: $AFTER_DELETE"
echo "purge: $(curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID/permanent" -H "Authorization: Bearer $TOKEN")"
echo "purge dedup copy: $(curl -sS -X DELETE "$BASE_URL/api/drive/files/$DEDUP_FILE_ID/permanent" -H "Authorization: Bearer $TOKEN")"
echo "delete now-empty folder: $(curl -sS -X DELETE "$BASE_URL/api/drive/folders/$FOLDER_ID" -H "Authorization: Bearer $TOKEN")"
