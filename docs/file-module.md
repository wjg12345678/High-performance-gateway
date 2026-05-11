# 文件业务模块

本文档描述当前文件业务的接口边界、数据模型、权限控制、存储方式和测试入口。当前实现面向中小文件的真实上传、虚拟目录管理、公开文件与分享链接场景。

## 接口

- `GET /healthz`
  - 健康检查
- `POST /api/login`
  - 登录成功后返回会话 `token`
- `POST /api/private/logout`
  - 默认使当前 Bearer Token 失效
  - `{"scope":"all"}` 可使当前用户全会话失效
- `GET /api/drive/items?folder_id=0`
  - 返回当前虚拟目录下的文件夹和文件，是网盘主页核心数据源
- `POST /api/drive/folders`
  - 创建数据库虚拟目录，不在磁盘创建真实目录
- `DELETE /api/drive/folders/:id`
  - 删除当前用户的空目录；非空目录返回冲突，不做递归删除
- `POST /api/drive/files/preflight`
  - 上传前校验接口，JSON 入参示例：`{"size":12345,"folder_id":0}`
  - 返回 `allowed`、`used_bytes`、`remaining_bytes`、`max_single_file_bytes`、`max_total_bytes` 和拒绝原因
- `POST /api/drive/files/upload`
  - 需要 `Authorization: Bearer <token>`
  - 主路径使用 `multipart/form-data`
  - 文件字段默认使用 `file`
  - 可通过普通表单字段传 `filename`、`is_public`、`folder_id`
  - 请求体先流式写入临时文件，再提取文件 part，避免整块常驻内存
  - 上传大小上限由 `upload_max_bytes` / `TWS_UPLOAD_MAX_BYTES` 控制，默认 `100 MB`
  - 上传落库前会校验账号总容量，默认由 `user_storage_quota_bytes` / `TWS_USER_STORAGE_QUOTA_BYTES` 控制为 `1 GiB`
- `GET /api/drive/files/:id/download`
  - 仅允许下载自己的文件
- `DELETE /api/drive/files/:id`
  - 仅允许删除自己的文件，删除后进入回收站并从目录列表消失
- `GET /api/drive/trash`
  - 返回当前用户回收站文件
- `DELETE /api/drive/trash`
  - 清空当前用户回收站，逐个彻底删除回收站文件
- `POST /api/drive/files/:id/restore`
  - 恢复回收站文件；原目录不存在时恢复到根目录，同名冲突自动重命名
- `DELETE /api/drive/files/:id/permanent`
  - 彻底删除回收站文件，最后引用被清理后会删除物理文件
- `POST /api/drive/files/:id/visibility`
  - 切换自己的文件公开状态
- `POST /api/drive/files/:id/share`
  - 为自己的文件或任意公开文件创建独立分享链接；私有文件仍只能由拥有者创建
  - 支持 `access_code`、`expires_in_seconds`、`max_downloads`
- `GET /api/share/:token`
  - 查看分享文件元信息；设置提取码时需传 `code` / `access_code`
- `GET /api/share/:token/download`
  - 下载分享文件；成功下载会递增次数，超过 `max_downloads` 返回 `429`
- `GET /api/private/operations`
  - 返回当前用户最近 50 条操作记录
- `DELETE /api/private/operations`
  - 删除当前用户自己的所有操作日志
- `DELETE /api/private/operations/:id`
  - 删除当前用户自己的一条操作日志

## 数据表

- `users`
  - 用户主表，使用 `id` 作为主键，保存用户名、密码哈希、创建/更新时间、最近登录时间和禁用时间
- `user_sessions`
  - 登录会话表，通过 `user_id` 外键关联 `users`
- `files`
  - 用户视角文件元数据，通过 `user_id`、`physical_id`、`folder_id` 外键关联用户、物理文件和目录；同用户同目录的活跃文件名唯一
- `physical_files`
  - 物理文件表，按 `SHA-256` 唯一保存真实落盘文件、大小和引用计数；引用计数由数据库触发器维护
- `folders`
  - 用户虚拟目录树，通过 `user_id` 和自引用 `parent_id` 建模；根目录在库内用 `NULL` 表示，API 仍使用 `0`
- `operation_logs`
  - 用户行为日志，记录登录、上传、下载、删除、鉴权失败等事件，保留 `user_id` 和用户名快照
- `file_shares`
  - 独立分享链接表，记录 token、分享人、文件、提取码摘要、过期时间、下载次数限制和已下载次数

