# Benchmark Report

这份文档记录当前仓库一套可复现的正式压测结果，重点回答 5 个问题：

- 压测场景是否分层
- 压测变量是否固定
- 是否同时记录吞吐、延迟、错误和资源占用
- 能否从结果里判断瓶颈大致落在哪一层
- 是否做过一轮前后对比实验，而不是只报一组数字

## 测试环境

- 机器：MacBook Pro（本地开发机）
- OS：macOS Sonoma 14.6, Darwin 23.6.0, x86_64
- 部署方式：`docker compose up -d`
- 压测工具：`wrk --latency`
- Web / MySQL：同机 Docker Compose 容器
- HTTPS：关闭
- 发布配置：
  - `TWS_LOG_WRITE=0`
  - `TWS_THREAD_NUM=8`
  - `TWS_SQL_NUM=8`
  - `TWS_CONN_TIMEOUT=15`

说明：

- 这台机器上同步日志模式明显优于默认异步日志模式，因此本文发布数据采用 `TWS_LOG_WRITE=0`。
- 如果要做跨机器对比，必须重新采集，不应该直接复用这里的绝对数值。

## 压测场景

- 轻量探活：`GET /healthz`
- 登录接口：`POST /api/login`
- 私有轻接口：`GET /api/private/ping`
- 文件列表：`GET /api/drive/items`
- 小文件上传：`POST /api/drive/files/upload`

## 固定变量

- 线程数：`4`
- 时长：`10s`
- 统一使用 `wrk --latency`
- `login` 请求体：`48B` JSON
- `upload` 请求体：`336B` multipart/form-data 小文件
- 鉴权接口均使用 Bearer Token

`wrk --latency` 原生输出只给到 `50/75/90/99` 分位，所以这里统一记录 `Avg / P50 / P90 / P99`，不伪造 `P95`。


## 可信 Headline 样本

正式对外只引用 `errors=none` / `Socket errors=0` 的 case。下面这组样本来自 `best_mutex_fixed_20260423`，固定 `wrk -t4 -d15s --latency`、`TWS_LOG_WRITE=0`、`TWS_THREAD_NUM=8`、`TWS_SQL_NUM=8`，Web 和 MySQL 为同机 Docker Compose 容器。

`wrk --latency` 原生只输出 `P50/P75/P90/P99`，没有 `P95`，因此表中 `P95` 记为 `NA`，不伪造不存在的分位值。结构化数据见 [benchmark-trusted.csv](benchmark-trusted.csv)。

| 场景 | 并发 | QPS | P95 | P99 | 错误率 | Web CPU 峰值 | Web 内存峰值 | MySQL CPU 峰值 | MySQL 内存峰值 |
| --- | ---: | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| `POST /api/login` | 100 | 988.30 | NA | 161.76ms | 0% | 119.25% | 118.6 MiB | 178.62% | 442 MiB |
| `GET /api/private/ping` | 100 | 8336.97 | NA | 69.31ms | 0% | 524.76% | 121 MiB | 39.87% | 441.6 MiB |
| `GET /api/drive/items` | 100 | 5356.82 | NA | 81.27ms | 0% | 536.03% | 120.5 MiB | 489.04% | 441.8 MiB |

这些数据适合作为简历和答辩中的保守口径；带 `read/timeout/write` 错误的高并发结果只用于说明拐点和瓶颈，不作为 headline。当前 benchmark runner 的上传 case 已改为 multipart，小文件上传 headline 需要重新采集到 `Socket errors=0` 后再对外引用。

## 主体矩阵

### 1. 轻接口

| 接口 | 并发 | Avg | P50 | P90 | P99 | Requests/sec | Errors |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| `/healthz` | 100 | 30.97ms | 13.35ms | 24.90ms | 710.15ms | 6898.92 | connect 0, read 0, write 0, timeout 4 |
| `/healthz` | 200 | 55.11ms | 28.14ms | 55.97ms | 848.46ms | 5016.87 | connect 0, read 1462, write 0, timeout 96 |
| `/healthz` | 500 | 101.19ms | 76.94ms | 139.49ms | 915.19ms | 4476.61 | connect 0, read 4059, write 8, timeout 214 |
| `/` | 200 | 51.75ms | 42.55ms | 72.94ms | 167.11ms | 4299.95 | connect 0, read 40, write 0, timeout 0 |
| `/` | 500 | 112.57ms | 92.01ms | 147.78ms | 815.33ms | 4851.51 | connect 0, read 586, write 0, timeout 40 |

### 2. 鉴权与文件查询

