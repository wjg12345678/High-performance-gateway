# Atlas WebServer 完整学习路线

这份文档回答一个问题：**怎么把这个项目学到能面试、能讲源码、能解释设计取舍，而不是只会背 README。**

结论先放前面：不要一行行从 `main.cpp` 读到最后。这个项目应该按 **链路、问题、验证** 三条线学习。

```text
链路：一个请求从 socket 到 Controller / Service / Repository 再回到响应
问题：为什么这么设计，解决了什么并发、一致性、安全或工程问题
验证：对应的单测、smoke、race test、failure test 怎么证明它有效
```

最重要的优先级：

```text
1. epoll + 非阻塞 socket + 主从 Reactor + 线程池
2. HTTP 请求生命周期和分层
3. 文件上传、去重、ref_count、DB/磁盘一致性
4. MySQL 事务、行锁、唯一索引、trigger
5. Redis-Limiter 解耦限流
6. Docker、测试、benchmark、生产化边界
```

如果时间有限，先学透 **WebServer 底层** 和 **文件一致性**。这两块最能体现项目含金量。

## 0. 学习目标

学完这个项目，你应该能做到：

- 画出整体架构图。
- 讲清楚 MainReactor、SubReactor、线程池怎么配合。
- 讲清楚一个 HTTP 请求怎么从 socket 到业务层。
- 讲清楚登录、上传、下载、删除、分享的完整链路。
- 解释为什么要拆 `files` 和 `physical_files`。
- 解释 SHA-256 去重、`ref_count`、trigger、回收站之间的关系。
- 解释并发上传相同文件和并发 quota 竞争如何处理。
- 解释为什么 DB 和磁盘不能原子提交，以及怎么补偿。
- 解释 Atlas 和 Redis-Limiter 为什么拆开，Atlas 怎么接入限流组件。
- 能主动承认生产化边界，不把项目包装成生产级网盘。
- 能用脚本证明关键设计是可验证的。

## 1. 仓库和模块地图

### 1.1 后端仓库核心目录

```text
Atlas-WebServer/
|-- app/                  # main、配置、WebServer、MainReactor/SubReactor 编排
|-- http/
|   |-- core/             # 连接、HTTP parser、请求/响应模型、IO、运行时上下文
|   |-- router/           # 路由入口
|   |-- controllers/      # Auth / File / Operation Controller
|   `-- files/            # multipart、文件响应、文件名/路径辅助
|-- service/
|   |-- auth/             # 密码、登录、注册、session 业务
|   |-- files/            # 上传、文件、目录、分享业务
|   `-- rate_limit/       # Atlas 登录/注册限流适配
|-- repo/mysql/           # SQL 封装和结果映射
|-- infra/
|   |-- db/               # MySQL 连接池
|   |-- log/              # 日志
|   |-- storage/          # 本地存储辅助
|   |-- threadpool/       # 线程池
|   `-- timer/            # 连接超时定时器
|-- migrations/           # 数据库迁移
|-- scripts/              # smoke、专项测试、迁移、benchmark
|-- tests/                # C++ 单元测试
`-- docs/                 # 架构、API、面试、benchmark 文档
```

### 1.2 另一个项目 Redis-Limiter

```text
Redis-Limiter/
|-- include/redis_pool.hpp
|-- include/sliding_window_limiter.hpp
|-- src/redis_pool.cpp
|-- src/sliding_window_limiter.cpp
|-- src/python_binding.cpp
|-- examples/
|-- tests/
`-- README.md
```

两者关系：

```text
Redis-Limiter = 通用限流组件
Atlas = 网盘后端，只使用 Redis-Limiter 做登录/注册限流
```

Atlas 不应该复制限流组件源码，而是通过 CMake 链接外部 `redis_limiter::core`。这样两个项目可以分开写简历，也能体现组件解耦。

## 2. 第一阶段：先跑起来

### 2.1 为什么第一步不是读代码？

因为你不先跑起来，就不知道：

- 项目有哪些服务。
- API 怎么调用。
- 数据库长什么样。
- 文件上传后磁盘和 DB 怎么变化。
- 测试脚本在验证什么。

先跑起来，再带着现象看代码，效率最高。

### 2.2 Docker Compose 启动

从 Atlas 根目录启动：

```bash
cd /home/ubuntu/Atlas
docker compose up -d --build
docker compose ps
```

你要观察：

- `backend` 是否启动。
- `mysql` 是否 healthy。
- `redis` 是否 healthy。
- 前端是否能访问。
- 后端健康检查是否返回 200。

健康检查：

```bash
curl -i http://127.0.0.1:9006/healthz
```

### 2.3 本地构建

后端仓库内：

```bash
cd /home/ubuntu/Atlas/Atlas-WebServer
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target server atlas-unit-tests --parallel
ctest --test-dir build --output-on-failure
```

如果你安装了 `libhiredis-dev`，CMake 会尝试链接外部 Redis-Limiter。否则认证限流会被禁用或走本地配置边界，这一点要能解释清楚。

### 2.4 主链路手动复现

建议按下面顺序跑一次：

```text
register
login
private ping
upload file
list files
download file
delete file
trash list
restore
share
public download
permanent delete
```

对应文档：

- [quickstart-5min.md](quickstart-5min.md)
- [api.md](api.md)

你要学会观察三件事：

- HTTP 响应是什么。
- MySQL 表怎么变化。
- `webroot/uploads/` 文件怎么变化。

## 3. 第二阶段：整体架构

### 3.1 一句话架构

标准讲法：

> Atlas 底层是 C++17 实现的 Linux WebServer，使用非阻塞 socket、epoll、主从 Reactor 和线程池处理连接；HTTP 层解析请求并路由到 Controller；业务层按 Service / Repository 分层访问 MySQL 和本地文件系统；登录/注册限流通过外部 Redis-Limiter 组件接入 Redis。

### 3.2 请求总链路

```text
Client
  -> listenfd
  -> MainReactor accept
  -> 分发 connfd 到 SubReactor
  -> SubReactor epoll_wait
  -> HttpConnection::read_once
  -> threadpool
  -> HttpConnection::process
  -> HTTP parser
  -> Router
  -> Controller
  -> Service
  -> Repository
  -> MySQL / Storage / Redis-Limiter
  -> HttpResponse
  -> EPOLLOUT
  -> write response
