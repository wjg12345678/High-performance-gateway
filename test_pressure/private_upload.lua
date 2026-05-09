local token = os.getenv("TOKEN") or ""

if token == "" then
    error("TOKEN environment variable is required")
end

local boundary = "atlas_wrk_boundary"
local body = table.concat({
    "--" .. boundary .. "\r\n",
    "Content-Disposition: form-data; name=\"filename\"\r\n\r\n",
    "wrk-demo.txt\r\n",
    "--" .. boundary .. "\r\n",
    "Content-Disposition: form-data; name=\"is_public\"\r\n\r\n",
    "false\r\n",
    "--" .. boundary .. "\r\n",
    "Content-Disposition: form-data; name=\"file\"; filename=\"wrk-demo.txt\"\r\n",
    "Content-Type: text/plain\r\n\r\n",
    "hello tiny web server\n",
    "\r\n--" .. boundary .. "--\r\n",
}, "")

wrk.method = "POST"
wrk.headers["Authorization"] = "Bearer " .. token
wrk.headers["Content-Type"] = "multipart/form-data; boundary=" .. boundary
wrk.headers["Connection"] = "keep-alive"
wrk.body = body
