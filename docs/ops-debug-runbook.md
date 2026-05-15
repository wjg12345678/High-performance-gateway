# Atlas WebServer 运维排障与事故处理手册

这份文档按真实排障顺序组织：先判断服务有没有起来，再看网络端口，再看 MySQL/Redis，再看业务链路，最后定位到具体模块。它适合用于面试现场演示，也适合自己复习项目时快速找问题。

## 1. 排障总原则

Atlas 的请求链路大致是：

```text
client
  -> socket / epoll / HttpConnection
  -> HTTP parser
  -> Router
  -> Controller
  -> Service
  -> Repository
  -> MySQL / Redis-Limiter / local storage
```

排障不要一上来就读代码。先确认系统状态，再缩小范围：

```text
1. 进程是否存在
2. 端口是否监听
3. 健康检查是否成功
4. 数据库是否可连
5. Redis 限流是否可用
6. API 参数是否正确
7. 权限和 token 是否正确
8. 业务状态是否符合预期
9. 文件对象是否存在
10. 日志和专项脚本是否能复现
```

## 2. 快速健康检查

### 2.1 查看端口

```bash
ss -ltnp
```

重点看 Atlas 配置端口是否处于 `LISTEN`。如果没有监听，要回到启动日志、配置文件和进程状态。

### 2.2 健康检查

```bash
curl -i http://127.0.0.1:9006/healthz
```

预期：

```text
HTTP/1.1 200 OK
```

如果 `/healthz` 都失败，通常不是业务问题，而是服务未启动、端口错误、监听地址错误、防火墙或反向代理配置问题。

### 2.3 私有接口检查

```bash
curl -i http://127.0.0.1:9006/api/private/ping \
  -H "Authorization: Bearer <token>"
```

`/healthz` 成功但私有接口失败，重点看 token、session、数据库和认证逻辑。

## 3. 启动失败排查

| 现象 | 常见原因 | 排查方向 |
| --- | --- | --- |
| 端口没有监听 | 程序启动失败 | 看启动输出、配置文件、依赖库 |
| `Address already in use` | 端口被占用 | `ss -ltnp` 找占用进程，换端口或停旧进程 |
| MySQL 连接失败 | DB 未启动、账号密码错误、库不存在 | 检查 `server.conf`、Docker、迁移 |
| 迁移失败 | schema 不匹配、权限不足 | 看 `scripts/migrate_db.sh` 输出 |
| 上传目录不存在 | `webroot/uploads` 缺失或权限不对 | 检查目录权限和可写性 |

## 4. 数据库排查

### 4.1 确认迁移版本

```sql
SELECT * FROM schema_migrations ORDER BY version;
```

如果迁移版本不完整，可能出现字段缺失、trigger 缺失或业务 SQL 失败。

### 4.2 用户表

```sql
SELECT id, username, created_at, disabled
FROM users
ORDER BY id;
```

常见问题：

- 用户不存在，登录必然失败。
- `username` 与你以为的用户 id 不是一回事。
- 修改 `users.id` 会影响所有外键，不能随便改。

### 4.3 会话表

```sql
SELECT id, user_id, expires_at, revoked_at
FROM user_sessions
WHERE user_id = 1
ORDER BY id DESC;
```

排查点：

- token 是否过期。
- 是否已经登出。
- `user_id` 是否还指向正确用户。

### 4.4 文件逻辑表

```sql
SELECT id, user_id, folder_id, filename, physical_id, size,
       deleted_at, is_public, created_at
FROM files
WHERE user_id = 1
ORDER BY id DESC;
```

重点看：

- 文件是否在回收站：`deleted_at IS NOT NULL`
- 文件是否属于当前用户：`user_id`
- 文件是否还关联物理对象：`physical_id`

### 4.5 物理文件表

```sql
SELECT id, sha256, stored_name, size, ref_count, created_at
FROM physical_files
ORDER BY id DESC;
```

`ref_count` 的解释：

- 它代表有多少逻辑文件引用这份物理内容。
- 回收站文件是否计入，要以当前 trigger 和业务规则为准。
- 不建议应用层手动改 `ref_count`，应该通过 `files` 变化触发维护。

### 4.6 分享表

```sql
SELECT id, file_id, token, expires_at, max_downloads, download_count
FROM file_shares
ORDER BY id DESC;
```

