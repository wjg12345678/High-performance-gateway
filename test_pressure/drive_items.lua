local token = os.getenv("TOKEN") or ""

if token == "" then
    error("TOKEN environment variable is required")
end

wrk.method = "GET"
wrk.headers["Authorization"] = "Bearer " .. token
wrk.headers["Connection"] = "keep-alive"