```

### 3.3 先读哪些文件？

按这个顺序：

1. [README.md](../README.md)
2. [architecture.md](architecture.md)
3. [request-sequence.md](request-sequence.md)
4. [app/main.cpp](../app/main.cpp)
5. [app/webserver.cpp](../app/webserver.cpp)
6. [app/webserver_sub_reactor.cpp](../app/webserver_sub_reactor.cpp)
7. [http/core/connection.cpp](../http/core/connection.cpp)
8. [http/router/router.cpp](../http/router/router.cpp)

不要一开始就钻进 SQL 或工具函数。先把请求怎么流动搞清楚。

## 4. 第三阶段：服务启动流程

### 4.1 启动链路

```text
app/main.cpp
  -> 读取命令行参数
  -> Config::parse_arg / parse_file / env override
  -> configure_auth_rate_limiter
  -> WebServer::init
  -> log_write
  -> sql_pool
  -> thread_pool
  -> trig_mode
  -> eventListen
  -> init_sub_reactors
  -> eventLoop
```

重点文件：

- [app/main.cpp](../app/main.cpp)
- [app/config.cpp](../app/config.cpp)
- [app/webserver.cpp](../app/webserver.cpp)

### 4.2 配置来源

配置优先级：

```text
默认值
  -> server.conf
  -> 环境变量 TWS_*
  -> 命令行参数
```

重点配置：

| 配置 | 作用 |
| --- | --- |
| `TWS_PORT` | 后端监听端口 |
| `TWS_THREAD_NUM` | SubReactor / 基础工作线程数 |
| `TWS_THREADPOOL_MAX_THREADS` | 最大工作线程数 |
| `TWS_SQL_NUM` | MySQL 连接池大小 |
| `TWS_UPLOAD_MAX_BYTES` | 单文件大小限制 |
| `TWS_USER_STORAGE_QUOTA_BYTES` | 单用户容量限制 |
| `TWS_CONN_TIMEOUT` | 连接空闲超时 |
| `TWS_AUTH_RATE_LIMIT_ENABLED` | 是否启用认证限流 |
| `TWS_REDIS_HOST` | Redis 地址 |
| `TWS_AUTH_RATE_LIMIT_FALLBACK_MODE` | Redis 失败时 fallback 策略 |

### 4.3 面试会怎么问？

问题：服务启动时初始化了哪些核心资源？

回答：

> 启动时会读取配置，初始化日志、MySQL 连接池、线程池、限流配置、监听 socket、主 epoll 和 SubReactor。SubReactor 启动后各自维护连接 epoll 和超时堆，主 Reactor 只负责 accept 和分发新连接。

## 5. 第四阶段：WebServer 底层

### 5.1 你要掌握的关键词

必须能解释：

- socket
- bind / listen / accept
- 非阻塞 IO
- epoll
- LT / ET
- EPOLLONESHOT
- MainReactor
- SubReactor
- eventfd 唤醒
- thread pool
- read buffer / write buffer
- keep-alive
- timer / idle timeout

### 5.2 MainReactor 做什么？

MainReactor 负责：

- 创建 `listenfd`。
- 创建主 `epollfd`。
- 监听新连接事件。
- accept 新连接。
- 按轮询分发给 SubReactor。

它不负责每个连接后续的读写事件。

学习文件：

- [app/webserver.cpp](../app/webserver.cpp)

重点函数：

- `eventListen`
- `eventLoop`
- `dealclientdata`
- `dispatch_to_sub_reactor`

### 5.3 SubReactor 做什么？

SubReactor 负责：

- 持有自己的 `epollfd`。
- 通过 `eventfd` 被主 Reactor 唤醒。
- 注册新连接。
- 处理连接的 `EPOLLIN` / `EPOLLOUT`。
- 管理连接空闲超时。
- 把业务任务投递给线程池。

学习文件：

- [app/webserver_sub_reactor.cpp](../app/webserver_sub_reactor.cpp)

重点理解：

```text
MainReactor accept
  -> pending queue
  -> eventfd notify
  -> SubReactor drain_pending_connections
  -> epoll_ctl add connfd
```

### 5.4 为什么不用每连接一线程？

因为连接数大时，每连接一线程会带来：

- 大量线程栈内存。
- 上下文切换。
- 线程调度开销。
- 大量空闲连接浪费线程。

Reactor 模型让少量线程管理大量连接，只在事件就绪时处理。

### 5.5 epoll 为什么比 select/poll 更适合？

`select`：

- fd 数量受 `FD_SETSIZE` 限制。
- 每次都要复制 fd 集合。
- 返回后要线性扫描。

`poll`：

- 解决 fd 数量限制。
- 仍然要线性扫描。

`epoll`：

- fd 注册后由内核维护。
- 返回的是就绪事件。
- 适合大量连接少量活跃的场景。

### 5.6 ET 和 LT 怎么讲？

LT：

```text
只要 fd 还有数据没读完，epoll 会一直通知。
```

ET：

```text
只有状态从不可读变成可读时通知一次。
```

ET 的要求：

- socket 必须非阻塞。
- 读时循环读到 `EAGAIN`。
- 写时处理部分写，没写完继续等 EPOLLOUT。

面试回答：

> ET 通知次数更少，但实现更严格。如果读事件来了只读一次，缓冲区里剩下的数据可能不会再次触发事件，所以必须循环读到 `EAGAIN`。

### 5.7 EPOLLONESHOT 怎么讲？

作用：

```text
防止同一个连接同时被多个线程处理。
```

典型流程：

```text
EPOLLIN 触发
  -> 事件失效
  -> Reactor 读数据
  -> 线程池处理业务
  -> 处理完成后 modfd 重新注册 EPOLLIN/EPOLLOUT
