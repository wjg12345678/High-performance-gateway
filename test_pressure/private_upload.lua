local token = os.getenv("TOKEN") or ""

if token == "" then
    error("TOKEN environment variable is required")
end

wrk.method = "POST"
wrk.headers["Authorization"] = "Bearer " .. token
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Connection"] = "keep-alive"
wrk.body = "{\"filename\":\"wrk-demo.txt\",\"content_base64\":\"aGVsbG8gdGlueSB3ZWIgc2VydmVy\",\"content_type\":\"text/plain\"}"
