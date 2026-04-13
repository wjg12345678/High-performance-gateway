# Benchmark Report

这份文档记录当前仓库一套可复现、可讲述的正式压测结果，重点回答 5 个问题：

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
- 静态资源：首页 `GET /`
- 登录接口：`POST /api/login`
- 私有轻接口：`GET /api/private/ping`
- 文件列表：`GET /api/private/files`
- 小文件上传：`POST /api/private/files`

## 固定变量

- 线程数：`4`
- 时长：`10s`
- 统一使用 `wrk --latency`
- `login` 请求体：`48B` JSON
- `upload` 请求体：`101B` JSON + Base64
- 鉴权接口均使用 Bearer Token

`wrk --latency` 原生输出只给到 `50/75/90/99` 分位，所以这里统一记录 `Avg / P50 / P90 / P99`，不伪造 `P95`。

## 主体矩阵

### 1. 轻接口与静态资源

| 接口 | 并发 | Avg | P50 | P90 | P99 | Requests/sec | Errors |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| `/healthz` | 100 | 30.97ms | 13.35ms | 24.90ms | 710.15ms | 6898.92 | connect 0, read 0, write 0, timeout 4 |
| `/healthz` | 200 | 55.11ms | 28.14ms | 55.97ms | 848.46ms | 5016.87 | connect 0, read 1462, write 0, timeout 96 |
| `/healthz` | 500 | 101.19ms | 76.94ms | 139.49ms | 915.19ms | 4476.61 | connect 0, read 4059, write 8, timeout 214 |
| `/` | 100 | 26.04ms | 18.00ms | 30.56ms | 209.02ms | 5165.27 | none |
| `/` | 200 | 51.75ms | 42.55ms | 72.94ms | 167.11ms | 4299.95 | connect 0, read 40, write 0, timeout 0 |
| `/` | 500 | 112.57ms | 92.01ms | 147.78ms | 815.33ms | 4851.51 | connect 0, read 586, write 0, timeout 40 |

### 2. 鉴权与文件查询

| 接口 | 并发 | Avg | P50 | P90 | P99 | Requests/sec | Errors |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| `/api/private/ping` | 100 | 15.70ms | 14.27ms | 23.92ms | 60.52ms | 6654.09 | none |
| `/api/private/ping` | 200 | 50.88ms | 39.31ms | 73.16ms | 313.87ms | 4678.28 | connect 0, read 25, write 0, timeout 0 |
| `/api/private/ping` | 500 | 59.62ms | 50.43ms | 74.79ms | 596.27ms | 5077.29 | connect 0, read 8476, write 1345, timeout 292 |
| `/api/private/files` | 100 | 55.96ms | 37.93ms | 84.00ms | 573.09ms | 2196.76 | connect 0, read 0, write 0, timeout 3 |
| `/api/private/files` | 200 | 84.73ms | 59.10ms | 80.50ms | 892.15ms | 3103.96 | connect 0, read 37, write 0, timeout 13 |
| `/api/private/files` | 500 | 214.94ms | 211.71ms | 293.53ms | 597.87ms | 2173.21 | connect 0, read 731, write 0, timeout 60 |

### 3. 重业务写路径

| 接口 | 并发 | Avg | P50 | P90 | P99 | Requests/sec | Errors |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| `/api/login` | 100 | 117.69ms | 100.92ms | 159.00ms | 295.95ms | 875.10 | none |
| `/api/login` | 200 | 396.85ms | 371.96ms | 536.22ms | 785.85ms | 491.31 | connect 0, read 0, write 0, timeout 5 |
| `/api/login` | 500 | 1.08s | 1.09s | 1.43s | 1.84s | 426.50 | connect 0, read 337, write 2, timeout 31 |
| `/api/private/files` `POST` | 100 | 300.48ms | 272.29ms | 382.70ms | 697.62ms | 330.92 | none |
| `/api/private/files` `POST` | 200 | 497.55ms | 484.32ms | 582.59ms | 1.03s | 394.72 | connect 0, read 38, write 0, timeout 0 |
| `/api/private/files` `POST` | 500 | 1.41s | 1.45s | 1.65s | 1.84s | 320.38 | connect 0, read 586, write 0, timeout 74 |

