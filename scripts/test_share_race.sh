#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

TMP_FILE="$(make_demo_file)"
WORK_DIR="$(mktemp -d)"
trap 'rm -f "$TMP_FILE"; rm -rf "$WORK_DIR"' EXIT INT TERM

PARALLEL="${PARALLEL:-12}"

wait_for_server
register_user

LOGIN_INFO="$(require_token)"
TOKEN="$(printf '%s\n' "$LOGIN_INFO" | sed -n '2p')"

UPLOAD_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Expect:" \
    -F "file=@$TMP_FILE;type=text/plain" \
    -F "filename=share-race.txt" \
    -F "is_public=false")"
FILE_ID="$(extract_file_id "$UPLOAD_RESPONSE")"
if [ -z "$FILE_ID" ]; then
    echo "upload failed: $UPLOAD_RESPONSE"
    exit 1
fi

SHARE_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/drive/files/$FILE_ID/share" \
    -H "Authorization: Bearer $TOKEN" \
    -H "Content-Type: application/json" \
    -d '{"max_downloads":1}')"
SHARE_TOKEN="$(extract_share_token "$SHARE_RESPONSE")"
if [ -z "$SHARE_TOKEN" ] || ! printf '%s' "$SHARE_RESPONSE" | assert_json; then
    echo "share create failed: $SHARE_RESPONSE"
    exit 1
fi

download_once() {
    index="$1"
    body="$WORK_DIR/body_$index"
    status="$(curl -sS -o "$body" -w '%{http_code}' "$BASE_URL/api/share/$SHARE_TOKEN/download")"
    printf '%s %s\n' "$index" "$status" >> "$WORK_DIR/statuses"
}

i=1
while [ "$i" -le "$PARALLEL" ]; do
    download_once "$i" &
    i=$((i + 1))
done
wait

count_status() {
    value="$1"
    awk -v target="$value" '$2 == target { count += 1 } END { print count + 0 }' "$WORK_DIR/statuses"
}

count_200="$(count_status 200)"
count_429="$(count_status 429)"
count_other="$(awk '$2 != 200 && $2 != 429 { count += 1 } END { print count + 0 }' "$WORK_DIR/statuses")"

if [ "$count_200" -ne 1 ] || [ "$count_429" -ne $((PARALLEL - 1)) ] || [ "$count_other" -ne 0 ]; then
    echo "unexpected race result:"
    cat "$WORK_DIR/statuses"
    exit 1
fi

winner_index="$(awk '$2 == 200 { print $1; exit }' "$WORK_DIR/statuses")"
if [ -z "$winner_index" ] || ! cmp -s "$TMP_FILE" "$WORK_DIR/body_$winner_index"; then
    echo "winner download body mismatch"
    exit 1
fi

DETAIL_BODY="$WORK_DIR/detail_after_race"
DETAIL_STATUS="$(curl -sS -o "$DETAIL_BODY" -w '%{http_code}' "$BASE_URL/api/share/$SHARE_TOKEN")"
if [ "$DETAIL_STATUS" -ne 429 ]; then
    echo "share detail after race should be 429, got status=$DETAIL_STATUS body=$(cat "$DETAIL_BODY")"
    exit 1
fi

echo "share race ok: token=$SHARE_TOKEN parallel=$PARALLEL winner=$winner_index"
