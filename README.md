# WebServer

基于 C++ 的 Linux Web 服务器项目，重点覆盖了 Reactor 并发模型、HTTP/1.1 请求处理、静态文件传输、线程池、连接池、日志系统、配置化运行、守护进程和 HTTPS。

当前版本已经从经典的“半同步半反应堆”演进为 `Main-Reactor + Multi-SubReactor`，并补齐了 `ET`、`Keep-Alive`、超时管理、异步日志、OpenSSL TLS、中间件链、基础 API 路由等能力，适合放进 GitHub 作为 Linux 网络编程/C++ 服务端项目展示。

## 项目定位

- 展示 Linux 网络编程、`epoll`、Reactor、多线程协作
- 展示 HTTP 服务端基础设施能力，而不只是页面 demo
- 展示从课程型项目向工程化项目演进的思路

## 核心特性

- `Main-Reactor + Multi-SubReactor` 多反应堆模型
- `epoll ET` 边缘触发，支持一次性读满直到 `EAGAIN`
- HTTP/1.1 基础解析、长连接 `Keep-Alive`
- 路由分发：按“方法 + 路径”分派请求
- 中间件链：请求日志、统一鉴权、统一错误响应
- `sendfile` 零拷贝发送静态文件
- HTTPS 支持，集成 OpenSSL
- 动态线程池：支持扩容、优先级调度、空闲线程回收
- MySQL 连接池：连接复用、空闲检测、自动重连
- 最小堆定时器：管理空闲/超时连接
- 环形缓冲区 + 内存池：降低频繁分配和拷贝
- 异步日志、日志分级、日志滚动
- 配置文件驱动运行参数
- 守护进程模式、后台运行、信号处理、异常拉起

## 适合写进简历/GitHub 的点

- 将原始 TinyWebServer 从“半同步半反应堆”升级为主从 Reactor / Multi-Reactor
- 将 `LT` 改为 `ET`，补齐 ET 模式下的读写边界处理
- 引入 `sendfile`、最小堆定时器、动态线程池、异步日志
- 增加 HTTPS、配置文件、守护进程、中间件链和 API 路由
- 保留原始静态页面能力，同时补充接口化访问方式

## 架构概览

主要执行路径如下：

1. `main.cpp` 负责配置加载、环境变量覆盖、守护进程/信号处理、服务启动
2. 主 Reactor 负责监听 `listenfd` 并接收新连接
3. 新连接按轮询分发到多个 SubReactor
4. SubReactor 负责连接读写事件、TLS 握手推进和超时扫描
5. 业务解析与响应处理交给动态线程池协同完成
6. HTTP 明文静态文件走 `sendfile`
7. HTTPS 请求自动切换为 `SSL_read / SSL_write`

## 功能增强清单

- 从半同步半反应堆升级为主从 Reactor / Multi-Reactor
- 线程池优化：动态扩容、空闲线程回收
- `LT` 切换为 `ET`
- 增加 ET 模式下的一次性读满处理
- 修复 `EAGAIN` 处理不完整问题
- 支持长连接 `Keep-Alive`
- 增加最小堆定时器处理超时连接
- HTTP/1.1 基础解析增强
- MySQL 连接池复用、超时检测、自动重连
- 引入内存池与环形缓冲区
- 减少内存拷贝
- 日志系统异步化、日志分级、日志滚动
- 配置文件读取运行参数
- 增加守护进程模式、后台运行
- 完善异常处理、崩溃重启、信号处理
- 支持 HTTPS（OpenSSL）

## 目录结构