分享下载失败时重点看：

- token 是否存在。
- 是否过期。
- 下载次数是否达到上限。
- 关联文件是否仍然存在且允许访问。

## 5. 注册登录问题

### 5.1 注册返回 400

可能原因：

- username 为空或格式不符合要求。
- password 太短。
- JSON 格式错误。
- 用户名已存在。

排查顺序：

```text
1. 看请求体 Content-Type 是否为 application/json
2. 看 JSON 是否合法
3. 看 users 表是否已有同名用户
4. 看服务端日志中的参数校验错误
```

### 5.2 登录返回 401

可能原因：

- 用户不存在。
- 密码错误。
- 密码 hash 校验失败。
- 用户被禁用。

排查：

```sql
SELECT id, username, disabled
FROM users
WHERE username = '12';
```

不要直接从 DB 里看明文密码，因为项目不应该保存明文密码。

### 5.3 登录返回 429

这是限流触发，不是认证失败。

排查点：

- 同一 IP 登录太频繁。
- 同一 username 登录太频繁。
- Redis-Limiter 返回拒绝。
- Redis 不可用时 fallback 策略生效。

面试回答：

```text
登录接口同时按 IP 和用户名两个维度限流。按 IP 可以挡单个来源暴力请求，按用户名可以挡分布式来源集中撞同一个账号。限流组件在 Redis-Limiter 项目里，Atlas 只做业务适配。
```

## 6. 上传失败排查

上传链路是 Atlas 最复杂的业务链路：

```text
鉴权
  -> multipart/chunked 解析
  -> 临时文件落盘
  -> 计算 SHA-256
  -> 检查目录
  -> 检查文件名冲突
  -> 锁用户行检查配额
  -> 查重 physical_files
  -> 插入 files
  -> commit
  -> 移动/保留物理文件
```

### 6.1 返回 401

先确认 token：

```bash
curl -i http://127.0.0.1:9006/api/private/ping \
  -H "Authorization: Bearer <token>"
```

私有 ping 失败，上传不用继续查。

### 6.2 返回 400

常见原因：

- multipart 格式不正确。
- 表单字段名不对。
- 文件名为空。
- `folder_id` 非法。
- 文件超过单文件大小限制。

排查文件：

- `http/files/multipart_parser.cpp`
- `http/controllers/file_controller.cpp`
- `service/files/upload_service.cpp`

### 6.3 返回 403

通常是权限问题，例如目标目录不属于当前用户。

排查：

```sql
SELECT id, user_id, parent_id, name, deleted_at
FROM folders
WHERE id = <folder_id>;
```

### 6.4 返回 409

可能是文件名冲突，或者同一目录下已有同名文件。

排查：

```sql
SELECT id, filename, folder_id, deleted_at
FROM files
WHERE user_id = <user_id>
  AND folder_id = <folder_id>
  AND filename = '<filename>';
```

### 6.5 返回 413

表示请求体或文件太大。要看配置里的单文件大小限制和服务器读取限制。

### 6.6 返回 507 或配额不足

配额不足时重点看用户已用容量。当前项目通过服务端检查配额，不能依赖前端限制。

排查思路：

```sql
SELECT SUM(size)
FROM files
WHERE user_id = <user_id>
  AND deleted_at IS NULL;
```

如果并发上传时出现配额穿透，要重点看 Service 层是否对用户行加锁，专项测试是 `scripts/test_upload_quota_race.sh`。

## 7. 下载失败排查

下载链路：

```text
鉴权
  -> 查 files
  -> 校验 owner / public share
  -> 查 physical_files
  -> 定位磁盘 stored_name
  -> 构造文件响应
```

### 7.1 404

可能原因：

- 文件 id 不存在。
- 文件属于其他用户。
- 文件在回收站。
- 物理文件记录不存在。

SQL：

```sql
SELECT id, user_id, filename, physical_id, deleted_at
FROM files
WHERE id = <file_id>;
```

### 7.2 DB 有记录但磁盘没有文件

这是 DB 与磁盘不一致。当前项目应通过巡检脚本发现：

```bash
./scripts/check_storage_consistency.sh
```

如果出现这种情况，面试要承认：

