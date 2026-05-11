#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

status_body="$(mktemp)"
owner_file="$(make_demo_file)"
guest_file="$(make_demo_file)"
download_file="$(mktemp)"
trap 'rm -f "$status_body" "$owner_file" "$guest_file" "$download_file"' EXIT INT TERM

expect_status() {
    method="$1"
    url="$2"
    expected="$3"
    shift 3
    status="$(curl -sS -o "$status_body" -w '%{http_code}' -X "$method" "$url" "$@")"
    if [ "$status" -ne "$expected" ]; then
        echo "expected $expected for $method $url, got $status body=$(cat "$status_body")"
        exit 1
    fi
}

upload_file() {
    file_path="$1"
    filename="$2"
    token="$3"
    response="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
        -H "Authorization: Bearer $token" \
        -H "Expect:" \
        -F "file=@$file_path;type=text/plain" \
        -F "filename=$filename" \
        -F "is_public=false")"
    file_id="$(extract_file_id "$response")"
    if [ -z "$file_id" ]; then
        echo "upload failed: $response"
        exit 1
    fi
    printf '%s\n' "$file_id"
}

create_share() {
    file_id="$1"
    token="$2"
    body="$3"
    response="$(curl -sS -X POST "$BASE_URL/api/drive/files/$file_id/share" \
        -H "Authorization: Bearer $token" \
        -H "Content-Type: application/json" \
        -d "$body")"
    share_token="$(extract_share_token "$response")"
    if [ -z "$share_token" ] || ! printf '%s' "$response" | assert_json; then
        echo "share create failed: $response"
        exit 1
    fi
    printf '%s\n' "$share_token"
}

wait_for_server

OWNER_NAME="${USER_NAME}_owner_$$"
GUEST_NAME="${USER_NAME}_guest_$$"

USER_NAME="$OWNER_NAME" register_user
owner_login="$(USER_NAME="$OWNER_NAME" require_token)"
owner_token="$(printf '%s\n' "$owner_login" | sed -n '2p')"

USER_NAME="$GUEST_NAME" register_user
guest_login="$(USER_NAME="$GUEST_NAME" require_token)"
guest_token="$(printf '%s\n' "$guest_login" | sed -n '2p')"

owner_file_id="$(upload_file "$owner_file" "owner-share-edge.txt" "$owner_token")"
guest_file_id="$(upload_file "$guest_file" "guest-share-edge.txt" "$guest_token")"

expect_status POST "$BASE_URL/api/drive/files/$owner_file_id/share" 401 \
    -H "Content-Type: application/json" -d '{}'
expect_status POST "$BASE_URL/api/drive/files/$guest_file_id/share" 403 \
    -H "Authorization: Bearer $owner_token" -H "Content-Type: application/json" -d '{}'
guest_publish_response="$(curl -sS -X POST "$BASE_URL/api/drive/files/$guest_file_id/visibility" \
    -H "Authorization: Bearer $guest_token" \
    -H "Content-Type: application/json" \
    -d '{"is_public":true}')"
if ! printf '%s' "$guest_publish_response" | assert_json || ! printf '%s' "$guest_publish_response" | grep -q '"file is now public"'; then
    echo "guest publish failed: $guest_publish_response"
    exit 1
fi
public_guest_token="$(create_share "$guest_file_id" "$owner_token" '{}')"
public_guest_status="$(curl -sS -o "$download_file" -w '%{http_code}' "$BASE_URL/api/share/$public_guest_token/download")"
if [ "$public_guest_status" -ne 200 ] || ! cmp -s "$guest_file" "$download_file"; then
    echo "public guest share download failed: status=$public_guest_status body=$(cat "$download_file")"
    exit 1
fi
expect_status POST "$BASE_URL/api/drive/files/$owner_file_id/share" 400 \
    -H "Authorization: Bearer $owner_token" -H "Content-Type: application/json" -d '{"expires_in_seconds":"abc"}'
expect_status POST "$BASE_URL/api/drive/files/$owner_file_id/share" 400 \
    -H "Authorization: Bearer $owner_token" -H "Content-Type: application/json" -d '{"max_downloads":-1}'
long_code="123456789012345678901234567890123"
expect_status POST "$BASE_URL/api/drive/files/$owner_file_id/share" 400 \
    -H "Authorization: Bearer $owner_token" -H "Content-Type: application/json" -d "{\"access_code\":\"$long_code\"}"

plain_token="$(create_share "$owner_file_id" "$owner_token" '{}')"
expect_status GET "$BASE_URL/api/share/bad.token" 400
expect_status GET "$BASE_URL/api/share/00000000000000000000000000000000" 404

plain_detail="$(curl -sS "$BASE_URL/api/share/$plain_token")"
if ! printf '%s' "$plain_detail" | assert_json || ! printf '%s' "$plain_detail" | grep -q '"has_access_code":false'; then
    echo "plain share detail failed: $plain_detail"
    exit 1
fi
plain_status="$(curl -sS -o "$download_file" -w '%{http_code}' "$BASE_URL/api/share/$plain_token/download")"
if [ "$plain_status" -ne 200 ] || ! cmp -s "$owner_file" "$download_file"; then
    echo "plain share download failed: status=$plain_status body=$(cat "$download_file")"
    exit 1
fi

code_token="$(create_share "$owner_file_id" "$owner_token" '{"access_code":"2468"}')"
expect_status GET "$BASE_URL/api/share/$code_token?code=0000" 403
expect_status GET "$BASE_URL/api/share/$code_token/download?code=0000" 403

deleted_token="$(create_share "$owner_file_id" "$owner_token" '{}')"
delete_response="$(curl -sS -X DELETE "$BASE_URL/api/drive/files/$owner_file_id" -H "Authorization: Bearer $owner_token")"
if ! printf '%s' "$delete_response" | assert_json; then
    echo "delete failed: $delete_response"
    exit 1
fi
expect_status GET "$BASE_URL/api/share/$deleted_token" 404
expect_status GET "$BASE_URL/api/share/$deleted_token/download" 404

echo "share edge tests passed: owner=$OWNER_NAME guest=$GUEST_NAME public_guest=$public_guest_token plain=$plain_token code=$code_token deleted=$deleted_token"