```text
.
├── CGImysql/          # MySQL 连接池
├── http/              # HTTP 连接、请求解析、响应发送
├── lock/              # 同步原语封装
├── log/               # 日志系统
├── memorypool/        # 内存池
├── root/              # 静态资源
├── threadpool/        # 动态线程池
├── timer/             # 超时管理
├── config.cpp
├── config.h
├── main.cpp
├── server.conf        # 运行配置
├── server_ctl.sh      # start/stop/restart/reload/status
├── webserver.cpp
└── webserver.h
```
```
┌─────────────────────────────────────────────────────────────────┐
│                         启动阶段                                │
│                                                                 │
│  main()                                                         │
│    ├─ Config::parse_arg()     解析命令行 -f conf -p port ...    │
│    │    └─ load_file()        读取 server.conf                  │
│    │                                                            │
│    ├─ daemon_mode?                                              │
│    │   ├─ Yes → run_daemon_supervisor()  守护进程 + 自动重启    │
│    │   │         └─ fork() → run_server_process()               │
│    │   └─ No  → run_server_process()     前台运行               │
│    │                                                            │
│    └─ run_server_process()                                      │
│         ├─ 环境变量覆盖 TWS_DB_*, TWS_AUTH_TOKEN                │
│         ├─ WebServer::init()       保存所有配置参数             │
│         ├─ server.log_write()      初始化日志(同步/异步)        │
│         ├─ server.tls_init()       加载SSL证书(可选)            │
│         ├─ server.sql_pool()       创建数据库连接池             │
│         │    └─ initmysql_result() 预加载user表到内存map        │
│         ├─ server.thread_pool()    创建线程池(动态伸缩)         │
│         ├─ server.trig_mode()      设置ET/LT触发模式            │
│         ├─ server.eventListen()    创建监听socket + epoll       │
│         │    └─ init_sub_reactors()  启动N个子Reactor线程       │
│         └─ server.eventLoop()      进入主事件循环 ──────────┐   │
└─────────────────────────────────────────────────────────────│───┘
                                                              │
┌─────────────────────────────────────────────────────────────▼───┐
│                      连接接入阶段                               │
│                                                                 │
│  主Reactor (eventLoop)                                          │
│    └─ epoll_wait() 只监听 listenfd                              │
│         └─ dealclientdata()                                     │
│              ├─ accept() 接受新连接                             │
│              ├─ 轮询选择子Reactor (round-robin)                 │
│              └─ SubReactor::dispatch(connfd)                    │
│                   └─ 写eventfd唤醒子Reactor                     │
│                                                                 │
│  子Reactor线程 (SubReactor::run)                                │
│    └─ handle_notify()                                           │
│         └─ register_connection()                                │
│              ├─ http_conn::init()  初始化连接对象               │
│              │    └─ SSL_new() (如果HTTPS)                      │
│              ├─ 注册到子Reactor的epoll                          │
│              └─ refresh_timer() 加入超时堆                      │
└─────────────────────────────────────────────────────────────────┘
                            │
                 ┌──────────▼──────────┐
                 │   子Reactor epoll    │
                 │   检测到 EPOLLIN     │
                 └──────────┬──────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                      请求处理阶段                               │
│                                                                 │
│  dealwithread(sockfd)                                           │
│    ├─ TLS握手 (如需要): SSL_accept()                           │
│    ├─ read_once()                                               │
│    │    ├─ ring_recv() ← recv() / SSL_read()                   │
│    │    └─ sync_read_buffer() → 数据进入 m_read_buf            │
│    └─ threadpool::append_p()  提交到线程池                     │
│                                                                 │
│  线程池工作线程                                                 │
│    └─ http_conn::process()                                      │
│         │                                                       │
│         ├─ process_read() ── HTTP解析状态机 ──┐                 │
│         │   ├─ parse_request_line()           │                 │
│         │   │    提取 METHOD, URL, VERSION     │                │
│         │   ├─ parse_headers()                │                 │
│         │   │    提取 Host, Content-Length,    │                 │
│         │   │    Content-Type, Authorization   │                │
│         │   └─ parse_content()                │                 │
│         │        读取请求体                    │                 │
│         │                                     ▼                 │
│         │                              do_request()             │
│         │                                     │                 │
│         │              ┌──────────────────────┤                 │
│         │              ▼                      ▼                 │
│         │     run_before_middlewares()   parse_post_body()      │
│         │       ├─ middleware_request_log()  解析表单/JSON      │
│         │       └─ middleware_auth()                            │
│         │            Bearer Token校验                           │
│         │              │                                        │
│         │              ▼                                        │
│         │        route_request() ── 路由匹配 ──┐                │
│         │         │                            │                │
│         │    ┌────┴────┐              ┌────────┴───────┐       │
│         │    │ API路由  │              │  静态文件路由   │       │
│         │    │ 精确匹配 │              │ handle_static  │       │
│         │    │ g_routes │              │   _route()     │       │
│         │    └────┬────┘              └────────┬───────┘       │
│         │         │                            │                │
│         │   ┌─────┴──────┐          resolve_static_path()      │
│         │   │ 登录/注册   │          open_static_file()         │
│         │   │ handle_auth │          open() + stat()            │
│         │   │ _request()  │                   │                 │
│         │   │ ↓           │                   │                 │
│         │   │ mysql_query  │                  │                 │
│         │   │ (转义防注入) │                  │                 │
│         │   └─────┬──────┘                   │                 │
│         │         │                           │                 │
│         │         ▼                           ▼                 │
│         │     run_after_middlewares()                           │
│         │       API结果 → JSON格式化                            │
│         │              │                                        │
│         │              ▼                                        │
│         ├─ process_write(HTTP_CODE) ── 生成响应 ──┐             │
│         │   ├─ add_status_line()   "HTTP/1.1 200 OK\r\n"      │
│         │   ├─ add_headers()       Content-Length, Type...     │
│         │   ├─ add_content()       响应体(错误页/JSON)          │
│         │   └─ 写入 m_write_ring 缓冲区                        │
│         │                                                       │
│         └─ 注册 EPOLLOUT → 等待可写事件                        │
└─────────────────────────────────────────────────────────────────┘
                            │
                 ┌──────────▼──────────┐
                 │   子Reactor epoll    │
                 │   检测到 EPOLLOUT    │
                 └──────────┬──────────┘
                            │
┌───────────────────────────▼─────────────────────────────────────┐
│                      响应发送阶段                               │
│                                                                 │
│  dealwithwrite(sockfd) → http_conn::write()                     │
│    ├─ ring_send() 发送响应头                                    │
│    │    └─ send() / SSL_write()                                 │
│    ├─ 发送文件体                                                │
│    │    ├─ HTTP:  sendfile() 零拷贝                             │
│    │    └─ HTTPS: read() + SSL_write()                          │
│    └─ 完成后:                                                   │
│         ├─ Keep-Alive → init() 重置, 注册EPOLLIN等待下个请求   │
│         └─ Close → close_conn() 关闭连接                       │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                      辅助模块                                   │
│                                                                 │
│  超时管理 (子Reactor内)                                         │
│    ├─ 最小堆管理连接超时 (默认15秒)                             │
│    ├─ 每次I/O后 refresh_timer() 刷新                           │
│    └─ scan_timeout() 清理过期连接                              │
│                                                                 │
│  线程池 (动态伸缩)                                              │
│    ├─ 核心线程: 8个, 常驻等待                                   │
│    ├─ 最大线程: 16个, 按需扩展                                  │
│    └─ 空闲线程: 超时30秒后自动回收                              │
│                                                                 │
│  数据库连接池                                                   │
│    ├─ RAII自动获取/归还连接                                     │
│    ├─ 空闲超时60秒自动ping保活                                  │
│    └─ 启动时预加载user表到内存                                  │
│                                                                 │
│  日志系统                                                       │
│    ├─ 同步/异步模式可选                                         │
│    ├─ 按日期+行数自动切割                                       │
│    └─ 级别过滤: DEBUG < INFO < WARN < ERROR                    │
└─────────────────────────────────────────────────────────────────┘
```