```

如果没有它，同一个连接可能在业务线程还没处理完时再次被其他线程拿到。

### 5.8 连接超时怎么做？

每个连接维护活跃时间。SubReactor 定期扫描超时堆：

```text
连接有 IO
  -> refresh_timer
空闲超过 TWS_CONN_TIMEOUT
  -> close_conn
```

学习文件：

- [infra/timer/heap_timer.cpp](../infra/timer/heap_timer.cpp)
- [app/webserver_sub_reactor.cpp](../app/webserver_sub_reactor.cpp)

### 5.9 线程池怎么讲？

线程池负责执行业务处理，避免 Reactor 线程被阻塞。

学习文件：

- [infra/threadpool/threadpool.h](../infra/threadpool/threadpool.h)

你要能说：

> Reactor 线程负责事件分发，工作线程负责 HTTP process、路由、数据库和文件业务。线程池可以复用线程，避免频繁创建销毁，也能通过队列形成背压。

### 5.10 这块最常见面试问题

必须准备：

- 为什么用 epoll？
- ET 怎么保证不漏读？
- EPOLLONESHOT 解决什么问题？
- Reactor 和线程池怎么配合？
- 线程池满了怎么办？
- MySQL 慢查询会不会阻塞 Reactor？
- keep-alive 怎么处理？
- 连接超时怎么回收？

## 6. 第五阶段：HTTP 请求生命周期

### 6.1 总链路

```text
HttpConnection::read_once
  -> buffer append
  -> HttpConnection::process
  -> parse request line / headers / body
  -> build HttpRequest
  -> build RequestContext
  -> Router::handle
  -> Controller
  -> Service
  -> Repository
  -> HttpResponse
  -> write
```

重点文件：

- [http/core/connection.cpp](../http/core/connection.cpp)
- [http/core/parser.cpp](../http/core/parser.cpp)
- [http/core/http_message.cpp](../http/core/http_message.cpp)
- [http/core/response.cpp](../http/core/response.cpp)
- [http/router/router.cpp](../http/router/router.cpp)

### 6.2 为什么要拆 `HttpRequest` / `HttpResponse` / `RequestContext`？

不拆的问题：

- `HttpConnection` 同时负责 IO、解析、业务和 SQL。
- 业务难单测。
- 上传临时文件生命周期和业务逻辑混在一起。
- controller 不能脱离连接对象复用。

拆完后的边界：

| 对象 | 职责 |
| --- | --- |
| `HttpConnection` | socket IO、请求解析、响应写回、连接生命周期 |
| `HttpRequest` | method、path、headers、query、body、上传文件信息 |
| `RequestContext` | 当前用户、MySQL 连接、doc root、配额、客户端 IP |
| `HttpResponse` | status、headers、body、文件响应 |
| `Router` | 路由匹配 |
| `Controller` | 参数校验和 HTTP 适配 |
| `Service` | 业务规则和事务 |
| `Repository` | SQL |

### 6.3 HTTP parser 要学什么？

你不需要背每个字符状态机，但要理解：

- 请求行：method、path、version。
- header：大小写、冒号、空格、结束 CRLF。
- body：按 `Content-Length` 或 chunked 读取。
- query：`?a=1&b=2`。
- form：`application/x-www-form-urlencoded`。
- JSON：controller 层解析。
- multipart：文件上传。

### 6.4 chunked 为什么重要？

因为 chunked 没有固定 `Content-Length`，body 被拆成多个 chunk：

```text
<hex-size>\r\n
<data>\r\n
...
0\r\n
\r\n
```

面试可以说：

> 不能假设请求体一次 read 完。chunked 要按 chunk size 逐段解析，直到 0 长度 chunk。项目里有 `parser_chunked_test` 和脚本验证真实 chunked 请求。

### 6.5 multipart 上传为什么复杂？

multipart 要处理：

- boundary。
- 每个 part 的 header。
- 文件字段。
- 普通 form 字段。
- 大文件流式写临时文件。
- 请求过大时提前拒绝。
- 异常时清理临时文件。

重点文件：

- [http/files/multipart_parser.cpp](../http/files/multipart_parser.cpp)
- [http/core/connection.cpp](../http/core/connection.cpp)

### 6.6 路由和 Controller 怎么学？

从 API 入口读：

- [http/router/router.cpp](../http/router/router.cpp)
- [http/controllers/auth_controller.cpp](../http/controllers/auth_controller.cpp)
- [http/controllers/file_controller.cpp](../http/controllers/file_controller.cpp)
- [http/controllers/operation_controller.cpp](../http/controllers/operation_controller.cpp)

学习方法：

```text
找一个接口
  -> 看 router 怎么匹配
  -> 看 controller 怎么取参数
  -> 看 controller 调哪个 service
  -> 看 service 做了什么事务
  -> 看 repository 执行什么 SQL
