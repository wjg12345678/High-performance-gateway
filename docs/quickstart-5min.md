# 5 分钟复现指南

这份指南用于秋招面试或代码评审现场快速复现 Atlas WebServer 主链路：启动服务、注册、登录、上传、下载、chunked 上传与完整冒烟测试。

## 前置要求

- Linux / macOS + Docker Compose
- 本机 `9006` 端口未被占用
- 已安装 `curl` 和 `python3`

## 一键启动

```bash
docker compose up -d --build
```

等待健康检查通过：

```bash
BASE_URL=http://127.0.0.1:9006
for i in $(seq 1 30); do
  curl -fsS "$BASE_URL/healthz" && break
  sleep 1
done
```

期望输出：

```json
{"code":0,"status":"ok"}
```

## 注册与登录

```bash
USER_NAME=resume_demo
PASSWORD=123456

curl -sS -X POST "$BASE_URL/api/register" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"$USER_NAME\",\"passwd\":\"$PASSWORD\"}"

LOGIN_JSON="$(curl -sS -X POST "$BASE_URL/api/login" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"$USER_NAME\",\"passwd\":\"$PASSWORD\"}")"
echo "$LOGIN_JSON"

TOKEN="$(printf '%s' "$LOGIN_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')"
```

## 上传、列表与下载

```bash
printf 'hello atlas webserver\n' > /tmp/atlas-demo.txt

UPLOAD_JSON="$(curl -sS -X POST "$BASE_URL/api/private/files" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Expect:' \
  -F 'file=@/tmp/atlas-demo.txt;type=text/plain' \
  -F 'filename=atlas-demo.txt' \
  -F 'is_public=false')"
echo "$UPLOAD_JSON"

FILE_ID="$(printf '%s' "$UPLOAD_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["file"]["id"])')"

curl -sS "$BASE_URL/api/private/files?limit=10" \
  -H "Authorization: Bearer $TOKEN" \
  | python3 -m json.tool

curl -i -sS "$BASE_URL/api/private/files/$FILE_ID/download" \
  -H "Authorization: Bearer $TOKEN"
```

## 真实 chunked 上传

`scripts/test_chunked_api.sh` 会用 raw socket 发送 `Transfer-Encoding: chunked` 请求，覆盖 `/api/echo` 和 multipart 文件上传。

```bash
BASE_URL="$BASE_URL" USER_NAME=resume_chunked_demo PASSWORD=123456 \
  scripts/test_chunked_api.sh
```

## 完整冒烟测试

```bash
BASE_URL="$BASE_URL" scripts/run_smoke_suite.sh
```

冒烟测试覆盖：认证、私有 API、文件上传/下载/回收站、chunked echo 和 chunked multipart 上传。

## 清理环境

```bash
docker compose down
rm -f /tmp/atlas-demo.txt
```

如果需要同时清理 MySQL 数据卷：

```bash
docker compose down -v
```