### 核心架构特点

- **主从 Reactor 模式**：主 Reactor 只负责 accept，N 个子 Reactor 各自管理一组连接的 I/O 和超时
- **Proactor/Reactor 可切换**：Proactor 模式下主线程完成 I/O，线程池只做业务处理；Reactor 模式下线程池也负责 I/O
- **零拷贝**：HTTP 模式下用 `sendfile()` 直接从文件发送到 socket；HTTPS 则需经过 SSL 加密中转
- **连接复用**：HTTP/1.1 Keep-Alive 支持，处理完一个请求后重置状态继续等待下一个
## 编译环境

- Linux
- `g++`
- `epoll`
- MySQL Client 开发库
- OpenSSL 开发库

Docker 构建环境已经在仓库内配置完成。

## 快速开始

### 1. 使用 Docker Compose

```bash
docker compose up --build
```

默认会启动：

- MySQL 8.0
- Web 服务

访问地址：

```text
http://127.0.0.1:9006/
```

推荐先验证：

```bash
curl -I http://127.0.0.1:9006/
```

### 2. 本地编译

确保本机已经安装：

- `g++`
- `libmysqlclient`
- `openssl`

然后执行：

```bash
make server
./server -f server.conf
```

## 技术栈

- 语言：C++
- 平台：Linux
- IO 多路复用：`epoll`
- 并发模型：Main-Reactor + Multi-SubReactor
- 数据库：MySQL
- TLS：OpenSSL
- 构建方式：`make` / Docker Compose

