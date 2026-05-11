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
DOWNLOAD_FILE="$(mktemp)"
trap 'rm -f "$TMP_FILE" "$DOWNLOAD_FILE" "$STATUS_BODY"' EXIT INT TERM
STATUS_BODY="$(mktemp)"

UPLOAD_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Expect:" \
    -F "file=@$TMP_FILE;type=text/plain" \
    -F "filename=share-demo.txt" \
    -F "is_public=false")"
FILE_ID="$(extract_file_id "$UPLOAD_RESPONSE")"
if [ -z "$FILE_ID" ]; then
    echo "upload failed: $UPLOAD_RESPONSE"
    exit 1
fi

SHARE_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/$FILE_ID/share" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d '{"access_code":"2468","expires_in_seconds":3600,"max_downloads":1}')"
SHARE_TOKEN="$(extract_share_token "$SHARE_RESPONSE")"
if [ -z "$SHARE_TOKEN" ] || ! printf '%s' "$SHARE_RESPONSE" | assert_json; then
    echo "share create failed: $SHARE_RESPONSE"
    exit 1
fi

NO_CODE_STATUS="$(curl -sS -o "$STATUS_BODY" -w '%{http_code}' "$BASE_URL/api/share/$SHARE_TOKEN")"
if [ "$NO_CODE_STATUS" -ne 403 ]; then
    echo "share detail without code should be 403: status=$NO_CODE_STATUS body=$(cat "$STATUS_BODY")"
    exit 1
fi

DETAIL_RESPONSE="$(curl -sS "$BASE_URL/api/share/$SHARE_TOKEN?code=2468")"
if ! printf '%s' "$DETAIL_RESPONSE" | assert_json || ! printf '%s' "$DETAIL_RESPONSE" | grep -q '"has_access_code":true'; then
    echo "share detail failed: $DETAIL_RESPONSE"
    exit 1
fi

DOWNLOAD_STATUS="$(curl -sS -o "$DOWNLOAD_FILE" -w '%{http_code}' "$BASE_URL/api/share/$SHARE_TOKEN/download?code=2468")"
if [ "$DOWNLOAD_STATUS" -ne 200 ] || ! cmp -s "$TMP_FILE" "$DOWNLOAD_FILE"; then
    echo "share download failed: status=$DOWNLOAD_STATUS body=$(cat "$DOWNLOAD_FILE")"
    exit 1
fi

SECOND_STATUS="$(curl -sS -o "$STATUS_BODY" -w '%{http_code}' "$BASE_URL/api/share/$SHARE_TOKEN/download?code=2468")"
SECOND_BODY="$(cat "$STATUS_BODY")"
if [ "$SECOND_STATUS" -ne 429 ]; then
    echo "share max_downloads should be 429: status=$SECOND_STATUS body=$SECOND_BODY"
    exit 1
fi

EXPIRING_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/$FILE_ID/share" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d '{"expires_in_seconds":1}')"
EXPIRING_TOKEN="$(extract_share_token "$EXPIRING_RESPONSE")"
if [ -z "$EXPIRING_TOKEN" ] || ! printf '%s' "$EXPIRING_RESPONSE" | assert_json; then
    echo "expiring share create failed: $EXPIRING_RESPONSE"
    exit 1
fi
sleep 2
EXPIRED_STATUS="$(curl -sS -o "$STATUS_BODY" -w '%{http_code}' "$BASE_URL/api/share/$EXPIRING_TOKEN")"
if [ "$EXPIRED_STATUS" -ne 410 ]; then
    echo "expired share should be 410: status=$EXPIRED_STATUS body=$(cat "$STATUS_BODY")"
    exit 1
fi

echo "login: $LOGIN_RESPONSE"
echo "upload: $UPLOAD_RESPONSE"
echo "share: $SHARE_RESPONSE"
echo "detail: $DETAIL_RESPONSE"
echo "download: status=$DOWNLOAD_STATUS"
echo "download limit: status=$SECOND_STATUS body=$SECOND_BODY"
echo "expired: status=$EXPIRED_STATUS"
echo "logout: $(curl -sS -X POST "$BASE_URL/api/private/logout" -H "Authorization: Bearer $TOKEN")"
