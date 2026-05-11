# 面试展示材料

本文档用于秋招面试讲解。目标不是罗列功能，而是把项目里的工程问题、设计取舍、验证方式和可追问点讲清楚。

## 项目定位

Atlas WebServer 是一个 C++ Linux WebServer 后端，从传统 `epoll + 线程池 + MySQL` 服务器演进到带账号体系、Bearer Token 会话、网盘文件管理、公开分享、上传配额、审计日志、数据库迁移、Docker Compose 和回归测试的完整工程。

一句话讲法：

> 这是一个基于 C++17、epoll、线程池、MySQL 和 Redis 的网盘型 WebServer。我重点做了 HTTP 层解耦、登录/注册限流、文件去重、引用计数一致性、并发配额控制，以及 DB 和磁盘文件之间的一致性补偿。

## 核心亮点

| 方向 | 面试讲法 | 对应验证 |
| --- | --- | --- |
| 网络模型 | 主 Reactor 负责 accept，SubReactor 管连接事件，线程池处理业务任务 | `docs/architecture.md` |
| HTTP 解耦 | `HttpConnection` 只处理 IO、解析、响应落地，业务入口改为 `HttpRequest` / `RequestContext` / `HttpResponse` | 编译、单测、smoke |
| 认证会话 | PBKDF2 密码存储，Bearer Token 持久化，会话滑动过期和注销 | `scripts/test_auth.sh` |
| 文件去重 | 文件内容按 SHA-256 去重，多个逻辑文件复用一条 `physical_files` | `scripts/test_ref_count_consistency.sh` |
| 引用计数 | `ref_count` 由 MySQL trigger 维护，避免应用层手动增减不一致 | `migrations/006_ref_count_triggers.sql` |
| 并发控制 | 上传路径用事务、用户行锁、物理文件行锁保护配额和去重 | `scripts/test_upload_race_consistency.sh` |
| 配额竞争 | 并发上传时最终用量不超过用户 quota | `scripts/test_upload_quota_race.sh` |
| 故障注入 | 模拟提交前失败，验证 DB 回滚和磁盘清理 | `scripts/test_upload_failure_cleanup.sh` |
| DB/磁盘补偿 | 巡检 `files`、`physical_files`、`webroot/uploads`，发现/清理 orphan | `scripts/check_storage_consistency.sh` |

## 重点工程问题

### 1. HTTP 和业务为什么要解耦

早期 WebServer 项目容易把 socket 状态、HTTP 解析、路由、鉴权、业务 SQL 都放进连接对象里。这样会导致：

- controller 难以单独测试
- 业务函数依赖连接生命周期
- 上传临时文件、响应写回和业务错误处理耦合
- 后续加接口时容易继续堆进 `HttpConnection`

当前设计：

- `HttpConnection`：解析请求、管理 IO、落地响应、释放临时上传文件
- `HttpRequest`：方法、路径、query、header、body、上传文件元信息
- `RequestContext`：当前用户、MySQL 连接、doc root、上传配额等请求上下文
- `HttpResponse`：状态码、header、body、文件响应
- controller/service/repository：只依赖业务上下文，不直接依赖连接对象

面试回答：

> 我把连接层从业务层里拆出来。连接对象只负责协议和生命周期，业务层只看请求模型和上下文。这样上传、鉴权、文件服务都可以按 controller、service、repository 分层扩展，避免继续把逻辑堆在 `HttpConnection`。

### 2. `ref_count` 一致性怎么修

原问题：

- 应用代码手动 `increment_physical_ref` / `decrement_physical_ref`
- 如果插入文件成功但加引用失败，引用数会偏小
- 如果删除逻辑文件成功但减引用失败，引用数会偏大
- 并发上传、去重、删除时更容易出现竞态

当前设计：

