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

CHUNKED_RESULT="$(BASE_URL="$BASE_URL" TOKEN="$TOKEN" python3 - <<'PY'
import json
import os
import socket
import sys
from urllib.parse import urlparse

base_url = os.environ.get("BASE_URL", "http://127.0.0.1:9006")
token = os.environ["TOKEN"]
parsed = urlparse(base_url)
if parsed.scheme != "http":
    raise SystemExit("chunked API test only supports http BASE_URL")
host = parsed.hostname or "127.0.0.1"
port = parsed.port or 80
base_path = parsed.path.rstrip("/")
if base_path == "":
    base_path = ""


def request_chunked(path, headers, chunks):
    request_path = base_path + path
    with socket.create_connection((host, port), timeout=10) as sock:
        lines = [
            f"POST {request_path} HTTP/1.1",
            f"Host: {host}:{port}",
            "Transfer-Encoding: chunked",
            "Connection: close",
        ]
        for key, value in headers.items():
            lines.append(f"{key}: {value}")
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("ascii"))
        for chunk in chunks:
            if isinstance(chunk, str):
                chunk = chunk.encode("utf-8")
            sock.sendall((f"{len(chunk):X}\r\n").encode("ascii"))
            sock.sendall(chunk)
            sock.sendall(b"\r\n")
        sock.sendall(b"0\r\n\r\n")

        response = bytearray()
        while True:
            data = sock.recv(65536)
            if not data:
                break
            response.extend(data)

    head, _, body = bytes(response).partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n", 1)[0].decode("iso-8859-1")
    parts = status_line.split()
    if len(parts) < 2:
        raise AssertionError(f"invalid response: {status_line!r}")
    return int(parts[1]), body.decode("utf-8", "replace")


echo_status, echo_body = request_chunked(
    "/api/echo",
    {"Content-Type": "text/plain"},
    ["hello ", "chunked ", "echo"],
)
if echo_status != 200:
    raise AssertionError(f"echo status={echo_status} body={echo_body}")
echo_json = json.loads(echo_body)
if echo_json.get("body") != "hello chunked echo":
    raise AssertionError(f"echo body mismatch: {echo_json}")

boundary = "atlas_chunked_boundary"
file_body = b"hello chunked file\n"
multipart = b"".join([
    b"--" + boundary.encode() + b"\r\n",
    b'Content-Disposition: form-data; name="filename"\r\n\r\n',
    b"chunked-api.txt\r\n",
    b"--" + boundary.encode() + b"\r\n",
    b'Content-Disposition: form-data; name="is_public"\r\n\r\n',
    b"false\r\n",
    b"--" + boundary.encode() + b"\r\n",
    b'Content-Disposition: form-data; name="file"; filename="chunked-api.txt"\r\n',
    b"Content-Type: text/plain\r\n\r\n",
    file_body,
    b"\r\n--" + boundary.encode() + b"--\r\n",
])
mid = len(multipart) // 2
upload_status, upload_body = request_chunked(
    "/api/drive/files/upload",
    {
        "Authorization": f"Bearer {token}",
        "Content-Type": f"multipart/form-data; boundary={boundary}",
    },
    [multipart[:mid], multipart[mid:]],
)
if upload_status != 200:
    raise AssertionError(f"upload status={upload_status} body={upload_body}")
upload_json = json.loads(upload_body)
if upload_json.get("code") != 0:
    raise AssertionError(f"upload failed: {upload_json}")
file_info = upload_json.get("file", {})
if file_info.get("filename") != "chunked-api.txt" or file_info.get("size") != len(file_body):
    raise AssertionError(f"upload metadata mismatch: {upload_json}")

print(json.dumps({
    "echo": echo_json,
    "upload": upload_json,
}, separators=(",", ":")))
PY
)"

FILE_ID="$(printf '%s\n' "$CHUNKED_RESULT" | sed -n 's/.*"id":\([0-9][0-9]*\).*/\1/p')"
if [ -n "$FILE_ID" ]; then
    curl -sS -X DELETE "$BASE_URL/api/drive/files/$FILE_ID" -H "Authorization: Bearer $TOKEN" >/dev/null || true
fi

echo "login: $LOGIN_RESPONSE"
echo "chunked: $CHUNKED_RESULT"