```text
本地磁盘和 MySQL 不是一个事务系统，不能做到真正原子。当前项目通过操作顺序和巡检脚本降低风险；生产化会引入对象存储状态字段和 outbox 补偿任务。
```

### 7.3 下载很慢

排查：

- 文件是否很大。
- 是否在 Docker 文件系统上。
- 是否有大量并发下载。
- `write()` 是否频繁返回 EAGAIN。
- 是否缺少 Range / sendfile 优化。

相关文件：

- `http/files/file_response.cpp`
- `http/core/io.cpp`
- `http/core/connection.cpp`

## 8. 删除、回收站和恢复排查

### 8.1 删除后文件还在列表里

区分：

- 普通文件列表应该过滤 `deleted_at IS NULL`。
- 回收站列表应该查询 `deleted_at IS NOT NULL`。

SQL：

```sql
SELECT id, filename, deleted_at
FROM files
WHERE user_id = <user_id>
ORDER BY id DESC;
```

### 8.2 恢复失败

常见原因：

- 原目录不存在或被删除。
- 同目录下已有同名文件。
- 文件不属于当前用户。

恢复时不能简单把 `deleted_at` 置空，要检查命名冲突和目录状态。

### 8.3 永久删除后磁盘文件还在

如果 `ref_count` 已经为 0 但磁盘文件仍在，属于清理失败或待清理状态。当前单机版本可以用巡检脚本发现；生产版本应使用 outbox 后台任务重试。

## 9. 分享问题排查

### 9.1 分享链接访问 404

检查：

```sql
SELECT id, file_id, token, expires_at, max_downloads, download_count
FROM file_shares
WHERE token = '<token>';
```

可能原因：

- token 不存在。
- 分享已取消。
- 分享过期。
- 文件被删除。

### 9.2 分享码错误

不要保存明文分享码。排查时确认服务端做的是 hash 对比，不能直接从 DB 找明文。

### 9.3 下载次数并发问题

如果 `max_downloads` 限制为 1，同时两个请求下载，必须保证只成功一个。排查 Service 或 Repository 是否使用事务和行锁。

专项测试：

```bash
./scripts/test_share_race.sh
```

## 10. Redis-Limiter 排查

Atlas 不内嵌限流算法源码，只保留业务适配。限流问题要分两层看：

```text
Atlas service/rate_limit/auth_rate_limiter.cpp
Redis-Limiter 独立项目
```

排查维度：

| 现象 | 可能原因 |
| --- | --- |
| 所有登录都被拒绝 | fail-closed、规则配置过严、Redis 状态异常 |
| 限流完全不生效 | 适配层没有调用、key 设计错误、fallback fail-open |
| 偶发 429 | IP 或用户名维度达到阈值 |
| Redis 连接慢 | Redis 压力、网络、连接池不足 |

面试表达：

```text
Atlas 只负责构造登录 IP、登录用户名、注册 IP 这些业务 key；真正的令牌桶、Lua 原子扣减、Redis TIME 和 fallback 在 Redis-Limiter 组件里。这样两个项目可以分开维护，也方便其他服务复用限流能力。
```

## 11. 日志和审计排查

普通日志回答“程序发生了什么”，操作审计回答“用户做了什么”。

操作审计表可以查：

```sql
SELECT id, user_id, action, resource_type, resource_id,
       success, ip, created_at
FROM operation_logs
ORDER BY id DESC
LIMIT 50;
```

排查某个用户的上传/删除/分享行为时，先看审计表，再看业务表。

## 12. 压测异常排查

压测前固定变量：

- 是否 Docker 部署。
- MySQL 和 Redis 是否同机。
- 是否开启日志。
- 数据库是否已有大量历史数据。
- 上传文件大小是否一致。
- wrk 并发和线程数是否一致。

不要只看 QPS。要同时看：

- HTTP 错误率
- p95 / p99 延迟
- 数据库连接池使用率
- CPU idle
- 内存增长
- 文件一致性检查
- 上传后 `ref_count` 是否正确

相关脚本：

```bash
./scripts/run_benchmark_suite.sh
./scripts/run_smoke_suite.sh
./scripts/test_upload_race_consistency.sh
./scripts/test_upload_quota_race.sh
./scripts/test_ref_count_consistency.sh
```

## 13. 常见事故处理

