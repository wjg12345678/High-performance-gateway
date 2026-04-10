# 架构图

这份文档用于面试和 README 展示，重点说明服务启动、Reactor 协作、线程池执行、数据库与文件模块的关系。

静态 SVG 版本：

![Architecture Overview](architecture-overview.svg)

## 整体架构

```mermaid
flowchart LR
    Client[Browser / API Client]
    Main[Main Reactor<br/>listenfd + epoll]
    Sub1[SubReactor 1<br/>conn epoll + timer heap]
    SubN[SubReactor N<br/>conn epoll + timer heap]
    Pool[Dynamic Thread Pool]
    HTTP[http_conn<br/>HTTP parse + route + middleware]
    MySQL[(MySQL<br/>user / user_sessions / files / operation_logs)]
    Files[(root/uploads)]
    Log[Async Log]

    Client --> Main
    Main --> Sub1
    Main --> SubN
    Sub1 --> Pool
    SubN --> Pool
    Pool --> HTTP
    HTTP --> MySQL
    HTTP --> Files
    HTTP --> Log
    Sub1 --> Client
    SubN --> Client
```

## 启动阶段

```mermaid
flowchart TD
    A[main.cpp] --> B[读取 server.conf / 环境变量]
    B --> C[WebServer::init]
    C --> D[log_write]
    D --> E[tls_init]
    E --> F[sql_pool]
    F --> G[thread_pool]
    G --> H[trig_mode]
    H --> I[eventListen]
    I --> J[init_sub_reactors]
    J --> K[eventLoop]
```

说明：

- `main.cpp` 负责组装所有配置并驱动服务启动
- `sql_pool()` 初始化 MySQL 连接池并预加载用户数据
- `thread_pool()` 创建动态线程池
- `eventListen()` 创建监听 socket、主 `epoll` 和 SubReactor

## Reactor 协作模型

```mermaid
flowchart LR
    L[listenfd]
    M[Main Reactor]
    R1[SubReactor]
    R2[SubReactor]
    EFD[eventfd notify]
    TQ[Task Queue]

    L --> M
    M -->|accept| M
    M -->|round-robin dispatch| EFD
    EFD --> R1
    EFD --> R2
    R1 -->|EPOLLIN/EPOLLOUT| TQ
    R2 -->|EPOLLIN/EPOLLOUT| TQ
```

说明：

- 主 Reactor 只关心 `listenfd`
- 新连接通过轮询分发到不同 SubReactor
- SubReactor 自己维护连接事件和超时堆
- 业务处理不在 Reactor 线程里执行，而是交给线程池

## 线程池与数据库

```mermaid
flowchart TD
    EP[SubReactor read/write event]
    TP[threadpool::append_p]
    W[Worker Thread]
    RAII[connectionRAII]
    HP[http_conn::process]
    DB[(MySQL)]

    EP --> TP
    TP --> W
    W --> RAII
    RAII --> DB
    W --> HP
    HP --> DB
```

说明：

- SubReactor 收到 `EPOLLIN` 后将连接对象投递到线程池
- 工作线程通过 `connectionRAII` 临时获取数据库连接
- `http_conn::process()` 内完成请求解析、鉴权、中间件、路由和响应组装

## 文件服务模块

```mermaid
flowchart TD
    Req[POST /api/private/files]
    Auth[middleware_auth]
    Parse[parse_post_body<br/>JSON / form / multipart]
    Store[写入 root/uploads]
    Meta[写入 files 表]
    Audit[写入 operation_logs]
    Resp[返回 file id / metadata]

    Req --> Auth
    Auth --> Parse
    Parse --> Store
    Store --> Meta
    Meta --> Audit
    Audit --> Resp
```

说明：

- 上传文件内容落盘到 `root/uploads`
- 文件元数据单独写入 `files`
- 上传、下载、删除、登录等行为写入 `operation_logs`

## 鉴权与会话

```mermaid
flowchart TD
    Login[POST /api/login]
    Hash[密码校验<br/>salt + hash]
    Session[生成 token]
    Persist[写入 user_sessions]
    Access[访问 /api/private/*]
    Lookup[lookup_session]
    Allow[通过]
    Deny[401]

    Login --> Hash
    Hash --> Session
    Session --> Persist
    Persist --> Access
    Access --> Lookup
    Lookup -->|token valid| Allow
    Lookup -->|token missing/expired| Deny
```

说明：

- 密码以带盐哈希形式存储
- token 会写入 `user_sessions`
- 私有接口统一通过 `middleware_auth()` 做 Bearer Token 校验

## 超时回收

```mermaid
flowchart LR
    IO[读写事件]
    Refresh[refresh_timer]
    Heap[HeapTimer]
    Tick[scan_timeout]
    Close[close_conn]

    IO --> Refresh
    Refresh --> Heap
    Heap --> Tick
    Tick -->|expired| Close
```

说明：

- 每次 I/O 后都会刷新连接活跃时间
- SubReactor 周期性扫描最小堆
- 过期连接主动关闭，避免长时间占用 fd 和线程池资源