```

不要从 controller 第一行读到最后。按接口读。

## 7. 第六阶段：认证和会话

### 7.1 登录链路

```text
POST /api/login
  -> Router
  -> AuthController
  -> check_auth_rate_limit
  -> AuthService
  -> UserRepository
  -> PBKDF2 校验密码
  -> make_session_token
  -> SessionRepository 写 user_sessions
  -> 返回 token
```

重点文件：

- [http/controllers/auth_controller.cpp](../http/controllers/auth_controller.cpp)
- [service/auth/auth_service.cpp](../service/auth/auth_service.cpp)
- [repo/mysql/user_repository.cpp](../repo/mysql/user_repository.cpp)
- [repo/mysql/session_repository.cpp](../repo/mysql/session_repository.cpp)

### 7.2 密码为什么不能明文？

项目使用 PBKDF2-HMAC-SHA256：

- 随机 salt。
- 多轮迭代。
- 存储 hash，不存明文。

面试回答：

> 简单 SHA256 太快，泄露后容易被暴力枚举。PBKDF2 通过迭代增加破解成本，salt 防止相同密码产生相同 hash，也能抵抗彩虹表。

### 7.3 token 怎么讲？

登录成功生成随机 token，写入 `user_sessions`。客户端后续带：

```text
Authorization: Bearer <token>
```

服务端通过 token 查 session：

- token 存在。
- 未过期。
- 用户未禁用。
- 需要时刷新过期时间。

### 7.4 登出怎么做？

两类：

- 登出当前会话：删除当前 token。
- 登出全部会话：删除用户所有 session。

这比只让前端删除 token 更可靠，因为服务端 session 会失效。

### 7.5 学习认证模块的重点

不是背 SQL，而是理解：

- 注册和登录分别校验什么。
- 密码存储为什么这样设计。
- session token 为什么必须随机。
- Bearer Token 在哪里解析。
- 私有 API 怎么统一鉴权。
- 登录失败和限流怎么返回。

## 8. 第七阶段：文件模型和数据库

### 8.1 核心表

| 表 | 作用 |
| --- | --- |
| `users` | 用户账号、密码哈希、状态 |
| `user_sessions` | 登录 token 和过期时间 |
| `folders` | 用户目录 |
| `files` | 用户视角的逻辑文件 |
| `physical_files` | 磁盘真实文件对象 |
| `file_shares` | 分享 token、访问码、过期、下载次数 |
| `operation_logs` | 操作审计 |
| `schema_migrations` | 迁移版本 |

重点读：

- [migrations/001_init_schema.sql](../migrations/001_init_schema.sql)
- [migrations/005_normalize_schema.sql](../migrations/005_normalize_schema.sql)
- [migrations/006_ref_count_triggers.sql](../migrations/006_ref_count_triggers.sql)

### 8.2 为什么拆 `files` 和 `physical_files`？

用户看到的文件和磁盘真实对象不是同一个概念。

`files` 表：

- 用户 ID。
- 文件名。
- 目录 ID。
- 是否删除。
- 是否公开。
- 逻辑元数据。

`physical_files` 表：

- SHA-256。
- 磁盘存储名。
- 文件大小。
- 引用计数。

这样做的价值：

- 同内容文件可以去重。
- 一个物理文件可以被多个逻辑文件引用。
- 回收站文件仍然可以保留引用。
- 永久删除时只有 `ref_count=0` 才删除物理对象。

### 8.3 `ref_count` 为什么由 trigger 维护？

应用层手动维护会出问题：

```text
insert files 成功
increment ref_count 失败
  -> ref_count 偏小

delete files 成功
decrement ref_count 失败
  -> ref_count 偏大
```

trigger 的好处：

- `files` insert/delete 和 `ref_count` 更新在同一个 DB 事务中。
- 引用变化跟逻辑文件事实绑定。
- 减少异常路径漏处理。

标准回答：

> 我把 `files` 作为事实来源，`physical_files.ref_count` 只是派生数据，由 trigger 根据 `files` 的 insert/delete 自动维护。

### 8.4 回收站和 `ref_count`

普通删除：

```text
files.deleted_at = now()
```

这时：

- `files` 记录仍存在。
- 文件仍占用户容量。
- `ref_count` 不减少。
- 磁盘文件不删除。

永久删除：

```text
DELETE FROM files
trigger decrement ref_count
if ref_count == 0:
  delete physical_files
  delete disk file after commit
```

### 8.5 数据库迁移怎么讲？

不要说“启动时自动建表”作为重点。更好的说法：

> 项目使用版本化 SQL 迁移，`schema_migrations` 记录已执行版本。这样 schema 演进可追踪，也避免应用启动时临时拼接 `ALTER TABLE`。

重点文件：

- [scripts/migrate_db.sh](../scripts/migrate_db.sh)
- [migrations/](../migrations)

## 9. 第八阶段：上传链路

### 9.1 上传完整链路

```text
POST /api/drive/files/upload
  -> Bearer Token 鉴权
  -> multipart parser 找到 file part
  -> 写入 webroot/uploads/.tmp
  -> 计算文件大小和 SHA-256
  -> FileController
  -> UploadService
  -> MySQL transaction begin
  -> SELECT user FOR UPDATE
  -> 检查 quota
  -> 检查目录
  -> 检查同名冲突
  -> 查询 physical_files by sha256
  -> 不存在则插入 physical_files
  -> 插入 files 逻辑记录
  -> commit
  -> 临时文件转正式文件或去重命中删除临时文件
  -> 返回文件元数据
