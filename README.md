# Atlas WebServer

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Linux](https://img.shields.io/badge/Linux-epoll%20%2B%20Reactor-0f766e)
![HTTP](https://img.shields.io/badge/HTTP-1.1%20%2B%20multipart-b45309)
![MySQL](https://img.shields.io/badge/MySQL-8.x-2563eb)
![Redis](https://img.shields.io/badge/Redis-rate%20limit-dc2626)
![Build](https://img.shields.io/badge/Build-CMake%20%2B%20Docker-7c3aed)

Atlas WebServer 是一个基于 C++17 的 Linux 网盘后端项目。项目从自研 WebServer 演进而来，底层保留 `epoll`、非阻塞 socket、主从 Reactor、线程池、HTTP/1.1 parser、chunked 和 multipart 解析、MySQL 连接池等网络编程能力；业务层实现账号会话、文件上传下载、目录、回收站、公开分享、操作审计、SHA-256 去重、配额控制、Redis 登录/注册限流和 Docker 部署验证。

这个仓库定位为 **Atlas 后端服务**。前端 Vue/Vite 管理台在兄弟目录 `Atlas-Frontend`；通用分布式限流组件在独立项目 `Redis-Limiter`。Atlas 不再内嵌限流算法源码，只保留登录/注册场景的业务适配层。

## 目录

- [项目定位](#项目定位)
- [核心能力](#核心能力)
- [系统架构](#系统架构)
- [请求链路](#请求链路)
- [模块分层](#模块分层)
- [数据库设计](#数据库设计)
- [文件存储与一致性](#文件存储与一致性)
- [认证与限流](#认证与限流)
- [API 概览](#api-概览)
- [快速启动](#快速启动)
- [配置说明](#配置说明)
- [测试与验证](#测试与验证)
- [性能与压测](#性能与压测)
- [生产化边界](#生产化边界)
- [简历与面试讲法](#简历与面试讲法)
- [文档索引](#文档索引)

## 项目定位

Atlas WebServer 不是把成熟 Web 框架套在 CRUD 上，而是从 Linux 网络服务一路做到网盘业务闭环：

- 底层：非阻塞 socket、epoll、ET/LT、EPOLLONESHOT、主从 Reactor、线程池、连接超时、Keep-Alive。
- 协议：HTTP/1.1 请求解析、Header/Query/Form/JSON 解析、chunked body、multipart/form-data、文件下载响应。
- 业务：注册登录、Bearer Token 会话、目录、文件上传下载、回收站、恢复、彻底删除、公开分享、分享码、下载次数限制。
- 数据：MySQL schema 迁移、逻辑文件/物理文件双表、SHA-256 去重、引用计数、操作审计。
- 工程：CMake、Docker Compose、自动迁移、单测、smoke、并发一致性脚本、benchmark、Sanitizer/coverage/format 检查。

推荐在简历中把它写成：

```text
Atlas｜C++ Linux 网盘系统

基于 C++17、epoll、主从 Reactor、线程池、MySQL、Redis 和 Vue/Vite 实现的前后端分离网盘系统，支持账号会话、文件上传下载、目录、回收站、公开分享、操作审计、SHA-256 去重、上传配额、登录/注册限流、Docker Compose 部署和自动化回归验证。
```

## 核心能力

| 模块 | 能力 |
| --- | --- |
| 网络模型 | 主 Reactor 监听 `listenfd` 并 accept，多个 SubReactor 负责连接读写事件，线程池执行业务任务 |
| I/O 模型 | 非阻塞 socket、ET/LT 触发模式、EPOLLONESHOT、连接超时、Keep-Alive |
| HTTP | HTTP/1.1、JSON API、form-urlencoded、chunked、multipart、HEAD/OPTIONS、文件下载响应、可选 HTTPS |
| 认证 | 注册、登录、PBKDF2-HMAC-SHA256 密码哈希、随机 token、Bearer Token、滑动过期、登出当前/全部会话 |
| 限流 | 登录 IP、登录用户名、注册 IP 三个维度限流；通过外部 Redis-Limiter 组件实现 Redis 令牌桶和本地降级 |
| 文件网盘 | 目录列表、创建/删除空目录、上传、下载、回收站、恢复、彻底删除、公开/取消公开 |
| 上传治理 | 单文件大小限制、用户总容量限制、preflight、临时文件落盘、SHA-256 去重、失败清理 |
| 分享 | 分享 token、访问码 hash、过期时间、最大下载次数、公开文件列表、公开下载 |
| 审计 | 登录、注册、上传、下载、删除、恢复、分享、限流等操作日志 |
| 数据库 | MySQL 8、版本化迁移、外键、唯一索引、CHECK、trigger 维护 `ref_count` |
| 工程验证 | CTest、shell smoke、并发上传、配额竞争、分享竞争、故障注入、DB/磁盘一致性巡检 |

## 系统架构

```text
Browser / curl / wrk
        |
        v
Frontend Nginx / reverse proxy
        |
        v
Atlas WebServer
  MainReactor
    -> accept
    -> dispatch connfd
  SubReactors
    -> epoll_wait(EPOLLIN / EPOLLOUT)
    -> read_once / write
  ThreadPool
    -> HttpConnection::process
    -> Router
    -> Controller
    -> Service
    -> Repository
        |
        +--> MySQL
        +--> Redis-Limiter -> Redis
        +--> Local disk storage
```

### 主从 Reactor

`app/webserver.cpp` 中主 Reactor 创建 `m_epollfd`，只监听 `listenfd`。新连接到来时 `dealclientdata()` accept，然后按轮询方式分发给某个 SubReactor。

`app/webserver_sub_reactor.cpp` 中每个 SubReactor 自己持有一个 epoll 和一个 `eventfd`。主 Reactor 将 `connfd` 放入 pending 队列后写 `eventfd` 唤醒 SubReactor，SubReactor 再把连接注册到自己的 epoll 中。

连接初始化时会调用 `addfd()`，设置非阻塞，并根据配置使用 `EPOLLET` 和 `EPOLLONESHOT`。`EPOLLONESHOT` 用来避免同一个连接同时被多个线程处理；处理完成后通过 `modfd()` 重新注册读写事件。

### 线程池

SubReactor 收到读事件后调用 `read_once()` 把数据读入连接缓冲区，再把 `HttpConnection` 投递到线程池。工作线程取得 MySQL 连接后执行 `HttpConnection::process()`，完成 HTTP 解析、路由分发、业务处理和响应生成。

如果响应已经准备好，连接会切换到 `EPOLLOUT`，由 SubReactor 在 socket 可写时调用 `write()` 发送。这样不是“每连接一线程”，而是 epoll 管大量连接，线程池处理相对耗时的业务和数据库访问。

## 请求链路

以登录为例：

```text
POST /api/login
  -> HttpConnection 解析 HTTP 请求
  -> Router 匹配 /api/login
  -> AuthController 读取 username/password
  -> auth_service 校验 PBKDF2 密码
  -> make_session_token 生成安全随机 token
  -> session_repository 写入 user_sessions
  -> 返回 JSON token
```

以文件上传为例：

```text
POST /api/drive/files/upload
  -> Bearer Token 鉴权
  -> multipart parser 流式解析文件 part
  -> 临时文件写入 webroot/uploads/.tmp
  -> 计算 SHA-256 和大小
  -> Service 检查用户配额、目标目录、文件名冲突
  -> physical_files 查重或插入
  -> files 插入逻辑文件记录
  -> 提交事务
  -> 临时文件转正式存储或去重复用
  -> 返回文件元数据
```

以永久删除为例：

```text
DELETE /api/drive/files/:id/permanent
  -> 鉴权
  -> SELECT file FOR UPDATE
  -> 校验文件属于当前用户且在回收站
  -> DELETE files
  -> trigger 维护 physical_files.ref_count
  -> ref_count=0 时删除 physical_files
  -> commit
  -> commit 后尝试删除磁盘物理文件
```

## 模块分层

```text
.
|-- app/                  # main、配置、WebServer、MainReactor/SubReactor 编排
|-- http/
|   |-- core/             # HttpConnection、parser、IO、response、runtime
|   |-- router/           # healthz/API 路由
|   |-- controllers/      # Auth/File/Operation Controller
|   `-- files/            # multipart parser、文件下载响应、文件辅助函数
|-- service/
|   |-- auth/             # 认证、密码、会话业务
|   |-- files/            # 文件、目录、上传、分享业务
|   `-- rate_limit/       # Atlas 登录/注册限流适配层
|-- repo/mysql/           # Repository / SQL 封装
|-- infra/                # db、storage、threadpool、timer、log、lock
|-- migrations/           # 版本化 SQL 迁移
|-- scripts/              # 迁移、测试、benchmark、coverage、format
|-- tests/                # C++ 单元测试
|-- docs/                 # 架构、API、迁移、性能、面试材料
|-- docker/               # MySQL 初始化 SQL
|-- deploy/               # Nginx 部署示例
`-- webroot/uploads/      # 本地上传文件目录
```

分层职责：

| 层 | 职责 |
| --- | --- |
| `HttpConnection` | socket IO、HTTP 解析、请求/响应生命周期、上传临时文件生命周期 |
| `Router` | 路由匹配、公共 API 分发、404/405 等协议错误 |
| `Controller` | 参数读取、认证检查、HTTP 状态码和 JSON 响应适配 |
| `Service` | 业务规则、事务边界、并发控制、失败处理 |
| `Repository` | SQL 拼装、结果映射、数据库错误返回 |
| `Infra` | 连接池、线程池、日志、定时器、存储等基础能力 |

## 数据库设计

当前 schema 采用 `users.id` 作为主键，其他业务表通过 `user_id` 外键关联。

| 表 | 作用 |
| --- | --- |
| `users` | 用户账号、密码哈希、登录时间、禁用状态 |
| `user_sessions` | Bearer Token 会话、过期时间 |
| `folders` | 用户目录，支持父子目录和软删除 |
| `files` | 用户视角的逻辑文件，记录文件名、目录、大小、公开状态、删除状态 |
| `physical_files` | 磁盘真实文件，按 SHA-256 去重，记录 `stored_name` 和 `ref_count` |
| `file_shares` | 分享 token、访问码 hash、过期时间、下载次数 |
| `operation_logs` | 操作审计，记录用户、动作、资源、详情、结果、IP、UA |
| `schema_migrations` | 已执行迁移版本 |

逻辑文件和物理文件拆分是项目的核心设计：

```text
files            用户能看到的文件记录
physical_files   磁盘真实文件对象

files.physical_id -> physical_files.id
```

这样同一个物理内容可以被多个逻辑文件引用，支撑去重、秒传扩展和物理文件引用计数。

迁移文件：

| 文件 | 说明 |
| --- | --- |
| `001_init_schema.sql` | 初始化 schema |
| `002_upgrade_existing_drive_dedup.sql` | 升级到目录 + 去重模型 |
| `003_recycle_bin_ref_counts.sql` | 回收站与引用计数修正 |
| `004_drop_passwd_salt.sql` | 清理旧密码字段 |
| `005_normalize_schema.sql` | 迁移到 `users.id` 外键模型 |
| `006_ref_count_triggers.sql` | 创建/校准 `ref_count` trigger |

自动迁移入口是 `scripts/migrate_db.sh`。详细说明见 [docs/database-migrations.md](docs/database-migrations.md)。

## 文件存储与一致性

当前文件存储使用本地磁盘：

```text
webroot/uploads/
  .tmp/                 # 上传临时文件
  <stored_name>         # 正式物理文件
```

上传一致性策略：

- 请求体先流式落到临时文件，避免大文件常驻内存。
- 计算 SHA-256 后进入数据库事务。
- 锁定用户行进行配额检查，防止并发上传超配额。
- `physical_files.sha256` 唯一索引防止并发插入同一物理文件。
- `files` 唯一索引防止同一用户同一目录下活跃同名文件冲突。
- DB 失败时清理临时文件或新物理文件。
- 去重命中时删除本次临时文件，只新增逻辑文件记录。

删除一致性策略：

- 普通删除是软删除，设置 `deleted_at`，文件进入回收站。
- 恢复时检查原目录是否存在，必要时恢复到根目录，并自动处理同名冲突。
- 永久删除在事务内删除逻辑文件和未引用物理记录。
- 物理磁盘删除放在 DB commit 之后，失败时不会破坏用户视角；后续通过巡检脚本补偿清理。

一致性检查：

```bash
scripts/check_storage_consistency.sh --dry-run
scripts/test_ref_count_consistency.sh
scripts/test_upload_race_consistency.sh
scripts/test_upload_failure_cleanup.sh
```

## 认证与限流

### 密码与会话

- 密码使用 PBKDF2-HMAC-SHA256 存储，不保存明文密码。
- 每个密码有随机 salt 和固定迭代次数。
- 登录成功后生成 32 字节安全随机 token，编码后写入 `user_sessions`。
- 请求通过 `Authorization: Bearer <token>` 鉴权。
- 会话支持过期、刷新、登出当前会话、登出全部会话。

### Redis-Limiter 解耦

Atlas 不再复制限流组件源码。限流分成两层：

```text
Redis-Limiter
  -> 通用 C++ core
  -> Redis 连接池
  -> Lua 原子限流
  -> TokenBucket / SlidingWindow
  -> Redis 故障降级
  -> Python binding

Atlas service/rate_limit
  -> 登录 IP 限流
  -> 登录用户名限流
  -> 注册 IP 限流
  -> Atlas 配置读取
  -> HTTP 429 响应适配
```

Atlas 通过 CMake 链接外部 target：

```cmake
redis_limiter::core
```

常见目录布局：

```text
workspace/
|-- Atlas-WebServer/
`-- Redis-Limiter/
```

构建时可以显式指定组件路径：

```bash
cmake -S . -B build \
  -DATLAS_REDIS_LIMITER_ROOT=/path/to/Redis-Limiter
```

如果没有安装 hiredis 或找不到 Redis-Limiter，Atlas 仍会构建运行，但认证限流会被禁用；限流算法和故障降级逻辑只由 Redis-Limiter 组件提供。

## API 概览

通用约定：

- Base URL：`http://127.0.0.1:9006`
- 私有接口使用 `Authorization: Bearer <token>`
- JSON 请求使用 `Content-Type: application/json`
- 成功响应通常包含 `"code":0`
- 错误响应通常为 `{ "code": <http_status>, "message": "..." }`

| 分组 | 接口 |
| --- | --- |
| 健康检查 | `GET /healthz`、`HEAD /healthz` |
| 调试 | `POST /api/echo` |
| 认证 | `POST /api/register`、`POST /api/login`、`GET /api/private/ping`、`POST /api/private/logout` |
| 操作日志 | `GET /api/private/operations`、`DELETE /api/private/operations/:id` |
| 目录 | `GET /api/drive/items?folder_id=0`、`POST /api/drive/folders`、`DELETE /api/drive/folders/:id` |
| 上传 | `POST /api/drive/files/preflight`、`POST /api/drive/files/upload` |
| 文件 | `GET /api/drive/files/:id/download`、`DELETE /api/drive/files/:id`、`POST /api/drive/files/:id/visibility` |
| 回收站 | `GET /api/drive/trash`、`DELETE /api/drive/trash`、`POST /api/drive/files/:id/restore`、`DELETE /api/drive/files/:id/permanent` |
| 公开文件 | `GET /api/files/public`、`GET /api/files/public/:id`、`GET /api/files/public/:id/download` |
| 分享 | `POST /api/drive/files/:id/share`、`GET /api/share/:token`、`GET /api/share/:token/download` |

完整字段和响应示例见 [docs/api.md](docs/api.md)。

## 快速启动

### Docker Compose 全栈部署

推荐从 Atlas 根目录启动完整前后端：

```bash
cp .env.example .env
docker compose up -d --build
curl -i http://127.0.0.1:${ATLAS_FRONTEND_PORT:-8080}/healthz
```

服务：

| 服务 | 说明 |
| --- | --- |
| `frontend` | Nginx 托管 Vue/Vite 构建产物，并反代 `/api/` 到后端 |
| `backend` | C++ Atlas WebServer |
| `mysql` | MySQL 8 数据库 |
| `redis` | 可选 Redis 限流后端 |
| `mysql-backup` | 定时 MySQL dump 备份 |

默认 Docker build 不引入外部 Redis-Limiter，Atlas 镜像可以独立构建运行；这种模式下认证限流会被禁用。需要启用认证限流时，在本地构建或自定义镜像中提供 Redis-Limiter 源码，并通过 CMake 指定组件路径：

```bash
cmake -S . -B build \
  -DATLAS_REDIS_LIMITER_ROOT=/path/to/Redis-Limiter
```

### 后端独立部署

在本仓库目录下启动后端、MySQL、Redis：

```bash
docker compose up -d --build
```

### 本地构建

依赖：

- Linux
- CMake 3.16+
- C++17 compiler
- OpenSSL dev
- MySQL client dev
- 可选：hiredis dev
- 可选：外部 Redis-Limiter 源码

构建：

```bash
./build.sh
./build/server
```

指定 Redis-Limiter：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DATLAS_REDIS_LIMITER_ROOT=/path/to/Redis-Limiter
cmake --build build --target server --parallel
```

## 5 分钟主链路

```bash
BASE_URL=http://127.0.0.1:9006
USER_NAME=demo
PASSWORD=123456

curl -sS -X POST "$BASE_URL/api/register" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"$USER_NAME\",\"passwd\":\"$PASSWORD\"}"

LOGIN_JSON="$(curl -sS -X POST "$BASE_URL/api/login" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"$USER_NAME\",\"passwd\":\"$PASSWORD\"}")"

TOKEN="$(printf '%s' "$LOGIN_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')"

printf 'hello atlas\n' > /tmp/atlas-demo.txt

UPLOAD_JSON="$(curl -sS -X POST "$BASE_URL/api/drive/files/upload" \
  -H "Authorization: Bearer $TOKEN" \
  -H 'Expect:' \
  -F 'file=@/tmp/atlas-demo.txt;type=text/plain' \
  -F 'filename=atlas-demo.txt' \
  -F 'is_public=false')"

FILE_ID="$(printf '%s' "$UPLOAD_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["file"]["id"])')"

curl -sS "$BASE_URL/api/drive/items?folder_id=0" \
  -H "Authorization: Bearer $TOKEN" | python3 -m json.tool

curl -i -sS "$BASE_URL/api/drive/files/$FILE_ID/download" \
  -H "Authorization: Bearer $TOKEN"
```

更多步骤见 [docs/quickstart-5min.md](docs/quickstart-5min.md)。

## 配置说明

配置来源：

1. `server.conf`
2. 环境变量
3. Docker Compose `.env`

常用环境变量：

| 环境变量 | 默认值 | 说明 |
| --- | --- | --- |
| `TWS_PORT` | `9006` | 后端监听端口 |
| `TWS_DB_HOST` | `127.0.0.1` | MySQL host |
| `TWS_DB_PORT` | `3306` | MySQL port |
| `TWS_DB_USER` | `root` | MySQL 用户 |
| `TWS_DB_PASSWORD` | 空 | MySQL 密码 |
| `TWS_DB_NAME` | `qgydb` | MySQL 数据库 |
| `TWS_SQL_NUM` | `8` | MySQL 连接池大小 |
| `TWS_THREAD_NUM` | `8` | SubReactor / 基础工作线程数 |
| `TWS_THREADPOOL_MAX_THREADS` | `8` | 动态线程池最大线程数 |
| `TWS_THREADPOOL_QUEUE_MODE` | `mutex` | 线程池队列，支持 `mutex` / `lockfree` |
| `TWS_UPLOAD_MAX_BYTES` | `104857600` | 单文件上传上限 |
| `TWS_USER_STORAGE_QUOTA_BYTES` | `1073741824` | 单用户容量上限，`0` 不限制 |
| `TWS_CONN_TIMEOUT` | `60` | HTTP 连接空闲超时 |
| `TWS_AUTH_RATE_LIMIT_ENABLED` | `1` | 是否启用认证限流 |
| `TWS_REDIS_HOST` | `redis` in compose | Redis host |
| `TWS_REDIS_PORT` | `6379` | Redis port |
| `TWS_AUTH_RATE_LIMIT_FALLBACK_MODE` | `local` | Redis 失败时的降级策略 |

HTTPS、日志、daemon、legacy compatibility 等更多配置见 [server.conf](server.conf) 和 [app/config.cpp](app/config.cpp)。

## 测试与验证

| 命令 | 说明 |
| --- | --- |
| `scripts/run_unit_tests.sh` | CTest 单元测试入口 |
| `scripts/run_smoke_suite.sh` | 认证、私有 API、文件、Drive、ref-count、并发上传、分享、chunked API 冒烟 |
| `scripts/test_auth_rate_limit.sh` | 登录/注册限流验证 |
| `scripts/test_file_workflow.sh` | 文件主链路 |
| `scripts/test_ref_count_consistency.sh` | 去重和引用计数专项 |
| `scripts/test_upload_race_consistency.sh` | 并发上传同内容一致性 |
| `scripts/test_upload_quota_race.sh` | 并发配额竞争 |
| `scripts/test_upload_failure_cleanup.sh` | 上传失败故障注入 |
| `scripts/test_share_race.sh` | 分享并发竞争 |
| `scripts/check_storage_consistency.sh --dry-run` | DB/磁盘一致性巡检 |
| `scripts/format_check.sh check` | clang-format 检查 |
| `scripts/run_coverage.sh` | coverage smoke |

本地验证示例：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target atlas-unit-tests --parallel
ctest --test-dir build --output-on-failure
```

容器验证：

```bash
docker compose exec -T backend ./scripts/run_smoke_suite.sh
```

## 性能与压测

Benchmark 入口：

```bash
scripts/run_benchmark_suite.sh
```

输出：

- `*.wrk.txt`：wrk 原始输出
- `*.stats.csv`：容器 CPU/内存采样
- `benchmark.csv`：汇总指标
- `benchmark-trusted.csv`：通过 gate 的可信样本

README 不直接宣传单次压测 headline 数字。原因是项目运行环境、Docker 状态、MySQL/Redis 延迟和机器负载都会影响结果。更稳妥的方式是给出压测方法、原始数据、gate 和复现脚本。详细说明见 [docs/benchmark.md](docs/benchmark.md)。

## 生产化边界

这个项目适合作为 C++ WebServer + 网盘业务的工程项目，但不能包装成生产级网盘。需要主动承认的边界：

- 当前文件存储是本地磁盘，不适合直接多实例部署。
- MySQL 和本地磁盘不能做真正的原子提交，只能通过事务边界、顺序约束、故障清理和巡检补偿降低不一致风险。
- Redis-Limiter 默认是单 Redis 接入，生产需要 Sentinel/Cluster、高可用和容量规划。
- 当前没有完整对象存储、断点续传、分片上传、Range 下载、文件预览、协作权限、异步任务平台。
- 本地 fallback 只能保证单实例限流，Redis 故障时不能继续保证多实例全局配额。

生产化演进方向：

- 抽象 `Storage` 接口，支持 LocalStorage 和 S3/MinIO/OSS。
- 文件上传改成对象存储直传或后端签名上传。
- 物理删除改为 outbox / cleanup job，支持重试和告警。
- 引入 Range 下载、秒传 API、分片上传会话、后台异步合并。
- MySQL 主从/备份恢复演练、Redis Sentinel/Cluster、统一监控和日志追踪。
- 认证限流规则下沉到配置中心或独立限流服务。

## 简历与面试讲法

### 30 秒介绍

```text
Atlas 是我用 C++17 做的 Linux 网盘后端。底层是 epoll、非阻塞 socket、主从 Reactor 和线程池，上层实现了 HTTP/1.1、chunked、multipart、MySQL 连接池、账号会话、文件上传下载、目录、回收站、公开分享和操作审计。文件部分采用逻辑文件和物理文件双表，通过 SHA-256 去重和 trigger 维护 ref_count，并用事务、唯一索引和行锁处理并发上传、删除和配额一致性。登录/注册限流接入了我独立实现的 Redis-Limiter 组件，Atlas 只保留业务适配层。
```

### 高频追问

| 问题 | 回答要点 |
| --- | --- |
| 为什么用 C++ 做后端 | 目标是展示 Linux 网络编程和底层工程能力，不只是业务 CRUD |
| Reactor 怎么实现 | MainReactor 只 accept，SubReactor 管连接读写，线程池跑业务 |
| ET 怎么读 | 非阻塞循环读到 `EAGAIN/EWOULDBLOCK`，否则边沿触发会漏数据 |
| 为什么逻辑文件和物理文件拆表 | 支持用户视角文件和磁盘真实对象解耦，便于 SHA-256 去重和引用计数 |
| ref_count 怎么保证 | trigger 在 `files` insert/delete 后维护，事务内结合行锁和唯一索引 |
| DB 成功但磁盘失败怎么办 | 无法原子提交，采用先逻辑状态正确，再通过补偿清理孤儿文件 |
| 限流组件怎么解耦 | Redis-Limiter 提供通用 C++ core，Atlas 只处理登录/注册 key 和 HTTP 响应 |
| 能不能多实例部署 | 当前本地磁盘不能直接多实例，生产要切对象存储并让服务无状态化 |

更多材料：

- [docs/project-study-guide-complete.md](docs/project-study-guide-complete.md)
- [docs/interview-qna-complete.md](docs/interview-qna-complete.md)
- [docs/interview-highlights.md](docs/interview-highlights.md)
- [docs/resume-and-pitch.md](docs/resume-and-pitch.md)
- [docs/final-review.md](docs/final-review.md)

## 文档索引

| 文档 | 内容 |
| --- | --- |
| [docs/quickstart-5min.md](docs/quickstart-5min.md) | 5 分钟主链路复现 |
| [docs/api.md](docs/api.md) | API 字段和响应示例 |
| [docs/architecture.md](docs/architecture.md) | 架构设计 |
| [docs/request-sequence.md](docs/request-sequence.md) | 请求时序 |
| [docs/file-module.md](docs/file-module.md) | 文件模块设计 |
| [docs/database-migrations.md](docs/database-migrations.md) | 数据库迁移 |
| [docs/project-structure.md](docs/project-structure.md) | 目录结构 |
| [docs/benchmark.md](docs/benchmark.md) | benchmark 方法 |
| [docs/perf-flamegraph.md](docs/perf-flamegraph.md) | perf flamegraph |
| [docs/project-study-guide-complete.md](docs/project-study-guide-complete.md) | 完整学习路线 |
| [docs/interview-qna-complete.md](docs/interview-qna-complete.md) | 面试完整问答 |
| [docs/interview-highlights.md](docs/interview-highlights.md) | 面试展示材料 |
| [docs/resume-and-pitch.md](docs/resume-and-pitch.md) | 简历与面试话术 |
| [docs/final-review.md](docs/final-review.md) | 项目最终复盘 |
| [docs/production-hardening-roadmap.md](docs/production-hardening-roadmap.md) | 生产化加固与升级路线 |
| [docs/ops-debug-runbook.md](docs/ops-debug-runbook.md) | 运维排障与事故处理手册 |
| [docs/interview-defense-playbook.md](docs/interview-defense-playbook.md) | 面试答辩攻防手册 |

## License

本项目使用 MIT License，详见 [LICENSE](LICENSE)。
