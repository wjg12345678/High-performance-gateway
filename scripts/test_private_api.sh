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
SECOND_LOGIN_INFO="$(require_token)"
SECOND_LOGIN_RESPONSE="$(printf '%s\n' "$SECOND_LOGIN_INFO" | sed -n '1p')"
SECOND_TOKEN="$(printf '%s\n' "$SECOND_LOGIN_INFO" | sed -n '2p')"

FIRST_TOKEN_PING="$(curl -sS "$BASE_URL/api/private/ping" -H "Authorization: Bearer $TOKEN")"
SECOND_TOKEN_PING="$(curl -sS "$BASE_URL/api/private/ping" -H "Authorization: Bearer $SECOND_TOKEN")"
OPERATIONS_RESPONSE="$(curl -sS "$BASE_URL/api/private/operations" -H "Authorization: Bearer $SECOND_TOKEN")"
LOGOUT_ALL_RESPONSE="$(curl -sS -X POST "$BASE_URL/api/private/logout" \
    -H "Authorization: Bearer $SECOND_TOKEN" \
    -H "Content-Type: application/json" \
    -d '{"scope":"all"}')"
POST_LOGOUT_PING="$(curl -sS "$BASE_URL/api/private/ping" -H "Authorization: Bearer $SECOND_TOKEN")"

echo "$FIRST_TOKEN_PING" | grep '"code":401' >/dev/null
echo "$SECOND_TOKEN_PING" | grep '"code":0' >/dev/null
echo "$LOGOUT_ALL_RESPONSE" | grep '"scope":"all"' >/dev/null
echo "$POST_LOGOUT_PING" | grep '"code":401' >/dev/null

echo "login: $LOGIN_RESPONSE"
echo "second login: $SECOND_LOGIN_RESPONSE"
echo "old token ping: $FIRST_TOKEN_PING"
echo "active token ping: $SECOND_TOKEN_PING"
echo "operations: $OPERATIONS_RESPONSE"
echo "logout all: $LOGOUT_ALL_RESPONSE"
echo "post logout ping: $POST_LOGOUT_PING"