```

重点文件：

- [http/controllers/file_controller.cpp](../http/controllers/file_controller.cpp)
- [service/files/upload_service.cpp](../service/files/upload_service.cpp)
- [repo/mysql/file_repository.cpp](../repo/mysql/file_repository.cpp)
- [http/files/file_store.cpp](../http/files/file_store.cpp)
- [infra/storage/storage.cpp](../infra/storage/storage.cpp)

### 9.2 为什么先落临时文件？

原因：

- 大文件不能常驻内存。
- 计算 SHA-256 需要完整内容。
- 事务失败时临时文件可以删除。
- 去重命中时不需要把临时文件转正式文件。

### 9.3 preflight 有什么用？

preflight 用于上传前检查：

- 文件名是否合法。
- 目录是否存在。
- 是否超单文件大小。
- 是否可能超用户容量。
- 是否有同名冲突。

但它不是安全边界。真正上传时服务端必须再检查一遍，因为用户可以绕过前端或 preflight。

### 9.4 并发 quota 怎么防穿透？

问题：

```text
quota = 100B
当前已用 = 0
10 个请求并发上传 30B
如果都同时读到 0，每个都认为可以上传
最终可能 300B
```

解决：

```text
上传事务内 SELECT user FOR UPDATE
同一个用户上传串行检查 quota
每次在锁内重新计算已用容量
```

验证：

- [scripts/test_upload_quota_race.sh](../scripts/test_upload_quota_race.sh)

面试回答：

> 配额检查不能只靠普通 SELECT。项目在上传事务里锁用户行，让同一用户的 quota 检查串行化，防止并发请求同时看到相同剩余额度。

### 9.5 并发同内容上传怎么去重？

风险：

```text
两个请求同时上传相同内容
都查询 physical_files 不存在
都尝试插入
```

解决：

- `physical_files.sha256` 唯一索引。
- 事务内查询物理文件。
- 插入冲突时回查已有记录。
- 逻辑文件插入后 trigger 增加 `ref_count`。

验证：

- [scripts/test_upload_race_consistency.sh](../scripts/test_upload_race_consistency.sh)

### 9.6 上传失败怎么处理？

失败可能发生在：

- multipart 解析失败。
- 文件太大。
- 临时文件写失败。
- SHA-256 计算失败。
- DB 事务失败。
- 物理文件转正式文件失败。
- commit 前 failpoint。

原则：

- commit 前失败：DB rollback，清理临时文件或新物理文件。
- commit 后失败：不能回滚 DB，需要补偿。

验证：

- [scripts/test_upload_failure_cleanup.sh](../scripts/test_upload_failure_cleanup.sh)

## 10. 第九阶段：下载、删除、回收站、分享

### 10.1 下载链路

```text
GET /api/drive/files/:id/download
  -> 鉴权
  -> 查询 files
  -> 校验 owner / deleted_at / visibility
  -> 查询 physical_files
  -> 定位 webroot/uploads/<stored_name>
  -> 生成文件响应
  -> 写 operation_logs
```

重点：

- 私有文件必须鉴权。
- 回收站文件不能普通下载。
- 物理文件不存在要返回错误。
- 当前还没有完整 Range 下载。

### 10.2 普通删除

普通删除是软删除：

```text
UPDATE files SET deleted_at = now()
```

结果：

- 文件进入回收站。
- `ref_count` 不变。
- 用户容量仍占用。
- 磁盘文件仍存在。

### 10.3 恢复

恢复要检查：

- 文件属于当前用户。
- 文件在回收站。
- 原目录是否存在。
- 是否有同名冲突。

如果原目录不存在，可以恢复到根目录或处理冲突名。

### 10.4 永久删除

永久删除链路：

```text
DELETE /api/drive/files/:id/permanent
  -> 鉴权
  -> SELECT file FOR UPDATE
  -> 校验 owner 和 deleted_at
  -> DELETE files
  -> trigger decrement ref_count
  -> 如果无引用，删除 physical_files
  -> commit
  -> commit 后删除磁盘文件
```

为什么 commit 后删磁盘？

> 如果先删磁盘再 commit，commit 失败会导致 DB 仍引用一个已经不存在的文件。先 commit DB 再删磁盘，最坏是留下 orphan 文件，可以由 checker 补偿。

### 10.5 分享链路

分享创建：

```text
POST /api/drive/files/:id/share
  -> 鉴权
  -> 校验文件属于用户
  -> 生成 share token
  -> 可选访问码 hash
  -> 设置过期时间 / 最大下载次数
  -> 写 file_shares
```

分享访问：

```text
GET /api/share/:token
  -> 校验 token
  -> 校验过期
  -> 校验访问码
  -> 返回文件元信息
```

分享下载：

```text
GET /api/share/:token/download
  -> 校验 token / code / expire / count
  -> 增加下载次数
  -> 返回文件响应
```

并发下载次数要通过事务和锁保护，否则最后一次下载可能被多个请求同时消耗。

## 11. 第十阶段：DB 和磁盘一致性

### 11.1 为什么 DB 和磁盘不能原子提交？

MySQL 事务只能管理数据库内部操作，不能把本地文件系统操作纳入同一个事务。

典型不一致：

```text
DB commit 成功，磁盘删除失败
  -> orphan file

磁盘删除成功，DB commit 失败
  -> DB 引用缺失文件，更严重
