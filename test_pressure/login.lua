local username = os.getenv("LOGIN_USER_NAME") or os.getenv("USER_NAME") or "upload_fix_manual"
local password = os.getenv("LOGIN_PASSWORD") or os.getenv("PASSWORD") or "123456"

wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"
wrk.headers["Connection"] = "keep-alive"
wrk.body = string.format("{\"username\":\"%s\",\"passwd\":\"%s\"}", username, password)