## 配置文件

项目使用 [server.conf](/Users/mac/Desktop/TinyWebServer-master/server.conf) 作为默认配置入口。

当前支持的核心配置项：

```ini
port=9006
log_write=1
log_level=1
log_split_lines=800000
log_queue_size=800
trig_mode=3
opt_linger=0
sql_num=8
thread_num=8
threadpool_max_threads=16
threadpool_idle_timeout=30
mysql_idle_timeout=60
conn_timeout=15
close_log=0
actor_model=0
daemon_mode=0
pid_file=./TinyWebServer.pid
https_enable=0
https_cert_file=./certs/server.crt
https_key_file=./certs/server.key
auth_token=tinywebserver-secret
db_host=127.0.0.1
db_port=3306
db_user=root
db_password=root
db_name=qgydb
```

### 配置说明

- `port`：监听端口
- `log_write`：日志模式，`0` 同步，`1` 异步
- `log_level`：日志级别
- `trig_mode`：触发模式，`3` 表示 `ET + ET`
- `sql_num`：MySQL 连接池大小
- `thread_num`：初始线程数
- `threadpool_max_threads`：线程池最大线程数
- `threadpool_idle_timeout`：线程空闲回收时间
- `mysql_idle_timeout`：MySQL 空闲连接检测时间
- `conn_timeout`：连接超时时间
- `actor_model`：并发模型选择
- `daemon_mode`：是否启用守护进程
- `pid_file`：守护进程 PID 文件
- `https_enable`：是否启用 HTTPS
- `https_cert_file`：证书路径
- `https_key_file`：私钥路径
- `auth_token`：API 鉴权 token

## 命令行参数

```bash
./server -f server.conf
./server -p 9006 -l 1 -m 3 -o 0 -s 8 -t 8 -c 0 -a 1 -d 1
```

支持的命令行参数：

- `-f`：配置文件路径
- `-p`：端口
- `-l`：日志写入模式
- `-m`：触发模式
- `-o`：`linger` 配置
- `-s`：数据库连接数量
- `-t`：线程数量
- `-c`：是否关闭日志
- `-a`：并发模型
- `-d`：是否启用守护进程

说明：数据库相关配置优先读取环境变量 `TWS_DB_*`。

## 守护进程与后台运行

启用方式：

```ini
daemon_mode=1
pid_file=./TinyWebServer.pid
```

控制脚本：

```bash
./server_ctl.sh start
./server_ctl.sh stop
./server_ctl.sh restart
./server_ctl.sh reload
./server_ctl.sh status
```

已支持：

- PID 文件写入与清理
- 防重复启动
- `SIGTERM / SIGINT / SIGHUP` 处理
- 守护模式下 worker 异常退出自动拉起

## HTTPS 使用说明

先准备证书：

```bash
mkdir -p certs
openssl req -x509 -nodes -newkey rsa:2048 \
  -keyout certs/server.key \
  -out certs/server.crt \
  -days 365 \
  -subj "/CN=localhost"
```

修改配置：

```ini
https_enable=1
https_cert_file=./certs/server.crt
https_key_file=./certs/server.key
```

启动后访问：

```bash
curl -k https://127.0.0.1:9006/
```

说明：

- HTTP 明文静态文件发送仍走 `sendfile`
- HTTPS 连接因 TLS 加密限制，自动回退到 `SSL_write` 分块发送

## 路由分发

当前版本已经把请求处理从散落的 `if/else` 判断整理为统一的“方法 + 路径”分发表，路由入口会先做请求体解析，再根据请求方法和 URL 分发到对应处理逻辑。

当前内置路由包括：

- `POST /api/login`
- `POST /api/register`
- `POST /api/echo`
- `GET /api/private/ping`
- `POST /2`
- `POST /3`
- `GET /0`
- `GET /1`
- `GET /5`
- `GET /6`
- `GET /7`