```

项目选择：

```text
DB 内部强一致
DB/磁盘边界用顺序约束 + 巡检补偿
```

### 11.2 storage checker 做什么？

脚本：

- [scripts/check_storage_consistency.sh](../scripts/check_storage_consistency.sh)

检查：

- DB 引用的磁盘文件是否存在。
- 磁盘是否有 DB 不引用的 orphan 文件。
- `ref_count` 是否和 `files` 统计一致。
- 临时目录是否有残留。

### 11.3 面试高频答法

问题：DB 成功但磁盘删失败怎么办？

回答：

> 这会留下 orphan 文件，但用户视角已经正确，因为 DB 没有引用了。项目通过 storage checker 巡检补偿。生产上我会把同步删除改成 outbox + cleanup worker，支持重试和告警。

问题：为什么不先删磁盘？

回答：

> 先删磁盘再 commit，如果 commit 失败，就会出现 DB 引用一个不存在的文件，用户下载会直接失败。先 commit 再删磁盘，最坏只是孤儿文件，更容易补偿。

## 12. 第十一阶段：Redis-Limiter

### 12.1 Atlas 怎么接入 Redis-Limiter？

Atlas 的限流适配层：

- [service/rate_limit/auth_rate_limiter.cpp](../service/rate_limit/auth_rate_limiter.cpp)
- [service/rate_limit/auth_rate_limiter.h](../service/rate_limit/auth_rate_limiter.h)

Redis-Limiter 项目：

- [/home/ubuntu/Redis-Limiter/README.md](/home/ubuntu/Redis-Limiter/README.md)
- [/home/ubuntu/Redis-Limiter/include/sliding_window_limiter.hpp](/home/ubuntu/Redis-Limiter/include/sliding_window_limiter.hpp)
- [/home/ubuntu/Redis-Limiter/src/sliding_window_limiter.cpp](/home/ubuntu/Redis-Limiter/src/sliding_window_limiter.cpp)

关系：

```text
Atlas service/rate_limit
  -> 生成 login_ip / login_user / register_ip key
  -> 读取 Atlas 配置
  -> 调 Redis-Limiter core
  -> 返回 AuthRateLimitDecision
  -> Controller 转 HTTP 429
```

### 12.2 为什么要拆成独立项目？

原因：

- 限流是通用能力，不只 Atlas 能用。
- Redis-Limiter 可以独立提供 C++ core 和 Python binding。
- Atlas 只保留业务适配，边界更清楚。
- 简历上可以作为两个项目分别讲。

### 12.3 Redis Lua 为什么必要？

如果限流用多条普通 Redis 命令：

```text
GET current
判断
SET current
```

并发下会超发。

Lua 把以下操作放进同一个原子脚本：

```text
读取状态
计算补充令牌
判断是否允许
扣减令牌
设置 TTL
返回 allowed / remaining / retry_after
```

Redis 单线程执行脚本，同一 key 不会被并发插入打断。

### 12.4 令牌桶怎么讲？

令牌桶参数：

```text
max_tokens = 桶容量
refill_rate = 每秒补充令牌数
tokens_needed = 本次请求消耗几个令牌
```

请求到来：

```text
根据 Redis TIME 计算 elapsed
tokens = min(capacity, tokens + elapsed * refill_rate)
if tokens >= cost:
  tokens -= cost
  allowed = true
else:
  allowed = false
  retry_after = 缺少令牌 / refill_rate
```

适合登录、注册、短信验证码这种“平均速率 + 允许少量突发”的场景。

### 12.5 滑动窗口怎么讲？

滑动窗口用 Redis ZSET：

```text
ZREMRANGEBYSCORE 删除窗口外请求
ZCARD 统计窗口内请求数
如果 current + cost <= limit:
  ZADD 当前请求
  PEXPIRE
```

适合“任意 N 秒最多 M 次”的严格语义，但高 QPS 长窗口下 ZSET 成本更高。

### 12.6 Redis 挂了怎么办？

三种策略：

| 策略 | 行为 | 适合 |
| --- | --- | --- |
| LocalTokenBucket | Redis 挂了走本地令牌桶 | 默认推荐 |
| FailOpen | Redis 挂了直接放行 | 可用性优先 |
| FailClosed | Redis 挂了直接拒绝 | 安全优先 |

边界：

> 本地 fallback 只能保护单实例，Redis 故障期间不能保证多实例全局配额。

### 12.7 allow 是否幂等？

不是。`allow()` 会消耗额度。

如果业务重试同一个请求，会重复扣令牌。真正需要幂等时，应由业务层引入 request id、幂等表或去重 key。

## 13. 第十二阶段：测试体系

### 13.1 CTest 单元测试

`CMakeLists.txt` 注册的测试：

```text
parser_chunked
http_message
file_helpers
log_smoke
multipart_parser
upload_service
file_service
share_service
auth_rate_limiter
```

运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target atlas-unit-tests --parallel
ctest --test-dir build --output-on-failure
```

### 13.2 smoke 测试

入口：

- [scripts/run_smoke_suite.sh](../scripts/run_smoke_suite.sh)

覆盖：

- auth。
- private API。
- files。
- drive。
- ref-count。
- upload-race。
- storage-consistency。
- share。
- share-race。
- chunked-api。

运行：

```bash
docker compose exec -T backend ./scripts/run_smoke_suite.sh
```

### 13.3 专项测试

| 脚本 | 证明什么 |
| --- | --- |
| `test_auth_rate_limit.sh` | 登录/注册限流 |
| `test_ref_count_consistency.sh` | 去重和 ref_count |
| `test_upload_race_consistency.sh` | 并发同内容上传 |
| `test_upload_quota_race.sh` | 并发 quota 不穿透 |
| `test_upload_failure_cleanup.sh` | 提交前失败无脏数据 |
| `test_share_race.sh` | 分享下载并发 |
| `check_storage_consistency.sh --dry-run` | DB/磁盘一致性 |

### 13.4 为什么测试是面试重点？

因为它证明你不是只写成功路径。

你要能说：

> 文件系统项目最容易在异常和并发路径出问题，所以我补了 race、quota、failure cleanup 和 storage checker。面试时我不只讲设计，还能说用哪个脚本验证。

