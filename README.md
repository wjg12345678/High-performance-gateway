# Atlas WebServer

![C++](https://img.shields.io/badge/C%2B%2B-11%2F17-blue)
![Platform](https://img.shields.io/badge/Platform-Linux%20%2B%20Docker-0f766e)
![Protocol](https://img.shields.io/badge/Protocol-HTTP%2F1.1%20%2B%20HTTPS-b45309)
![Database](https://img.shields.io/badge/Database-MySQL%208-2563eb)
![License](https://img.shields.io/badge/License-MIT-15803d)

Atlas WebServer 是一个基于 C++、Linux `epoll` 和 MySQL 的工程化 Web 服务项目。服务采用 `Main Reactor + Multi-SubReactor + Thread Pool` 并发模型，提供 HTTP/1.1、静态资源、JSON API、Bearer Token 鉴权、文件管理、操作审计、Docker Compose 部署、脚本化测试和性能分析材料。

## 功能特性

- 并发模型：主从 Reactor、SubReactor 连接分发、线程池业务处理、连接超时回收
- 协议能力：HTTP/1.1、Keep-Alive、静态资源、JSON API、可选 HTTPS
- 认证能力：用户注册登录、Bearer Token、会话持久化、私有接口访问控制
- 文件能力：文件上传、列表、下载、删除、公开可见性切换、公开下载
- 审计能力：登录、登出、上传、下载、删除、公开下载等操作日志
- 工程能力：配置文件、环境变量覆盖、容器化部署、健康检查、冒烟测试、压测脚本、FlameGraph 分析

## 系统架构

```mermaid
flowchart LR
    Client[Client]
    Main[Main Reactor]
    Sub[SubReactors]
    Pool[Thread Pool]
    HTTP[HTTP Core + API + File Service]
    DB[(MySQL)]
    FS[(root/uploads)]
    Log[Log]

    Client --> Main --> Sub --> Pool --> HTTP
    HTTP --> DB
    HTTP --> FS
    HTTP --> Log
```

主 Reactor 负责监听和连接接入，新连接按轮询方式分配给 SubReactor。SubReactor 维护连接读写事件和超时堆，业务处理交给线程池执行。HTTP 层完成请求解析、鉴权、路由、数据库访问、文件操作和响应拼装。

更完整的架构说明见 [docs/architecture.md](docs/architecture.md) 和 [docs/request-sequence.md](docs/request-sequence.md)。

## 目录结构

```text
.
|-- main.cpp
|-- webserver.cpp
|-- webserver_sub_reactor.cpp
|-- config.cpp
|-- server.conf
|-- http/
|   |-- core/
|   |-- api/
|   `-- files/
|-- CGImysql/
|-- threadpool/
|-- timer/
|-- log/
|-- lock/
|-- root/
|-- docker/
|-- scripts/
|-- docs/
`-- reports/
```

关键目录：

- `http/core/`：HTTP 解析、路由、读写、响应封装和运行时状态
- `http/api/`：认证、会话、私有接口和操作日志
- `http/files/`：文件上传、下载、列表、删除和元数据管理
- `CGImysql/`：MySQL 连接池
- `threadpool/`：动态线程池和任务队列
- `timer/`：连接超时管理
- `root/`：静态页面资源和上传目录
- `scripts/`：冒烟测试、压测和性能采样脚本
- `docs/`：架构、接口、文件模块、性能报告等补充文档

## 快速开始

### Docker Compose

推荐使用 Docker Compose 进行本地联调和验证。

```bash
docker compose up -d --build
curl -i http://127.0.0.1:9006/healthz
```

默认访问地址：

- Web 服务：`http://127.0.0.1:9006`
- MySQL：`127.0.0.1:3307`

停止服务：

```bash
docker compose down
```

### 本地编译

环境要求：

- Linux 或兼容的容器环境
- `g++`
- `make`
- `libmysqlclient`
- `OpenSSL`
- 可访问的 MySQL 8 实例

编译并运行：

```bash
make server

export TWS_DB_HOST=127.0.0.1
export TWS_DB_PORT=3306
export TWS_DB_USER=root
export TWS_DB_PASSWORD=root
export TWS_DB_NAME=qgydb

./server
```

## 配置说明

默认配置文件为 [server.conf](server.conf)。环境变量优先级高于配置文件，适合覆盖端口、数据库、线程池、日志和 HTTPS 配置。

| 配置项 | 环境变量 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `port` | `TWS_PORT` | `9006` | 服务监听端口 |
| `log_write` | `TWS_LOG_WRITE` | `1` | 日志模式，`0` 同步，`1` 异步 |
| `log_level` | `TWS_LOG_LEVEL` | `1` | 日志级别 |
| `log_split_lines` | `TWS_LOG_SPLIT_LINES` | `800000` | 单个日志文件最大行数 |
| `log_queue_size` | `TWS_LOG_QUEUE_SIZE` | `800` | 异步日志队列大小 |
| `trig_mode` | `TWS_TRIG_MODE` | `3` | epoll 触发模式组合 |
| `opt_linger` | `TWS_OPT_LINGER` | `0` | 是否启用 linger 关闭策略 |
| `close_log` | `TWS_CLOSE_LOG` | `0` | 是否关闭日志 |
| `actor_model` | `TWS_ACTOR_MODEL` | `0` | 并发模型参数 |
| `daemon_mode` | `TWS_DAEMON_MODE` | `0` | 是否以守护进程方式运行 |
| `thread_num` | `TWS_THREAD_NUM` | `8` | 基础工作线程数 |
| `threadpool_max_threads` | `TWS_THREADPOOL_MAX_THREADS` | `8` | 线程池最大线程数 |
| `threadpool_idle_timeout` | `TWS_THREADPOOL_IDLE_TIMEOUT` | `30` | 空闲线程回收秒数 |
| `threadpool_queue_mode` | `TWS_THREADPOOL_QUEUE_MODE` | `mutex` | 任务队列模式，支持 `mutex`、`lockfree` |
| `sql_num` | `TWS_SQL_NUM` | `8` | MySQL 连接池大小 |
| `mysql_idle_timeout` | `TWS_MYSQL_IDLE_TIMEOUT` | `60` | MySQL 空闲连接回收秒数 |
| `conn_timeout` | `TWS_CONN_TIMEOUT` | `15` | HTTP 空闲连接超时秒数 |
| `https_enable` | `TWS_HTTPS_ENABLE` | `0` | 是否启用 HTTPS |
| `https_cert_file` | `TWS_HTTPS_CERT_FILE` | `./certs/server.crt` | HTTPS 证书路径 |
| `https_key_file` | `TWS_HTTPS_KEY_FILE` | `./certs/server.key` | HTTPS 私钥路径 |
| `auth_token` | `TWS_AUTH_TOKEN` | 空 | 兼容鉴权 Token，生产环境建议使用环境变量 |
| `db_host` | `TWS_DB_HOST` | `127.0.0.1` | 数据库主机 |
| `db_port` | `TWS_DB_PORT` | `3306` | 数据库端口 |
| `db_user` | `TWS_DB_USER` | `root` | 数据库用户名 |
| `db_password` | `TWS_DB_PASSWORD` | 空 | 数据库密码 |
| `db_name` | `TWS_DB_NAME` | `qgydb` | 数据库名 |
| `pid_file` | `TWS_PID_FILE` | `./atlas-webserver.pid` | PID 文件路径 |

示例：

```bash
TWS_LOG_WRITE=0 TWS_THREADPOOL_QUEUE_MODE=mutex docker compose up -d --build
```

## API 概览

通用约定：

- Base URL：`http://127.0.0.1:9006`
- JSON 接口默认使用 `application/json`
- 私有接口使用 `Authorization: Bearer <token>`
- 成功响应通常返回 `{"code":0,...}`
- 错误响应通常返回 `{"code":<http_status>,"message":"..."}`

主要接口：

| 分组 | 接口 |
| --- | --- |
| 健康检查 | `GET /healthz` |
| 页面入口 | `GET /`、`/login.html`、`/register.html`、`/welcome.html`、`/files.html`、`/share.html` |
| 认证 | `POST /api/register`、`POST /api/login` |
| 私有接口 | `GET /api/private/ping`、`POST /api/private/logout`、`GET /api/private/operations`、`DELETE /api/private/operations/:id` |
| 文件接口 | `GET /api/private/files`、`POST /api/private/files`、`GET /api/private/files/:id/download`、`DELETE /api/private/files/:id`、`POST /api/private/files/:id/visibility` |
| 公开文件 | `GET /api/files/public`、`GET /api/files/public/:id`、`GET /api/files/public/:id/download` |

完整请求字段、响应示例和错误码见 [docs/api.md](docs/api.md)。

## 测试与验证

服务启动后执行冒烟测试：

```bash
./scripts/run_smoke_suite.sh
```

分项脚本：

- `./scripts/test_auth.sh`：注册、登录和登出链路
- `./scripts/test_private_api.sh`：私有接口鉴权链路
- `./scripts/test_files.sh`：文件上传、列表、下载和删除链路
- `./scripts/test_file_workflow.sh`：兼容旧入口的文件流程

手工检查：

```bash
curl -i http://127.0.0.1:9006/healthz
curl -i http://127.0.0.1:9006/
```

## 性能与分析

仓库提供可复现的 `wrk` 压测脚本、结构化基准数据和 FlameGraph 采样脚本。

```bash
./scripts/run_benchmark_suite.sh
./scripts/profile_perf_flamegraph.sh
```

当前性能资料包括：

- [docs/benchmark.md](docs/benchmark.md)：压测环境、场景矩阵、吞吐、延迟、错误和瓶颈分析
- [docs/benchmark.csv](docs/benchmark.csv)：结构化性能数据
- [docs/perf-flamegraph.md](docs/perf-flamegraph.md)：`perf + FlameGraph` 使用说明
- [reports/](reports/)：压测和采样产物

### 最佳配置压测快照

2026 年 4 月 23 日使用当前最佳默认方案做了一轮完整压测：

- 线程池：`TWS_THREADPOOL_QUEUE_MODE=mutex`
- 线程数：`TWS_THREAD_NUM=8`，`TWS_THREADPOOL_MAX_THREADS=8`
- 日志：`TWS_LOG_WRITE=1`
- epoll：`trig_mode=3`，即 `ET/ET`
- 压测工具：`wrk -t4 -d15s`
- 结果文件：`reports/benchmarks/best_mutex_fixed_20260423/results.csv`

注意：这轮压测期间 `web` 容器发生 4 次重启，日志出现 `server received SIGSEGV`。因此该数据只适合作为“当前最佳配置下的非稳定压测快照”，不能作为 release benchmark 或稳定版最终成绩。

| 接口 | 并发 | 早期 Req/s | 本次 Req/s | 变化 |
| --- | ---: | ---: | ---: | ---: |
| `/healthz` | 100 | 6898.92 | 10461.68 | +51.6% |
| `/healthz` | 200 | 5016.87 | 7325.45 | +46.0% |
| `/healthz` | 500 | 4476.61 | 8368.53 | +86.9% |
| `/` | 100 | 5165.27 | 6549.32 | +26.8% |
| `/` | 200 | 4299.95 | 4912.58 | +14.2% |
| `/` | 500 | 4851.51 | 6061.33 | +24.9% |
| `/api/private/ping` | 100 | 6654.09 | 8336.97 | +25.3% |
| `/api/private/ping` | 200 | 4678.28 | 7714.78 | +64.9% |
| `/api/private/ping` | 500 | 5077.29 | 9620.54 | +89.5% |
| `/api/login` | 100 | 875.10 | 988.30 | +12.9% |
| `/api/login` | 200 | 491.31 | 883.74 | +79.9% |
| `/api/login` | 500 | 426.50 | 726.94 | +70.4% |
| `/api/private/files` | 100 | 2196.76 | 5356.82 | +143.9% |
| `/api/private/files` | 200 | 3103.96 | 2605.79 | -16.0% |
| `/api/private/files` | 500 | 2173.21 | 2319.48 | +6.7% |
| `/api/private/files POST` | 100 | 330.92 | 350.47 | +5.9% |
| `/api/private/files POST` | 200 | 394.72 | 376.89 | -4.5% |
| `/api/private/files POST` | 500 | 320.38 | 303.31 | -5.3% |

结论：轻量读接口整体明显提升，`/healthz`、`/api/private/ping`、`/api/login` 提升最大；文件列表在 `c=100` 提升明显，但 `c=200` 退化；上传接口基本没有改善，`200/500` 并发略降。由于压测期间发生 `SIGSEGV`，下一步应优先定位崩溃，再继续讨论吞吐优化。

说明：性能数据受机器、Docker 资源、日志模式、线程池参数、数据库状态影响明显，跨环境对比前应重新采集。

## 运维说明

- Docker Compose 默认挂载 `./root/uploads` 保存上传文件
- MySQL 数据通过 Docker volume `mysql-data` 持久化
- 敏感配置建议使用环境变量注入，不要写入 `server.conf`
- 启用 HTTPS 前需要准备证书并配置 `https_cert_file` 和 `https_key_file`
- 健康检查接口为 `GET /healthz`

## 文档索引

- [架构说明](docs/architecture.md)
- [接口文档](docs/api.md)
- [请求时序](docs/request-sequence.md)
- [文件模块](docs/file-module.md)
- [性能报告](docs/benchmark.md)
- [FlameGraph 指南](docs/perf-flamegraph.md)
- [发布说明](RELEASE_NOTES.md)

## 许可证

[MIT License](LICENSE)
