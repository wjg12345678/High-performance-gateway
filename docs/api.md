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
{"code":0,"message":"ok"}
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
      "sha256": "2aae6c35c94fcfb415dbe95f408b9ce91ee846ed",
      "owner": "demo",
      "created_at": "2026-04-16 14:00:00",
      "is_deleted": false,
      "content_available": true,
      "deleted_at": null
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

上传文件。当前主路径使用 `multipart/form-data`，请求体先流式落盘到临时文件，再提取文件 part 并写入最终存储目录。

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