## 14. 第十三阶段：benchmark 和性能

### 14.1 benchmark 看什么？

不要只看 QPS。

要看：

- `requests/sec`
- 平均延迟
- P95 / P99
- 非 2xx/3xx 响应
- socket error
- backend 是否重启
- MySQL 是否重启
- 是否 SIGSEGV
- CPU / memory 峰值

入口：

- [scripts/run_benchmark_suite.sh](../scripts/run_benchmark_suite.sh)
- [docs/benchmark.md](benchmark.md)
- [docs/perf-flamegraph.md](perf-flamegraph.md)

### 14.2 为什么不能乱宣传压测数字？

因为结果受很多因素影响：

- 机器配置。
- Docker 环境。
- MySQL/Redis 延迟。
- 连接数。
- 数据库数据量。
- 是否命中业务路径。
- 是否出现错误响应。

正确讲法：

> benchmark 用于定位瓶颈和做回归比较，不直接当生产容量承诺。项目里加了 gate，避免 QPS 很高但大量错误响应的结果被误认为有效。

## 15. 第十四阶段：生产化边界

### 15.1 当前不能说生产级的原因

必须主动承认：

- 本地磁盘不适合多实例。
- DB 和磁盘无法原子提交。
- Redis 默认单点，生产要 Sentinel / Cluster。
- MySQL 缺少完整高可用和备份恢复演练。
- 没有对象存储。
- 没有断点续传、分片上传、Range 下载。
- 没有文件预览、协作权限。
- 清理任务还不是后台异步任务平台。
- 监控告警还不完整。

### 15.2 多实例怎么改？

核心是让后端无状态：

```text
本地磁盘 -> 对象存储 S3/MinIO/OSS
同步删除 -> outbox + cleanup worker
session -> MySQL/Redis
限流 -> Redis-Limiter + Redis HA
日志 -> centralized logging
```

### 15.3 对象存储怎么接？

抽象 Storage 接口：

```text
put(temp_file) -> object_key
get(object_key) -> stream
delete(object_key)
exists(object_key)
```

实现：

- LocalStorage。
- MinIOStorage。
- S3Storage。
- OSSStorage。

DB 保存 object key，而不是本地文件名。

### 15.4 异步清理怎么做？

引入 outbox 表：

```text
cleanup_tasks(
  id,
  object_key,
  task_type,
  status,
  retry_count,
  next_run_at,
  last_error
)
```

DB 事务内写 cleanup task，后台 worker 删除对象。失败重试，超过次数告警。

### 15.5 分片上传怎么做？

新增：

- `upload_sessions`
- `upload_parts`

流程：

```text
create upload session
upload part
record part number / size / hash
complete upload
merge parts
calculate final sha256
insert physical_files / files
cleanup parts
```

关键问题：

- part 重传幂等。
- session 过期清理。
- complete 并发控制。
- final hash 校验。

### 15.6 Range 下载怎么做？

解析：

```text
Range: bytes=start-end
```

返回：

```text
206 Partial Content
Content-Range: bytes start-end/total
```

需要支持：

- 断点续传。
- 视频预览。
- 大文件下载。
- 非法 Range 返回 416。

## 16. 学习顺序安排

### 第 1 天：跑项目和看文档

目标：

- 知道项目是什么。
- 能跑主链路。
- 能看懂 README 架构。

看：

- [README.md](../README.md)
- [quickstart-5min.md](quickstart-5min.md)
- [api.md](api.md)
- [architecture.md](architecture.md)

做：

```bash
cd /home/ubuntu/Atlas
docker compose up -d --build
docker compose exec -T backend ./scripts/run_smoke_suite.sh
```

### 第 2 天：WebServer 底层

目标：

- 画出 MainReactor / SubReactor / ThreadPool。
- 能解释 epoll、ET、EPOLLONESHOT。

看：

- [app/webserver.cpp](../app/webserver.cpp)
- [app/webserver_sub_reactor.cpp](../app/webserver_sub_reactor.cpp)
- [infra/threadpool/threadpool.h](../infra/threadpool/threadpool.h)
- [infra/timer/heap_timer.cpp](../infra/timer/heap_timer.cpp)

### 第 3 天：HTTP 链路

目标：

- 能讲登录请求从 socket 到 JSON 响应。
- 能讲 chunked 和 multipart。

看：

- [http/core/connection.cpp](../http/core/connection.cpp)
- [http/core/parser.cpp](../http/core/parser.cpp)
- [http/router/router.cpp](../http/router/router.cpp)
- [http/controllers/auth_controller.cpp](../http/controllers/auth_controller.cpp)

### 第 4 天：认证和 session

目标：

- 能讲 PBKDF2、token、Bearer、session 表。
- 能讲登录/注册限流入口。

看：

- [service/auth/auth_service.cpp](../service/auth/auth_service.cpp)
- [repo/mysql/user_repository.cpp](../repo/mysql/user_repository.cpp)
- [repo/mysql/session_repository.cpp](../repo/mysql/session_repository.cpp)
- [service/rate_limit/auth_rate_limiter.cpp](../service/rate_limit/auth_rate_limiter.cpp)

### 第 5 天：上传和去重

目标：

- 能讲上传完整链路。
- 能讲 SHA-256 去重。
- 能讲 quota 并发控制。

看：

- [service/files/upload_service.cpp](../service/files/upload_service.cpp)
- [repo/mysql/file_repository.cpp](../repo/mysql/file_repository.cpp)
- [http/files/multipart_parser.cpp](../http/files/multipart_parser.cpp)

跑：

```bash
docker compose exec -T backend ./scripts/test_upload_race_consistency.sh
docker compose exec -T backend ./scripts/test_upload_quota_race.sh
```

