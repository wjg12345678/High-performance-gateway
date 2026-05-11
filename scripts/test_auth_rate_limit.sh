#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/test_lib.sh"

wait_for_server

TEST_ID="${TEST_ID:-$(date +%s)_$$}"
LIMIT="${TWS_AUTH_LOGIN_USER_MAX_TOKENS:-10}"
ATTEMPTS="${AUTH_RATE_LIMIT_TEST_ATTEMPTS:-$((LIMIT + 2))}"
USER_NAME="${USER_NAME:-ratelimit_${TEST_ID}}"
PASSWORD="${PASSWORD:-wrong-password}"
CLIENT_IP="${CLIENT_IP:-198.51.100.$((($$ % 200) + 1))}"

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

i=1
rate_limited_body=""
rate_limited_status=""
while [ "$i" -le "$ATTEMPTS" ]; do
    body_file="$WORK_DIR/body_$i.json"
    status="$(curl -sS -o "$body_file" -w '%{http_code}' -X POST "$BASE_URL/api/login" \
        -H "Content-Type: application/json" \
        -H "X-Real-IP: $CLIENT_IP" \
        -d "{\"username\":\"$USER_NAME\",\"passwd\":\"$PASSWORD\"}")"
    body="$(cat "$body_file")"

    if [ "$status" = "429" ]; then
        rate_limited_status="$status"
        rate_limited_body="$body"
        break
    fi

    case "$status" in
        401|404) ;;
        *)
            echo "unexpected login response before rate limit: attempt=$i status=$status body=$body"
            exit 1
            ;;
    esac
    i=$((i + 1))
done

if [ "$rate_limited_status" != "429" ]; then
    echo "expected HTTP 429 after $ATTEMPTS attempts; auth rate limiting may be disabled"
    exit 1
fi

printf '%s' "$rate_limited_body" | assert_json
case "$rate_limited_body" in
    *"\"code\":429"*) ;;
    *)
        echo "unexpected 429 body: $rate_limited_body"
        exit 1
        ;;
esac

echo "auth rate limit ok: user=$USER_NAME ip=$CLIENT_IP attempts=$i body=$rate_limited_body"
