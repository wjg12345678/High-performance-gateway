# API Reference

本文档描述 Atlas WebServer 当前暴露的 HTTP 接口、认证方式、主要请求参数和典型响应结构。

## Conventions

- Base URL: `http://127.0.0.1:9006`
- Content-Type: JSON 接口默认使用 `application/json`
- 认证方式：`Authorization: Bearer <token>`
- 成功响应通常返回 `{"code":0,...}`
- 错误响应通常返回 `{"code":<http_status>,"message":"..."}`

## Health Check

### `GET /healthz`

用于探活与健康检查。

示例响应：

```json
{"code":0,"status":"ok"}
```

## Debug API

### `POST /api/echo`

调试回显接口，用于验证普通请求体和 `Transfer-Encoding: chunked` 请求体解析。

请求体可使用任意文本或 JSON，响应会返回原始 body 和请求 `Content-Type`。

成功响应示例：

```json
{"code":0,"content_type":"text/plain","body":"hello chunked echo"}
```

## Authentication

### `POST /api/register`

注册用户。

请求体：

```json
{
  "username": "demo",
  "password": "secret"
}
```

兼容字段：

- `password` / `passwd`
- `username` / `user`

成功响应：

```json
{"code":0,"message":"register success"}
```

### `POST /api/login`

用户登录并获取会话 Token。登录成功后会吊销同一用户的旧会话，只保留当前新签发的 Token。

请求体：

```json
{
  "username": "demo",
  "password": "secret"
}
```

成功响应：

```json
{
  "code": 0,
  "message": "login success",
  "target": "/welcome",
  "token": "token-string",
  "expires_in": 604800
}
```

### `POST /api/private/logout`

使 Bearer Token 失效。默认仅注销当前 Token；若请求体携带 `{"scope":"all"}` 或 `{"all_sessions":true}`，则注销当前用户全部会话。

请求头：

```text
Authorization: Bearer <token>
```

成功响应：

```json
{"code":0,"message":"logout success","scope":"current"}
```

## Private APIs

以下接口均需要 Bearer Token。

### `GET /api/private/ping`

私有轻量探活接口。

### `GET /api/private/operations`

返回当前用户最近 `50` 条操作记录。

成功响应示例：

```json
{
  "code": 0,
  "operations": [
    {
      "id": 1,
      "action": "login",
      "resource_type": "user",
      "resource_id": 0,
      "detail": "login success",
      "created_at": "2026-04-16 14:00:00"
    }
  ]
}
```

### `DELETE /api/private/operations/:id`

删除当前用户的一条操作日志。

成功响应：

```json
{"code":0,"message":"delete success"}
```

## File APIs

## Drive APIs

Drive API 是新的网盘目录接口层，复用同一套 Bearer Token 鉴权。目录是数据库里的虚拟目录，真实文件仍统一写入 `webroot/uploads`。

### `GET /api/drive/items?folder_id=0`

返回当前目录下的文件夹和文件。`folder_id=0` 表示根目录。

成功响应示例：

```json
{
  "code": 0,
  "folder_id": 0,
  "folders": [
    {"id": 3, "parent_id": 0, "name": "秋招材料", "created_at": "2026-05-09 10:00:00"}
  ],
  "files": [
    {
      "id": 12,
      "folder_id": 0,
      "filename": "resume.txt",
      "content_type": "text/plain",
      "size": 18,
      "is_public": false,
      "created_at": "2026-05-09 10:01:00",
      "download_url": "/api/drive/files/12/download"
    }
  ]
}
```

### `POST /api/drive/folders`

创建虚拟目录。

请求体：

```json
{"name":"秋招材料","parent_id":0}
```

成功响应：

```json
{"code":0,"message":"folder created","folder":{"id":3,"parent_id":0,"name":"秋招材料","created_at":"2026-05-09 10:00:00"}}
```

### `DELETE /api/drive/folders/:id`

删除当前用户拥有的空文件夹。根目录 `0` 不能删除；如果目录下还有文件或子文件夹，会返回 `409 Conflict`。

成功响应：

```json
{"code":0,"message":"folder deleted"}
```

### `POST /api/drive/files/upload`

上传文件到指定虚拟目录，使用 `multipart/form-data`。

表单字段：

- `file`：二进制文件内容
- `folder_id`：目标目录，根目录传 `0`
- `filename`：可选，覆盖展示文件名
- `is_public`：可选，`true/false`

成功响应：

```json
{"code":0,"message":"upload success","file":{"id":12,"filename":"resume.txt","folder_id":3,"physical_id":5,"size":18,"is_public":false,"deduplicated":true,"sha256":"..."}}
```

如果服务端已存在相同 `SHA-256` 的物理文件，上传只新增 `files` 元数据并复用 `physical_files` 记录，响应中的 `deduplicated` 为 `true`。

### `GET /api/drive/files/:id/download`

下载当前用户拥有的文件。该接口复用文件归属校验，只允许下载自己的文件。

### `DELETE /api/drive/files/:id`

软删除当前用户拥有的文件，文件会从目录列表中消失并进入回收站。

### Shell 测试

```bash
BASE_URL=http://127.0.0.1:9006 USER_NAME=drive_user PASSWORD=123456 ./scripts/test_drive.sh
```

### `GET /api/private/files`

返回当前用户的分页文件记录。

查询参数：

- `limit`：每页条数，默认 `20`，最大 `100`
- `cursor`：向下翻页游标，取上一页最后一条记录的 `id`
- `include_deleted=1` 或 `trash=1`：查看回收站

成功响应示例：