### 第 6 天：删除、回收站、分享、一致性

目标：

- 能讲普通删除、恢复、永久删除。
- 能讲 DB/磁盘一致性边界。
- 能讲分享下载次数竞争。

看：

- [service/files/file_service.cpp](../service/files/file_service.cpp)
- [service/files/share_service.cpp](../service/files/share_service.cpp)
- [scripts/check_storage_consistency.sh](../scripts/check_storage_consistency.sh)

跑：

```bash
docker compose exec -T backend ./scripts/test_ref_count_consistency.sh
docker compose exec -T backend ./scripts/test_share_race.sh
docker compose exec -T backend ./scripts/check_storage_consistency.sh --dry-run
```

### 第 7 天：Redis-Limiter

目标：

- 能讲 Atlas 和 Redis-Limiter 的边界。
- 能讲 Redis Lua 原子性。
- 能讲 fallback。

看：

- [/home/ubuntu/Redis-Limiter/README.md](/home/ubuntu/Redis-Limiter/README.md)
- [/home/ubuntu/Redis-Limiter/src/sliding_window_limiter.cpp](/home/ubuntu/Redis-Limiter/src/sliding_window_limiter.cpp)
- [service/rate_limit/auth_rate_limiter.cpp](../service/rate_limit/auth_rate_limiter.cpp)

### 第 8 天：面试串讲和查漏

目标：

- 背 1 分钟介绍。
- 背 5 个高频问题。
- 能主动讲生产化边界。

看：

- [interview-qna-complete.md](interview-qna-complete.md)
- [interview-highlights.md](interview-highlights.md)
- [resume-and-pitch.md](resume-and-pitch.md)

## 17. 读代码的方法

### 17.1 不要这样读

不要：

- 从目录第一个文件读到最后一个文件。
- 每一行都试图背下来。
- 陷入工具函数细节。
- 先读所有 SQL 再读业务。
- 只读成功路径。

### 17.2 应该这样读

按接口读：

```text
选一个 API
  -> 找 router
  -> 找 controller
  -> 找 service
  -> 找 repository
  -> 找 SQL / storage
  -> 找测试脚本
  -> 总结面试回答
```

按问题读：

```text
问题：并发上传会不会重复物理文件？
  -> physical_files 唯一索引
  -> upload_service 事务
  -> file_repository 插入/回查
  -> test_upload_race_consistency.sh
```

### 17.3 每读一个模块都问 6 个问题

```text
1. 这个模块负责什么？
2. 它的输入是什么？
3. 它的输出是什么？
4. 它依赖哪些下游？
5. 它最容易出什么 bug？
6. 项目用什么测试证明它没问题？
```

## 18. 最重要的面试主线

### 18.1 30 秒介绍

```text
Atlas 是我用 C++17 做的 Linux 网盘后端。底层是 epoll、非阻塞 socket、主从 Reactor 和线程池，上层实现 HTTP/1.1、chunked、multipart、MySQL 连接池、账号会话、文件上传下载、目录、回收站、公开分享和操作审计。文件部分采用逻辑文件和物理文件双表，通过 SHA-256 去重，ref_count 由 MySQL trigger 维护，并用事务、唯一索引和行锁处理并发上传、删除和配额一致性。登录/注册限流接入独立 Redis-Limiter 组件，Atlas 只保留业务适配层。
```

### 18.2 如果面试官只让讲一个点

优先讲文件一致性：

```text
逻辑文件 / 物理文件拆表
SHA-256 去重
trigger 维护 ref_count
事务 + 用户行锁防 quota 穿透
唯一索引 + 冲突回查防重复物理文件
DB commit 后删磁盘
storage checker 补偿 orphan
专项脚本验证
```

### 18.3 如果面试官偏底层

讲：

```text
MainReactor accept
SubReactor epoll 管连接
eventfd 唤醒
非阻塞 read/write
ET 读到 EAGAIN
EPOLLONESHOT
线程池处理业务
连接超时堆
```

### 18.4 如果面试官偏数据库

讲：

```text
files 是事实来源
physical_files 是物理对象
trigger 维护 ref_count
FOR UPDATE 锁用户行
唯一索引处理 sha256 并发
DB 内事务强一致
DB/磁盘靠补偿
```

### 18.5 如果面试官偏工程化

讲：

```text
Docker Compose
迁移脚本
单元测试
smoke
race test
failure injection
storage checker
benchmark gate
README / docs
```

## 19. 最后检查清单

面试前你应该能回答：

- [ ] 项目一句话介绍。
- [ ] MainReactor 和 SubReactor 怎么分工。
- [ ] 为什么用 epoll。
- [ ] ET 为什么要读到 `EAGAIN`。
- [ ] EPOLLONESHOT 解决什么。
- [ ] 线程池处理什么。
- [ ] 一个登录请求完整链路。
- [ ] 一个上传请求完整链路。
- [ ] 为什么拆 `files` 和 `physical_files`。
- [ ] SHA-256 去重怎么处理并发。
- [ ] `ref_count` 为什么用 trigger。
- [ ] 回收站文件是否占容量。
- [ ] 永久删除为什么 commit 后删磁盘。
- [ ] DB 成功但磁盘失败怎么办。
- [ ] quota 并发怎么防穿透。
- [ ] Redis-Limiter 为什么独立。
- [ ] Redis Lua 为什么原子。
- [ ] Redis 挂了怎么 fallback。
- [ ] `allow()` 是否幂等。
- [ ] 项目为什么不是生产级。
- [ ] 下一步怎么生产化。

如果这些都能讲清楚，这个项目就不需要逐行背代码了。