- `files` 是逻辑引用的唯一事实来源
- `physical_files.ref_count` 由 MySQL trigger 在 `files` insert/delete 后维护
- 上传时在一个事务内完成配额检查、物理文件查找或创建、逻辑文件插入
- 永久删除时锁住 `files` 记录，删除逻辑记录，再判断 `physical_files` 是否无引用
- DB commit 后再删除磁盘文件

边界说明：

> DB 内部可以通过事务保证强一致，但 DB 和磁盘不是同一个事务资源。磁盘删除放在 DB commit 后执行，避免 DB 回滚但文件已丢。commit 后删磁盘失败会留下 orphan 文件，所以加了 storage consistency checker 作为补偿。

### 3. 并发上传同一个文件如何保证只有一份物理文件

关键点：

- `physical_files.sha256` 有唯一约束
- 上传事务内先按 SHA-256 查询物理文件，并使用 `FOR UPDATE`
- 如果没有物理文件，插入 `physical_files`
- 如果并发插入撞唯一约束，回查并复用已有物理文件
- `files` 插入后 trigger 自动增加 `ref_count`

验证：

```bash
./scripts/test_upload_race_consistency.sh
```

该脚本默认 12 个并发上传同一份内容，断言：

- 只有 1 条 `physical_files`
- `ref_count = 12`
- 磁盘只有 1 个物理文件
- 并发永久删除后 `files` 清空，`physical_files` 删除，磁盘文件删除

### 4. 并发配额怎么防穿透

问题：

> 如果 10 个上传同时读到当前已用容量都是 0，就可能都判断允许，最终超过 quota。

当前设计：

- 上传事务开始后先 `SELECT user FOR UPDATE`
- 同一个用户的上传配额检查串行化
- 每个请求在锁内重新计算 `SUM(files.file_size)`
- 只有剩余空间足够才继续插入逻辑文件

验证：

```bash
./scripts/test_upload_quota_race.sh
```

默认参数：

- quota: `100B`
- 并发数: `10`
- 每个文件: `30B`

期望：

- 只成功 3 个
- 7 个返回 `user storage quota exceeded`
- 最终 DB 用量 `90B`
- 不超过 quota

### 5. 上传失败会不会留下脏数据

故障注入：

- 通过 `TWS_TEST_FAIL_UPLOAD_BEFORE_COMMIT=1` 启动临时 server
- 在物理文件已落盘、逻辑文件已插入、事务提交前主动失败

验证：

```bash
./scripts/test_upload_failure_cleanup.sh
```

断言：

- 接口返回预期错误
- `files` 没有残留
- `physical_files` 没有残留
- 磁盘没有 orphan 文件
- storage checker 仍 clean

讲法：

> 我不只测成功路径，还加了一个默认关闭的测试 failpoint，用来验证事务提交前失败时，DB 会回滚，物理文件会被清理，最终巡检仍然干净。

## 现场复现

从父目录启动完整环境：

```bash
cd /home/ubuntu/Atlas
docker compose up -d --build backend
docker compose ps backend mysql
```

默认 smoke：

```bash
docker compose exec -T backend ./scripts/run_smoke_suite.sh
```

专项验证：

```bash
docker compose exec -T backend ./scripts/test_ref_count_consistency.sh
docker compose exec -T backend ./scripts/test_upload_race_consistency.sh
docker compose exec -T backend ./scripts/test_upload_quota_race.sh
docker compose exec -T backend ./scripts/test_upload_failure_cleanup.sh
docker compose exec -T backend ./scripts/check_storage_consistency.sh --dry-run
```

本地编译和单测：

```bash
cd /home/ubuntu/Atlas/Atlas-WebServer
BUILD_DIR=build-http-decouple ./build.sh
BUILD_DIR=build-http-decouple-test ./scripts/run_unit_tests.sh
```

说明：

- `scripts/test_upload_quota_race.sh` 会临时启动低 quota server，不接进默认 smoke
- `scripts/test_upload_failure_cleanup.sh` 会临时启动带 failpoint 的 server，不接进默认 smoke
- `scripts/format_check.sh check` 依赖本机安装 `clang-format`

## 常见追问