## 权限模型

- 登录后由服务端生成会话 token，并持久化到 `user_sessions`
- `/api/private/*` 和 `/api/drive/*` 路由统一走 Bearer Token 鉴权
- 文件下载、删除、公开切换按 `files.user_id` 校验资源归属，接口层仍使用用户名
- 分享链接通过随机 token 访问；可叠加提取码、过期时间和下载次数限制，不暴露原始登录态

## 安全改造

- 密码改为 `PBKDF2-HMAC-SHA256` 存储，服务端使用安全随机盐和固定迭代参数
- 密码记录统一存放在 `users.password_hash` 字段，格式为 `pbkdf2_sha256$iterations$salt$hash`
- 登录态不再只存在进程内存里，服务重启后只要 `user_sessions` 未过期，Bearer Token 仍可继续使用
- 会话采用滑动过期刷新，接近过期时自动续期
- 新登录会吊销同用户旧会话，`logout` 也支持当前会话或全会话两种主动失效路径

## 前端边界

- 页面、样式、媒体资源和 Vite 构建位于独立项目 `../Atlas-Frontend`
- 本后端只暴露 `/healthz` 和 `/api/*`，文件管理、公开分享等页面通过 API 与后端交互
- 后端不直接静态托管 `webroot/`，上传文件只能通过受控下载 API 返回

## 存储说明

- `service/auth/` 负责注册、登录、密码哈希、会话持久化和会话刷新
- `service/files/upload_service.*` 负责上传 preflight、配额校验、去重落库、临时文件物化和失败清理
- `service/files/share_service.*` 负责分享 token、访问码摘要、过期时间、下载次数限制和分享下载校验
- `service/files/file_service.*` 负责目录列表、回收站、下载前记录校验、删除和可见性更新
- `infra/storage/` 统一封装上传目录、临时目录、文件落盘、跨文件系统移动和 `SHA-256` 计算
- multipart 请求体先临时写入 `webroot/uploads/.tmp/`
- 解析完成后，真实文件内容写入 `webroot/uploads/`
- 文件元数据写入 `files`
- 相同 `SHA-256` 文件会复用 `physical_files` 物理记录，实现秒传/去重；`files` 只新增用户视角元数据
- `physical_files.ref_count` 由数据库触发器维护，上传和彻底删除路径使用事务与行锁保护配额、去重和引用关系
- 用户总容量按 `files.file_size` 聚合统计；回收站文件仍占用磁盘，因此仍计入容量，彻底删除后释放
- DB 内的 `files` / `physical_files` 可以事务化提交；DB 与磁盘文件不是同一个事务资源，彻底删除会在 DB commit 后删除磁盘文件
- [scripts/check_storage_consistency.sh](../scripts/check_storage_consistency.sh) 用于巡检 `physical_files`、`files` 和 `webroot/uploads`，可 dry-run 报告缺失文件、孤儿文件、引用计数漂移，也可用 `--fix-orphans` 清理无逻辑引用的磁盘文件
- 目录元数据写入 `folders`，不映射为磁盘目录
- 用户操作审计写入 `operation_logs`
- `docker-compose.yml` 已将 `./webroot/uploads` 挂载到容器内 `/app/webroot/uploads`
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
- 网盘目录链路: [scripts/test_drive.sh](../scripts/test_drive.sh)
- 并发上传/删除一致性: [scripts/test_upload_race_consistency.sh](../scripts/test_upload_race_consistency.sh)
- 配额并发竞争专项: [scripts/test_upload_quota_race.sh](../scripts/test_upload_quota_race.sh)
- 上传失败清理专项: [scripts/test_upload_failure_cleanup.sh](../scripts/test_upload_failure_cleanup.sh)
- 存储一致性巡检: [scripts/test_storage_consistency.sh](../scripts/test_storage_consistency.sh)
- Chunked 上传链路: [scripts/test_chunked_api.sh](../scripts/test_chunked_api.sh)
- 文件流程回归入口: [scripts/test_file_workflow.sh](../scripts/test_file_workflow.sh)

## 当前限制

- 上传请求体支持 `Content-Length` 和 `Transfer-Encoding: chunked`，响应仍使用 `Content-Length`
- 当前主路径为单文件上传，多文件表单会返回错误
- 当前实现不是断点续传或对象存储方案
- 会话有效期仍为 7 天，但已补滑动刷新