## 1000 并发补充

这组数据只补最有代表性的读场景，用来看更高并发下的吞吐拐点和错误形态，不把登录/上传这种重写路径硬压到失真。

| 接口 | 并发 | Avg | P50 | P90 | P99 | Requests/sec | Errors |
| --- | ---: | --- | --- | --- | --- | ---: | --- |
| `/healthz` | 1000 | 132.18ms | 124.92ms | 157.00ms | 425.25ms | 7581.63 | connect 0, read 3577, write 0, timeout 6 |
| `/` | 1000 | 149.16ms | 139.69ms | 192.63ms | 313.45ms | 6610.76 | connect 0, read 3218, write 0, timeout 17 |
| `/api/private/ping` | 1000 | 133.55ms | 122.46ms | 173.25ms | 364.37ms | 7497.74 | connect 0, read 3489, write 0, timeout 1 |
| `/api/private/files` | 1000 | 285.23ms | 259.80ms | 410.50ms | 930.34ms | 3127.67 | connect 0, read 3145, write 40, timeout 179 |

## 资源占用观察

基于 [benchmark.csv](benchmark.csv) 里的容器采样结果，可以提炼出几个稳定结论：

- 轻接口主要吃 `web` 容器 CPU，MySQL 占用很低。
- `GET /api/private/files` 的 MySQL 峰值 CPU 多次接近或超过 `4~5` 个核的量级，是当前最明显的数据库热点。
- `POST /api/private/files` 的 `web` CPU 并不夸张，但延迟很高，说明瓶颈不只是 Web 线程本身，更像是鉴权、写库、Base64 解码和磁盘写入叠加造成的长链路开销。

## 前后对比实验

在 200 并发下对比两种日志模式：

| 接口 | 异步日志 `TWS_LOG_WRITE=1` | 同步日志 `TWS_LOG_WRITE=0` | 变化 |
| --- | --- | --- | --- |
| `/healthz` | 4110.59 req/s, P99 552.73ms | 6793.47 req/s, P99 417.65ms | 明显提升 |
| `/api/login` | 456.86 req/s, P99 797.20ms | 633.83 req/s, P99 611.20ms | 明显提升 |
| `/api/private/files` `GET` | 2045.00 req/s, P99 262.81ms | 2780.26 req/s, P99 180.96ms | 明显提升 |
| `/api/private/files` `POST` | 197.22 req/s, P99 1.64s | 291.28 req/s, P99 1.08s | 明显提升 |

结论：

- 当前实现里，异步日志并不是收益点，反而更像额外的锁竞争或队列开销来源。
- 这也是本文发布数据选择同步日志模式的直接原因。

## 结论

- `GET /healthz` 和首页静态资源可以跑到 `5k~7k req/s` 量级，说明连接接入、静态路由和基础响应路径是能打的。
- `GET /api/private/ping` 在带鉴权的情况下仍能接近 `6k~7.5k req/s`，说明 Bearer Token 校验链路本身不算重。
- `GET /api/private/files` 明显受数据库影响，在 `500~1000` 并发区间已经出现稳定的 `read/timeout` 错误和较高 MySQL CPU。
- `POST /api/login` 与 `POST /api/private/files` 是当前最重的两条写路径，尤其上传接口在 `500` 并发下已经进入秒级延迟区。
- 如果继续优化，优先级应该是：
  1. 日志模块实现
  2. `files` 列表与登录链路的 MySQL 访问
  3. 上传路径的 Base64 和磁盘写入成本

## 原始结果位置

- 主体矩阵：`reports/benchmarks/20260413_155116/`
- 同步/异步日志对比：
  - `reports/benchmarks/20260413_153624/`
  - `reports/benchmarks/20260413_154220/`
- 1000 并发补充：本轮手工 `wrk` 输出，已整理进 [benchmark.csv](benchmark.csv)
