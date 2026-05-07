# 文件业务模块

本文档描述当前文件业务的接口边界、数据模型、权限控制、存储方式和测试入口。当前实现面向中小文件的真实上传、分页查询与回收站恢复场景。

## 接口

- `GET /healthz`
  - 健康检查
- `POST /api/login`
  - 登录成功后返回会话 `token`
- `POST /api/private/logout`
  - 默认使当前 Bearer Token 失效
  - `{"scope":"all"}` 可使当前用户全会话失效
- `POST /api/private/files`
  - 需要 `Authorization: Bearer <token>`
  - 主路径使用 `multipart/form-data`
  - 文件字段默认使用 `file`
  - 可通过普通表单字段传 `filename`、`is_public`
  - 请求体先流式写入临时文件，再提取文件 part，避免整块常驻内存
  - 上传大小上限由 `upload_max_bytes` / `TWS_UPLOAD_MAX_BYTES` 控制，默认 `100 MB`
- `GET /api/private/files`
  - 返回当前登录用户的分页文件列表
  - 支持 `limit`、`cursor`
  - `include_deleted=1` 或 `trash=1` 返回回收站视图
- `GET /api/private/files/:id/download`
  - 仅允许下载自己的文件
- `DELETE /api/private/files/:id`
  - 仅允许删除自己的文件
  - 删除动作为软删除，文件进入回收站
- `POST /api/private/files/:id/restore`
  - 从回收站恢复文件
  - 若文件名已被占用，服务端自动按 `(n)` 规则重命名
- `GET /api/private/operations`
  - 返回当前用户最近 50 条操作记录

## 数据表

- `user`
  - 用户名和密码
- `files`
  - 文件归属、磁盘文件名、原始文件名、类型、大小、`SHA-256`、公开状态、软删除时间
- `operation_logs`
  - 用户行为日志，记录登录、上传、下载、删除、鉴权失败等事件

## 权限模型

- 登录后由服务端生成会话 token，并持久化到 `user_sessions`
- `/api/private/*` 路由统一走 Bearer Token 鉴权
- 文件下载、删除、恢复按 `owner_username` 校验资源归属

## 安全改造

- 密码改为 `PBKDF2-HMAC-SHA256` 存储，服务端使用安全随机盐和固定迭代参数
- 为兼容旧数据，若用户仍是旧明文或旧 `SHA-256(salt + password)` 记录，登录成功后会自动升级为 PBKDF2
- 登录态不再只存在进程内存里，服务重启后只要 `user_sessions` 未过期，Bearer Token 仍可继续使用
- 会话采用滑动过期刷新，接近过期时自动续期
- 新登录会吊销同用户旧会话，`logout` 也支持当前会话或全会话两种主动失效路径

## 页面流程

- 登录成功后进入欢迎页
- 欢迎页优先提供 `文件管理台` 与 `公开分享` 入口
- 静态媒体页保留为辅助样例，不再作为默认主入口
- 文件管理页直接复用当前登录态，不再内嵌二次登录表单

## 存储说明

- multipart 请求体先临时写入 `root/uploads/.tmp/`
- 解析完成后，真实文件内容写入 `root/uploads/`
- 文件元数据写入 `files`
- 用户操作审计写入 `operation_logs`
- `docker-compose.yml` 已将 `./root/uploads` 挂载到容器内 `/app/root/uploads`
- 这样容器重建后，上传文件不会因为容器层重置而丢失

## 文件命名与哈希

- 服务端为每个文件计算 `SHA-256`，并写入 `files.content_sha256`
- 磁盘文件名使用随机 token + 哈希前缀 + 扩展名，避免直接暴露用户原始文件名
- 同一用户重复上传同名文件时，展示文件名按 `demo.txt`、`demo (1).txt`、`demo (2).txt` 递增

## 测试

- 总入口: [scripts/run_smoke_suite.sh](../scripts/run_smoke_suite.sh)
- 认证链路: [scripts/test_auth.sh](../scripts/test_auth.sh)
- 私有接口链路: [scripts/test_private_api.sh](../scripts/test_private_api.sh)
- 文件链路: [scripts/test_files.sh](../scripts/test_files.sh)
- 文件流程回归入口: [scripts/test_file_workflow.sh](../scripts/test_file_workflow.sh)

## 当前限制

- 当前仍然依赖 `Content-Length`，未支持 chunked 上传
- 当前主路径为单文件上传，多文件表单会返回错误
- 当前实现不是断点续传或对象存储方案
- 会话有效期仍为 7 天，但已补滑动刷新
