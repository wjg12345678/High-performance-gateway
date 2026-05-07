#!/bin/sh

BASE_URL="${BASE_URL:-http://127.0.0.1:9006}"
USER_NAME="${USER_NAME:-workflow_user}"
PASSWORD="${PASSWORD:-123456}"

wait_for_server() {
    i=0
    while [ "$i" -lt 20 ]; do
        healthz_response="$(curl -sS "$BASE_URL/healthz" 2>/dev/null || true)"
        if [ -n "$healthz_response" ]; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    echo "server is not ready: $BASE_URL"
    exit 1
}

register_user() {
    curl -sS -X POST "$BASE_URL/api/register" \
        -H "Content-Type: application/json" \
        -d "{\"username\":\"$USER_NAME\",\"passwd\":\"$PASSWORD\"}" >/dev/null || true
}

login_user() {
    curl -sS -X POST "$BASE_URL/api/login" \
        -H "Content-Type: application/json" \
        -d "{\"username\":\"$USER_NAME\",\"passwd\":\"$PASSWORD\"}"
}

extract_token() {
    echo "$1" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p'
}

extract_file_id() {
    echo "$1" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p'
}

require_token() {
    login_response="$(login_user)"
    token="$(extract_token "$login_response")"
    if [ -z "$token" ]; then
        echo "login failed: $login_response"
        exit 1
    fi
    printf '%s\n' "$login_response"
    printf '%s\n' "$token"
}

make_demo_file() {
    tmp_file="$(mktemp)"
    printf '%s\n' 'hello tiny web server' > "$tmp_file"
    printf '%s\n' "$tmp_file"
}

file_to_base64() {
    base64 < "$1" | tr -d '\n'
}