### 为什么不用应用代码维护 `ref_count`

应用层手动维护容易出现跨语句不一致，尤其是并发上传、删除、异常返回时。现在把 `files` 作为事实来源，`ref_count` 由 trigger 维护，引用变化和逻辑文件 insert/delete 处在同一个 DB 事务里。

### 为什么 DB commit 后才删磁盘

DB 可以回滚，磁盘删除不能回滚。如果先删磁盘再 commit，commit 失败会导致 DB 仍引用一个已经不存在的文件。所以先 commit DB，再删除磁盘。commit 后删磁盘失败的风险通过 storage checker 清理 orphan 文件。

### 为什么上传时要锁用户行

配额检查依赖当前用户所有文件大小总和。如果并发请求同时检查容量，不加锁会出现每个请求都认为容量足够，最终总量超过 quota。锁用户行后，同一用户上传配额检查串行化。

### 为什么还要锁物理文件行

同 SHA-256 的物理文件需要复用。锁物理行可以保证并发请求看到稳定的去重状态；首次插入时再配合唯一约束处理竞争。

### `ref_count` 包含回收站文件吗

包含。回收站文件仍然有逻辑记录，仍占用容量和磁盘。只有永久删除 `files` 记录后，trigger 才会减少引用计数。

### DB 和磁盘能不能做到完全原子

单机 MySQL 事务不能覆盖本地文件系统操作。当前做法是 DB 内强一致，磁盘用顺序约束和巡检补偿。更进一步可以引入对象存储、异步清理任务、状态机或 outbox 表。

### 这个项目还有什么不足

可以主动讲：

- 文件系统和 DB 仍不是分布式事务，当前靠补偿脚本兜底
- 还不是断点续传或分片上传
- 默认 smoke 会复用测试库，长期运行会积累部分测试用户和操作日志
- failpoint 用环境变量控制，只用于测试环境，生产环境应编译期关闭或放进测试构建
- 当前存储是本地磁盘，生产更适合对象存储和异步清理任务

## 3 分钟讲解稿

第一段，项目背景：

> Atlas WebServer 是我基于 C++17 写的 Linux WebServer。底层保留了非阻塞 socket、epoll、主从 Reactor、线程池、HTTP/1.1 parser、chunked 和 multipart 上传、MySQL 连接池、Redis 限流这些网络编程能力。后来我把它演进成一个网盘后端，支持账号、会话、目录、上传、下载、回收站、分享、配额和审计日志。

第二段，架构分层：

> 这个项目我重点做了分层重构。`HttpConnection` 只处理连接、解析、IO 和响应落地，业务入口改成显式的 `HttpRequest`、`RequestContext`、`HttpResponse`。上层按 controller、service、repository、infra 拆开，controller 做参数和响应适配，service 做业务编排，repository 管 SQL，infra 管存储、线程池和连接池。

第三段，核心工程问题：

> 文件模块里最核心的问题是逻辑文件、物理文件和磁盘文件的一致性。相同内容会按 SHA-256 去重，多个 `files` 记录复用一条 `physical_files`。我把 `ref_count` 从应用层手动增减改成 MySQL trigger 维护，并在上传和永久删除路径加事务和行锁。上传时锁用户行保证配额不会被并发穿透，锁物理文件行和唯一约束保证同内容只生成一个物理文件。

第四段，边界和补偿：

> DB 内部可以事务化，但 DB 和磁盘不是一个事务资源。所以我把磁盘删除放在 DB commit 之后，并补了 storage consistency checker，巡检 DB 有引用但磁盘缺失、磁盘 orphan、`ref_count` 漂移等问题。测试上除了普通 smoke，我还补了并发上传、并发删除、配额竞争和 failpoint 故障注入，验证失败时不会留下脏数据。

收尾：

> 这个项目的价值不只是实现了接口，而是把 WebServer 从课程式 demo 往工程化方向推进，包括分层、迁移、并发控制、异常补偿和可复现测试。