```json
{
  "code": 0,
  "files": [
    {
      "id": 12,
      "filename": "demo.txt",
      "content_type": "text/plain",
      "size": 18,
      "is_public": false,
      "created_at": "2026-04-16 14:00:00"
    }
  ],
  "pagination": {
    "limit": 20,
    "next_cursor": 0,
    "has_more": false
  },
  "view": "active"
}
```

### `POST /api/private/files`

上传文件。当前主路径使用 `multipart/form-data`，同时支持 `Content-Length` 和 `Transfer-Encoding: chunked` 请求体；请求体先流式落盘到临时文件，再提取文件 part 并写入最终存储目录。

表单字段：

- `file`：二进制文件内容
- `filename`：可选，覆盖展示文件名
- `is_public`：可选，`true/false`
- `content_type`：可选，未传时优先使用文件 part 自带 `Content-Type`

限制：

- 上传上限由 `upload_max_bytes` / `TWS_UPLOAD_MAX_BYTES` 配置，默认 `100 MB`
- 默认仅支持单文件上传
- 若同一用户重复上传同名文件，服务端会自动重命名为 `demo (1).txt`、`demo (2).txt`

成功响应：

```json
{
  "code": 0,
  "message": "upload success",
  "file": {
    "id": 12,
    "filename": "demo.txt",
    "size": 11,
    "is_public": false,
    "sha256": "64ec88ca00b268e5ba1a35678a1b5316d212f4f36631ec0b0f4cfd92f6f2cf07"
  }
}
```

### `GET /api/private/files/:id/download`

下载当前用户拥有的文件。

响应类型：

- 文件流
- 带 `Content-Disposition: attachment`

### `DELETE /api/private/files/:id`

软删除当前用户拥有的文件，文件会进入回收站。

成功响应：

```json
{"code":0,"message":"file moved to recycle bin"}
```

### `POST /api/private/files/:id/restore`

从回收站恢复当前用户拥有的文件。

成功响应：

```json
{
  "code": 0,
  "message": "file restored",
  "file": {
    "id": 12,
    "filename": "demo.txt"
  }
}
```

### `POST /api/private/files/:id/visibility`

切换文件公开可见性。

请求体：

```json
{
  "is_public": true
}
```

成功响应之一：

```json
{"code":0,"message":"file is now public"}
```

```json
{"code":0,"message":"file is now private"}
```

### `POST /api/private/files/:id/share`

为当前用户拥有的文件创建独立分享链接。分享链接不依赖文件 `is_public` 状态，可选配置过期时间、提取码和下载次数限制。

请求体：

```json
{
  "access_code": "2468",
  "expires_in_seconds": 3600,
  "max_downloads": 3
}
```

字段说明：

- `access_code`：可选提取码，最多 32 字符；服务端只保存 `SHA-256` 摘要
- `expires_in_seconds` / `expires_in`：可选，单位秒；`0` 或不传表示不过期
- `max_downloads` / `download_limit`：可选；`0` 或不传表示不限次数

成功响应：

```json
{
  "code": 0,
  "share": {
    "token": "0123456789abcdef0123456789abcdef",
    "file_id": 12,
    "filename": "demo.txt",
    "content_type": "text/plain",
    "size": 11,
    "sha256": "64ec88ca00b268e5ba1a35678a1b5316d212f4f36631ec0b0f4cfd92f6f2cf07",
    "has_access_code": true,
    "expires_at": "2026-05-09 12:00:00",
    "max_downloads": 3,
    "download_count": 0,
    "share_url": "/share?token=0123456789abcdef0123456789abcdef",
    "detail_url": "/api/share/0123456789abcdef0123456789abcdef",
    "download_url": "/api/share/0123456789abcdef0123456789abcdef/download"
  }
}
```

### `GET /api/share/:token`

查看分享文件元信息，无需登录。若分享设置了提取码，需要通过查询参数或请求体传 `code` / `access_code`。

错误语义：

- `403`：缺少提取码或提取码错误
- `410`：分享链接已过期
- `429`：分享下载次数已用尽

### `GET /api/share/:token/download`

下载分享文件，无需登录。若分享设置了提取码，需要传 `code` / `access_code`；下载成功后原子递增 `download_count`，达到 `max_downloads` 后返回 `429`。

## Public File APIs

### `GET /api/files/public`

返回公开文件分页列表，支持 `limit`、`cursor`。

### `GET /api/files/public/:id`

返回公开文件详情。

成功响应示例：

```json
{
  "code": 0,
  "file": {
    "id": 12,
    "filename": "demo.txt",
    "content_type": "text/plain",
    "size": 11,
    "owner": "demo",
    "is_public": true,
    "sha256": "64ec88ca00b268e5ba1a35678a1b5316d212f4f36631ec0b0f4cfd92f6f2cf07",
    "download_url": "/api/files/public/12/download"
  }
}
```

### `GET /api/files/public/:id/download`

下载公开文件，无需登录。

## Static Pages

- `GET /`
- `GET /login`
- `GET /register`
- `GET /welcome`
- `GET /files`
- `GET /share`
- `GET /media`

兼容说明：

- 历史教学路由 `/0`、`/1`、`/2CGISQL.cgi`、`/3CGISQL.cgi`、`/5`、`/6`、`/7` 默认关闭
- 仅在 `legacy_compat=1` 或 `TWS_LEGACY_COMPAT=1` 时恢复

## Error Semantics

常见错误码：

- `400 Bad Request`：请求字段缺失或格式非法
- `401 Unauthorized`：缺失或无效 Token，或登录失败
- `403 Forbidden`：尝试访问无权限资源
- `404 Not Found`：目标资源不存在
- `413 Payload Too Large`：上传内容超过当前大小限制
- `500 Internal Server Error`：数据库或文件系统操作失败