| 接口 | 并发 | Avg | P50 | P90 | P99 | Requests/sec | Errors |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| `/api/private/ping` | 100 | 15.70ms | 14.27ms | 23.92ms | 60.52ms | 6654.09 | none |
| `/api/private/ping` | 200 | 50.88ms | 39.31ms | 73.16ms | 313.87ms | 4678.28 | connect 0, read 25, write 0, timeout 0 |
| `/api/private/ping` | 500 | 59.62ms | 50.43ms | 74.79ms | 596.27ms | 5077.29 | connect 0, read 8476, write 1345, timeout 292 |
| `/api/drive/files/upload` | 100 | 55.96ms | 37.93ms | 84.00ms | 573.09ms | 2196.76 | connect 0, read 0, write 0, timeout 3 |
| `/api/drive/files/upload` | 200 | 84.73ms | 59.10ms | 80.50ms | 892.15ms | 3103.96 | connect 0, read 37, write 0, timeout 13 |
| `/api/drive/files/upload` | 500 | 214.94ms | 211.71ms | 293.53ms | 597.87ms | 2173.21 | connect 0, read 731, write 0, timeout 60 |

### 3. 重业务写路径

| 接口 | 并发 | Avg | P50 | P90 | P99 | Requests/sec | Errors |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| `/api/login` | 100 | 117.69ms | 100.92ms | 159.00ms | 295.95ms | 875.10 | none |
| `/api/login` | 200 | 396.85ms | 371.96ms | 536.22ms | 785.85ms | 491.31 | connect 0, read 0, write 0, timeout 5 |
| `/api/login` | 500 | 1.08s | 1.09s | 1.43s | 1.84s | 426.50 | connect 0, read 337, write 2, timeout 31 |
| `/api/drive/files/upload` `POST` | 100 | 300.48ms | 272.29ms | 382.70ms | 697.62ms | 330.92 | none |
| `/api/drive/files/upload` `POST` | 200 | 497.55ms | 484.32ms | 582.59ms | 1.03s | 394.72 | connect 0, read 38, write 0, timeout 0 |
| `/api/drive/files/upload` `POST` | 500 | 1.41s | 1.45s | 1.65s | 1.84s | 320.38 | connect 0, read 586, write 0, timeout 74 |

## 1000 并发补充

这组数据只补最有代表性的读场景，用来看更高并发下的吞吐拐点和错误形态，不把登录/上传这种重写路径硬压到失真。

| 接口 | 并发 | Avg | P50 | P90 | P99 | Requests/sec | Errors |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| `/healthz` | 1000 | 132.18ms | 124.92ms | 157.00ms | 425.25ms | 7581.63 | connect 0, read 3577, write 0, timeout 6 |
| `/` | 1000 | 149.16ms | 139.69ms | 192.63ms | 313.45ms | 6610.76 | connect 0, read 3218, write 0, timeout 17 |
| `/api/private/ping` | 1000 | 133.55ms | 122.46ms | 173.25ms | 364.37ms | 7497.74 | connect 0, read 3489, write 0, timeout 1 |
| `/api/drive/files/upload` | 1000 | 285.23ms | 259.80ms | 410.50ms | 930.34ms | 3127.67 | connect 0, read 3145, write 40, timeout 179 |

## 资源占用观察

基于 [benchmark.csv](benchmark.csv) 里的容器采样结果，可以提炼出几个稳定结论：

- 轻接口主要吃 `web` 容器 CPU，MySQL 占用很低。
- `GET /api/drive/items` 的 MySQL 峰值 CPU 多次接近或超过 `4~5` 个核的量级，是当前最明显的数据库热点。
- `POST /api/drive/files/upload` 的 `web` CPU 并不夸张，但延迟很高，说明瓶颈不只是 Web 线程本身，更像是鉴权、multipart 解析、写库和磁盘写入叠加造成的长链路开销。

## 前后对比实验

在 200 并发下对比两种日志模式：

| 接口 | 异步日志 `TWS_LOG_WRITE=1` | 同步日志 `TWS_LOG_WRITE=0` | 变化 |
| --- | --- | --- | --- |
| `/healthz` | 4110.59 req/s, P99 552.73ms | 6793.47 req/s, P99 417.65ms | 明显提升 |
| `/api/login` | 456.86 req/s, P99 797.20ms | 633.83 req/s, P99 611.20ms | 明显提升 |
| `/api/drive/files/upload` `GET` | 2045.00 req/s, P99 262.81ms | 2780.26 req/s, P99 180.96ms | 明显提升 |
| `/api/drive/files/upload` `POST` | 197.22 req/s, P99 1.64s | 291.28 req/s, P99 1.08s | 明显提升 |

结论：

- 当前实现里，异步日志并不是收益点，反而更像额外的锁竞争或队列开销来源。
- 这也是本文发布数据选择同步日志模式的直接原因。

## LT/ET 触发模式对比

为了评估 `trig_mode` 对轻读路径的影响，额外补做了一轮 4 种组合对比实验。

固定条件：

- 机器与部署方式不变：本机 `docker compose up -d`
- 固定配置：`TWS_LOG_WRITE=0`
- 压测命令：`wrk -t4 -d10s --latency`
- 场景：`GET /healthz`、`GET /api/private/ping`
- 并发：`200`、`500`