### 13.1 MySQL 不可用

影响：

- 注册登录失败。
- 私有接口无法鉴权。
- 文件列表、上传、下载元数据查询失败。

处理：

```text
1. 停止继续压测或大流量请求
2. 检查 MySQL 进程和连接数
3. 检查 Atlas DB 配置
4. 查看是否有慢 SQL 或锁等待
5. 恢复后跑 smoke 和一致性检查
```

### 13.2 Redis 不可用

影响：

- 登录/注册限流进入 fallback。
- 具体放行还是拒绝取决于配置。

处理：

```text
1. 确认 Redis 是否恢复
2. 查看 Atlas 限流适配层日志
3. 确认 fallback 策略是否符合当前业务风险
4. 恢复后观察 429 比例是否回落
```

### 13.3 上传目录满

影响：

- 上传临时文件失败。
- 已有下载可能不受影响。

处理：

```text
1. 暂停上传入口或降低上传并发
2. 删除过期 tmp 文件
3. 跑 storage consistency checker
4. 扩容磁盘或迁移到对象存储
```

### 13.4 DB 与磁盘不一致

处理：

```text
1. 不要手动乱删 DB
2. 先跑一致性检查脚本
3. 确认是 missing object 还是 orphan object
4. missing object 影响用户下载，需要标记异常或恢复备份
5. orphan object 不影响用户逻辑视图，可以延迟删除
```

## 14. 代码定位表

| 问题 | 优先看 |
| --- | --- |
| 连接、epoll、线程池 | `app/webserver.cpp`、`app/webserver_sub_reactor.cpp`、`http/core/connection.cpp` |
| HTTP 解析 | `http/core/parser.cpp`、`http/core/http_message.cpp` |
| 路由 | `http/router/router.cpp` |
| 认证 | `http/controllers/auth_controller.cpp`、`service/auth/auth_service.cpp` |
| token | `service/auth/auth_service.cpp`、`repo/mysql/session_repository.cpp` |
| 上传 | `http/files/multipart_parser.cpp`、`service/files/upload_service.cpp` |
| 文件列表/删除/恢复 | `service/files/file_service.cpp`、`repo/mysql/file_repository.cpp` |
| 分享 | `service/files/share_service.cpp` |
| 限流 | `service/rate_limit/auth_rate_limiter.cpp` |
| DB 连接池 | `infra/db/sql_connection_pool.cpp` |
| 定时器 | `infra/timer/heap_timer.cpp` |

## 15. 演示前检查清单

演示前按顺序做：

```text
1. git status 干净
2. Docker / MySQL / Redis 状态正常
3. 迁移版本完整
4. /healthz 成功
5. 注册用户成功
6. 登录拿 token 成功
7. 上传小文件成功
8. 文件列表能看到
9. 下载内容正确
10. 删除进入回收站
11. 恢复成功
12. 分享链接成功
13. 限流脚本能触发 429
14. storage consistency checker 无异常
15. smoke suite 通过
```

## 16. 面试中的排障讲法

如果面试官问“线上上传失败你怎么排查”，可以这样答：

```text
我不会直接猜代码 bug，会按链路分层排查。先看服务端口和健康检查，再看 token 鉴权是否通过，然后确认 multipart 请求格式、目标目录、文件名冲突和配额。业务层重点看 upload_service 的事务边界：临时文件是否生成、sha256 是否计算、用户行是否加锁、physical_files 是否去重、files 是否插入。最后跑一致性检查脚本确认 DB 和磁盘没有出现 missing object 或 orphan object。
```

如果问“DB 和磁盘不一致怎么办”，可以这样答：

```text
先分类。DB 有记录但文件丢失是用户可见故障，需要阻断下载并尝试从备份恢复；磁盘有文件但 DB 没记录是孤儿文件，可以延迟清理。根本方案不是让请求线程硬重试，而是把对象操作变成 outbox 任务，失败可观测、可重试、可巡检。
```

## 17. 最终结论

Atlas 的排障重点不是背命令，而是记住链路：

```text
网络 -> HTTP -> 路由 -> 鉴权 -> Service 事务 -> Repository SQL -> MySQL/Redis/Storage
```

只要能按这条链路定位，就能把大多数问题从“玄学失败”拆成具体模块的问题。