其中：

- `/2`、`/3` 兼容原始页面登录/注册逻辑
- `/api/*` 用于 JSON / 表单接口测试
- `/0`、`/1`、`/5`、`/6`、`/7` 是静态页面快捷入口

这套结构后续可以继续扩展为：

- 更多 REST 风格接口
- 方法级权限控制
- 中间件式鉴权/日志处理
- 路由参数提取

## 中间件链

当前已经引入基础中间件链，执行顺序为：

1. 请求日志中间件
2. 鉴权中间件
3. 路由处理
4. 统一异常/错误响应格式化

当前已实现：

- 请求日志：记录 `method + path + content_type + content_length`
- 统一鉴权：拦截 `/api/private/*`、`/api/admin/*`
- 统一异常响应：API 请求错误自动转为 JSON

### 鉴权规则

默认 token 在配置文件中定义：

```ini
auth_token=tinywebserver-secret
```

请求时通过 Header 传递：

```http
Authorization: Bearer tinywebserver-secret
```

### 受保护接口示例

未携带 token：

```bash
curl http://127.0.0.1:9006/api/private/ping
```

返回：

```json
{"code":401,"message":"unauthorized"}
```

携带 token：

```bash
curl http://127.0.0.1:9006/api/private/ping \
  -H "Authorization: Bearer tinywebserver-secret"
```

返回：

```json
{"code":0,"message":"pong"}
```

## API 示例

### 1. 登录接口

表单方式：

```bash
curl -X POST http://127.0.0.1:9006/api/login \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "username=test&passwd=123456"
```

JSON 方式：

```bash
curl -X POST http://127.0.0.1:9006/api/login \
  -H "Content-Type: application/json" \
  -d '{"username":"test","passwd":"123456"}'
```

成功响应示例：

```json
{"code":0,"message":"login success","target":"/welcome.html"}
```

### 2. 注册接口

```bash
curl -X POST http://127.0.0.1:9006/api/register \
  -H "Content-Type: application/json" \
  -d '{"username":"new_user","passwd":"123456"}'
```

成功响应示例：

```json
{"code":0,"message":"register success"}
```

### 3. 回显接口

```bash
curl -X POST http://127.0.0.1:9006/api/echo \
  -H "Content-Type: application/json" \
  -d '{"hello":"world"}'
```

响应示例：

```json
{"code":0,"content_type":"application/json","body":"{\"hello\":\"world\"}"}
```

## 性能优化与项目演进

这一版改造不是单点功能堆叠，而是围绕“并发能力、连接稳定性、数据传输效率、工程可运维性”四条主线推进。

### 1. 并发模型升级

原始实现以“半同步半反应堆”为主，结构更偏教学型。当前版本将连接接入与连接处理拆分为 `Main-Reactor + Multi-SubReactor`：

- 主 Reactor 专注 `accept`
- 多个 SubReactor 分担连接事件
- 业务逻辑由动态线程池异步处理

这样做的直接收益：

- 降低单 Reactor 在高并发连接下的事件分发压力
- 让连接接入、IO 处理、业务执行三类职责边界更清晰
- 更适合继续演进为多核场景下的高并发服务端模型

### 2. IO 模型优化

事件触发模式从 `LT` 切换为 `ET`，并补齐 ET 模式下的“一次性读满直到 `EAGAIN`”处理逻辑，同时修复原生 `EAGAIN` 处理不完整的问题。

这部分优化的意义在于：

- 减少重复事件通知带来的额外开销
- 避免 ET 模式下因为未读空缓冲区而导致连接假死
- 提升高并发下连接处理的稳定性

### 3. 连接生命周期治理

围绕长连接和超时连接，补充了两类关键能力：

- HTTP/1.1 `Keep-Alive`
- 基于最小堆的连接超时管理

当前实现中，每个 `SubReactor` 都维护一个独立的最小堆定时器：

- 新连接注册后加入最小堆
- 每次读写成功后刷新该连接的过期时间
- 连接关闭时从堆中删除
- `epoll_wait()` 的超时时间直接由堆顶节点决定
- 事件循环结束后扫描堆顶，批量关闭已经超时的连接