模式映射：

- `0`：`listenfd LT + connfd LT`
- `1`：`listenfd LT + connfd ET`
- `2`：`listenfd ET + connfd LT`
- `3`：`listenfd ET + connfd ET`

### 吞吐对比

| 模式 | `/healthz` `c=200` | `/healthz` `c=500` | `/api/private/ping` `c=200` | `/api/private/ping` `c=500` |
| --- | ---: | ---: | ---: | ---: |
| `0 LT/LT` | 9557.94 req/s | 9768.11 req/s | 9035.52 req/s | 7604.08 req/s |
| `1 LT/ET` | 10505.95 req/s | 9794.33 req/s | 9888.52 req/s | 10067.04 req/s |
| `2 ET/LT` | 10779.72 req/s | 9892.14 req/s | 9592.16 req/s | 10399.59 req/s |
| `3 ET/ET` | 11183.76 req/s | 9896.31 req/s | 9341.16 req/s | 10687.70 req/s |

### 延迟对比

`/api/private/ping` 在 `500` 并发下的延迟差异最有代表性：

| 模式 | Avg | P50 | P90 | P99 | Errors |
| --- | --- | --- | --- | --- | --- |
| `0 LT/LT` | 70.50ms | 57.89ms | 99.41ms | 417.95ms | `read 566, timeout 56` |
| `1 LT/ET` | 56.22ms | 45.50ms | 72.53ms | 412.17ms | `read 534, timeout 31` |
| `2 ET/LT` | 53.03ms | 44.49ms | 70.20ms | 239.96ms | `read 551, timeout 12` |
| `3 ET/ET` | 51.70ms | 42.26ms | 68.02ms | 255.84ms | `read 480, timeout 26` |

### 结论

- `LT/LT` 是这轮实验里最弱的组合，尤其在 `GET /api/private/ping` `c=500` 下吞吐最低、尾延迟最差。
- 只要把 `listenfd` 或 `connfd` 其中一侧切到 `ET`，轻读路径表现都会明显优于 `LT/LT`。
- `connfd` 使用 `ET` 的收益更稳定，说明连接级读写事件减少重复通知后，对当前实现帮助更大。
- 综合吞吐与延迟，`ET/ET` 仍是本项目当前最合适的默认配置。

## 结论

- `GET /healthz` 可以跑到 `5k~7k req/s` 量级，说明连接接入和基础响应路径是能打的。
- `GET /api/private/ping` 在带鉴权的情况下仍能接近 `6k~7.5k req/s`，说明 Bearer Token 校验链路本身不算重。
- `GET /api/drive/items` 明显受数据库影响，在 `500~1000` 并发区间已经出现稳定的 `read/timeout` 错误和较高 MySQL CPU。
- `POST /api/login` 与 `POST /api/drive/files/upload` 是当前最重的两条写路径，尤其上传接口在 `500` 并发下已经进入秒级延迟区。
- 如果继续优化，优先级应该是：
  1. 日志模块实现
  2. `files` 列表与登录链路的 MySQL 访问
  3. 上传路径的 multipart 解析和磁盘写入成本

## 原始结果位置

- 新 benchmark 结果默认生成到 `reports/benchmarks/<timestamp>/`，该目录作为运行产物不提交。
- 可信 headline：已整理进 [benchmark-trusted.csv](benchmark-trusted.csv)
- 1000 并发补充：本轮手工 `wrk` 输出，已整理进 [benchmark.csv](benchmark.csv)

## benchmark.csv 字段说明

`[benchmark.csv](benchmark.csv)` 用于沉淀结构化性能数据，字段含义如下：

| Field | Description |
| --- | --- |
| `endpoint` | 请求路径 |
| `method` | HTTP 方法 |
| `concurrency` | `wrk` 并发连接数 |
| `duration_seconds` | 压测持续时间，单位秒 |
| `threads` | `wrk` 工作线程数 |
| `request_size_bytes` | 请求体字节数；无请求体时为 `0` |
| `db_hit` | 是否命中数据库，`1` 表示会访问 MySQL |
| `https` | 是否启用 HTTPS，`1` 表示启用 |
| `requests_per_sec` | 吞吐，单位 `req/s` |
| `avg_latency` | 平均延迟 |
| `p50_latency` | P50 延迟 |
| `p90_latency` | P90 延迟 |
| `p99_latency` | P99 延迟 |
| `errors` | `wrk` 输出中的错误汇总 |
| `web_cpu_peak` | 压测期间 `web` 容器 CPU 峰值 |
| `mysql_cpu_peak` | 压测期间 `mysql` 容器 CPU 峰值 |
| `source` | 原始结果来源文件或手工采样说明 |

补充说明：

- `NA` 表示该轮未采集到对应容器 CPU 指标
- 延迟字段保持 `wrk` 原始单位，不额外做数值归一化
- `db_hit` 和 `https` 为便于筛选和聚合而保留的布尔型标记字段