这套实现对应代码主要位于 `timer/heap_timer.*` 与 `webserver.cpp` 中的 `SubReactor::refresh_timer()`、`SubReactor::scan_timeout()`。

收益主要体现在：

- 减少频繁建连/断连的系统调用成本
- 更及时地回收空闲或异常连接
- 让服务在大量短连接和空闲连接混合场景下更稳定

### 4. 传输链路优化

静态文件在 HTTP 明文场景下使用 `sendfile`，避免用户态和内核态之间不必要的数据搬运；HTTPS 场景下则根据 TLS 加密要求回退为 `SSL_write` 分块发送。

这部分设计的价值是：

- 明文静态文件请求具备更高的传输效率
- HTTPS 场景保持兼容性和正确性
- 让“性能优化”和“协议正确性”两者不互相冲突

### 5. 线程池与资源池优化

线程池不再是固定规模，而是支持：

- 动态扩容
- 任务优先级调度
- 空闲线程回收

同时数据库侧补充了 MySQL 连接池复用、空闲检测、自动重连。

这类优化的核心收益：

- 在请求高峰时按需拉起更多执行资源
- 在低负载时回收空闲线程，避免长期空转
- 降低数据库连接反复创建带来的成本和失败概率

### 6. 内存与缓冲区优化

针对高频读写路径，引入了内存池和环形缓冲区，目标是减少频繁 `malloc/free` 和不必要的数据拷贝。

这部分适合在项目描述中强调为：

- 针对热点路径做内存分配优化
- 通过环形缓冲区提升收发数据处理效率
- 为后续更高吞吐场景预留扩展空间

### 7. 工程可运维性增强

除了网络和性能层面的改造，这个项目还补齐了实际运行所需的工程能力：

- 异步日志、日志分级、日志滚动
- 配置文件读取与环境变量覆盖
- 守护进程模式、PID 文件、控制脚本
- 信号处理、异常退出后的拉起机制
- HTTPS 证书接入能力

这意味着该项目已经不只是“能跑起来”的实验代码，而是具备了更完整的部署、管理和展示价值。

### 8. 适合简历的表述方式

如果你要把这个项目写进简历，可以直接提炼成下面这种风格：

- 基于 C++/Linux 实现 Web 服务器，并将原始 TinyWebServer 从半同步半反应堆升级为 `Main-Reactor + Multi-SubReactor`
- 基于 `epoll ET` 改造连接处理流程，补齐 `EAGAIN` 边界处理与 ET 模式读满机制，优化高并发下的事件处理效率
- 实现 `Keep-Alive`、最小堆超时管理、动态线程池、MySQL 连接池复用，提升连接稳定性与资源利用率
- 在 HTTP 场景下引入 `sendfile` 零拷贝传输，并补充 HTTPS、配置化运行、异步日志、守护进程和 API 路由能力

如果你要把它写进 GitHub 项目简介，更适合强调“从教学项目到工程化演进”的脉络，而不是只列功能点。

## MySQL 初始化

Docker Compose 已经内置初始化 SQL。

如果手动初始化，可参考：

```sql
CREATE DATABASE qgydb;
USE qgydb;

CREATE TABLE user(
    username CHAR(50) NULL,
    passwd CHAR(50) NULL
) ENGINE=InnoDB;

INSERT INTO user(username, passwd) VALUES('name', 'passwd');
```

## 页面资源

静态资源位于 [root/](/Users/mac/Desktop/TinyWebServer-master/root) 目录，包括：

- 登录
- 注册
- 图片展示
- 视频展示

## GitHub 提交建议

仓库已经补充 `.gitignore`，建议提交前确认以下内容不要进入版本库：

- `server`
- `ServerLog/`
- `*.pid`
- `certs/`
- MySQL 数据目录

## 后续可继续扩展的方向

- HTTP/1.1 更完整语义支持
- HTTPS 热加载证书
- HTTP/2
- 更细粒度的监控与指标
- 更严格的配置校验
- 单元测试与压测脚本整理

## License

本项目保留原始 TinyWebServer 学习项目的实践属性，适合用于：

- 网络编程学习
- Linux 服务器项目练手
- C++ 后端工程能力展示

如用于简历或公开展示，建议明确说明你在原始项目基础上的增强部分。
