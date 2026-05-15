# Atlas WebServer 面试完整问答

这份文档按“面试官可能怎么问”来组织。回答时不要死背整段，先用 2 到 3 句话回答核心结论，再根据面试官方向展开网络、数据库、一致性、限流或生产化边界。

## 1. 项目总览

### 1. 这个项目一句话怎么介绍？

Atlas WebServer 是一个基于 C++17 的 Linux 网盘后端项目。底层是非阻塞 socket、epoll、主从 Reactor、线程池和 HTTP/1.1 解析；上层实现账号会话、文件上传下载、目录、回收站、公开分享、操作审计、SHA-256 去重、容量配额、MySQL 迁移和 Redis 登录/注册限流。

面试时可以说：

> 它不是简单 CRUD，而是把一个 C++ WebServer 扩展成了网盘后端。我重点解决了 HTTP 层解耦、文件去重、引用计数一致性、并发上传配额、DB/磁盘非事务一致性和限流组件解耦。

### 2. 你在项目里主要做了什么？

可以按四条讲：

- 网络层：实现 epoll + 非阻塞 socket + Reactor + 线程池，支撑 HTTP 请求解析和响应写回。
- 业务层：实现认证、会话、文件上传下载、目录、回收站、分享、审计日志。
- 数据一致性：设计 `files` / `physical_files` 双表，SHA-256 去重，MySQL trigger 维护 `ref_count`，事务和行锁处理并发。
- 工程化：补 Docker Compose、数据库迁移、单测、smoke、并发上传测试、配额竞争测试、故障注入和 storage checker。

### 3. 这个项目最值得讲的亮点是什么？

最值得讲的是“从底层网络服务到真实业务一致性”的闭环：

- 底层不是直接用现成 Web 框架，而是自己处理 epoll、HTTP parser、multipart、chunked。
- 文件不是简单写磁盘，而是做逻辑文件和物理文件拆分、SHA-256 去重、回收站、引用计数。
- 并发路径不是靠口头保证，而是用事务、唯一索引、`FOR UPDATE`、trigger 和专项脚本验证。
- 主动承认本地磁盘不是生产级多机存储，并给出对象存储、outbox、异步清理的演进方案。

### 4. 这个项目和普通 WebServer demo 的区别是什么？

普通 WebServer demo 通常停留在静态文件、简单登录或压力测试。Atlas 做到了：

- HTTP 层支持 JSON、form、chunked、multipart、文件响应。
- 业务上有完整网盘模型：目录、文件、回收站、分享、公开下载、操作日志。
- 数据上有版本化迁移、外键、唯一索引、trigger、事务边界。
- 工程上有 Docker Compose、自动迁移、单测、smoke、race test、failpoint、benchmark。

### 5. 为什么选择做网盘？

网盘适合展示后端综合能力。它不只是 CRUD，而会自然引出：

- 大文件上传和 multipart 解析。
- 文件元数据和磁盘对象的一致性。
- 去重、引用计数、回收站、永久删除。
- 并发上传、配额竞争、分享下载次数竞争。
- 本地存储到对象存储的生产化演进。

### 6. 项目现在能称为生产级吗？

不能。可以说它是面试级、工程化较完整的网盘后端，但不能包装成生产级网盘。

原因：

- 文件存储仍是本地磁盘，不适合多实例直接部署。
- DB 和磁盘无法原子提交，只能靠顺序约束和巡检补偿。
- Redis 默认单点接入，生产需要 Sentinel / Cluster。
- 缺少断点续传、分片上传、Range 下载、预览、协作权限、异步任务平台。
- 缺少完整监控告警、容量治理、备份恢复演练。

更好的答法：

> 我会主动承认它不是生产级产品，但它覆盖了 WebServer、文件一致性、并发控制和工程验证这些面试项目最重要的点。生产化下一步会切对象存储、引入异步清理任务、完善 Redis/MySQL 高可用和监控。

## 2. 网络模型

### 7. WebServer 底层模型是什么？

服务端使用 Reactor 思想。主 Reactor 负责监听 `listenfd` 和 accept 新连接，SubReactor 负责连接上的读写事件，业务处理交给线程池。

可以这样说：

> 主线程不为每个连接创建线程，而是通过 epoll 监听事件。连接可读时先把数据读到连接缓冲区，再把请求投递给线程池处理。响应准备好后切换到 EPOLLOUT，由 Reactor 写回 socket。

### 8. 为什么用 epoll，不用 select/poll？

`select` 有 fd 数量限制，并且每次需要把 fd 集合从用户态复制到内核态，返回后还要线性扫描。`poll` 解决了 fd 数量限制，但仍然需要线性扫描。

`epoll` 的优势：

- 适合大量连接。
- fd 注册后保存在内核中，不需要每次重复传入完整集合。
- 通过就绪队列返回活跃事件，不需要扫描所有 fd。
- 支持 ET/LT、EPOLLONESHOT 等模式。

### 9. LT 和 ET 有什么区别？

LT 是水平触发，只要 fd 还有数据没读完，epoll 会持续通知。

ET 是边沿触发，只在状态变化时通知一次。使用 ET 时必须把 socket 设置为非阻塞，并循环读到 `EAGAIN` / `EWOULDBLOCK`，否则可能漏读数据。

项目里可以说：

> ET 减少重复事件通知，但实现要求更严格。读事件到来后，我会循环读取，直到内核缓冲区没有数据；写事件也要处理部分写，没写完继续注册 EPOLLOUT。

### 10. 为什么 socket 必须设为非阻塞？

Reactor 线程不能被某个连接卡住。如果 socket 是阻塞的，某个连接读不到完整数据或写缓冲区满时，线程会阻塞，其他连接事件就无法处理。

非阻塞 socket 的行为是：

- 没有数据可读时返回 `EAGAIN`。
- 写不进去时返回 `EAGAIN`。
- 应用层保存状态，等待下次可读/可写事件继续处理。

### 11. EPOLLONESHOT 是做什么的？

`EPOLLONESHOT` 让某个 fd 的事件触发一次后自动失效。这样可以避免同一个连接同时被多个线程处理。

典型流程：

```text
fd readable
  -> Reactor 读取数据
  -> 投递线程池
  -> 业务线程处理
  -> modfd 重新注册 EPOLLIN/EPOLLOUT
```

如果没有 `EPOLLONESHOT`，在多线程处理时，同一个连接可能重复触发读事件，导致并发读写同一个连接对象。

### 12. 主 Reactor 和 SubReactor 怎么协作？

主 Reactor 只负责 accept 新连接。新连接创建后按轮询方式分配给某个 SubReactor。SubReactor 有自己的 epoll 和唤醒机制，负责该连接后续读写事件。

这样做的好处：

- accept 和连接 IO 分离。
- 多个 SubReactor 可以分摊连接事件。
- 线程池负责业务，避免 Reactor 线程被数据库或文件操作阻塞太久。

### 13. 为什么不每个连接一个线程？

每连接一线程在连接数多时会产生大量线程栈内存、上下文切换和调度开销。很多连接其实大部分时间是空闲的，用线程阻塞等待不划算。

Reactor 模型用少量线程管理大量连接，只在事件就绪时处理，适合高并发网络服务。

### 14. 线程池处理什么？

线程池处理相对耗时或业务相关的工作：

- HTTP 请求解析后的路由分发。
- 鉴权、参数校验。
- MySQL 查询和事务。
- 文件服务逻辑。
- 生成 HTTP 响应。

Reactor 线程主要负责 accept、read、write 和事件注册，尽量保持轻量。

### 15. 线程池为什么不能无限扩容？

无限扩容会造成：

- 上下文切换过多。
- 内存消耗增加。
- 数据库连接池被打满。
- 下游 MySQL/Redis/磁盘成为瓶颈。

线程池大小应该和 CPU 核数、数据库连接数、请求类型有关。这个项目里线程数、最大线程数和队列模式都可以配置。

### 16. HTTP keep-alive 怎么处理？

如果请求支持 keep-alive，响应写完后不立即关闭连接，而是清理当前请求状态，重新注册读事件，继续等待下一次请求。

要注意：

- 每个请求的 body、header、临时上传文件状态要清理。
- 连接空闲太久要通过定时器关闭。
- 如果客户端声明 `Connection: close`，响应后关闭连接。

### 17. 如何处理半包和粘包？

TCP 是字节流，没有天然消息边界。HTTP parser 需要维护连接级缓冲区：

- header 没读完整时继续等待。
- 根据 `Content-Length` 判断 body 是否完整。
- `Transfer-Encoding: chunked` 时按 chunk 格式解析。
- multipart 上传时需要按 boundary 解析文件 part。

不能假设一次 `read` 就拿到完整请求。

### 18. 写响应时如果一次写不完怎么办？

非阻塞 socket 可能只写出一部分数据。连接对象需要记录已发送偏移量，未写完时继续注册 EPOLLOUT，下次 socket 可写时接着写。

这也是为什么响应写回和连接生命周期需要保存在 `HttpConnection` 中。

## 3. HTTP 与协议解析

### 19. HTTP 请求处理链路是什么？

以登录为例：

```text
POST /api/login
  -> HttpConnection 读取 socket 数据
  -> HTTP parser 解析 method/path/header/body
  -> Router 匹配路由
  -> AuthController 提取 username/password
  -> AuthService 校验密码并生成 session token
  -> Repository 写 user_sessions
  -> HttpResponse 返回 JSON
```

### 20. 为什么要把 HttpConnection 和业务解耦？

如果业务逻辑都写在 `HttpConnection` 里，会导致：

- 连接生命周期和业务 SQL 混在一起。
- controller 难以单测。
- 新增接口只能继续堆代码。
- 上传临时文件、鉴权、响应状态码难维护。

现在的边界是：

- `HttpConnection` 负责 socket IO、HTTP 解析、响应落地。
- `Router` 负责路由。
- `Controller` 负责参数和 HTTP 响应适配。
- `Service` 负责业务规则和事务。
- `Repository` 负责 SQL。

### 21. multipart/form-data 上传怎么处理？

上传请求会按 multipart boundary 找到文件 part。服务端把文件内容流式写入临时文件，避免整个文件常驻内存。

主要步骤：

```text
读请求体
  -> 识别 multipart boundary
  -> 提取 file part
  -> 写入 webroot/uploads/.tmp
  -> 计算 SHA-256 和大小
  -> 进入文件服务事务
```

### 22. 为什么上传要先落临时文件？

原因：

- 大文件不能全部放内存。
- 只有完整接收后才能计算 SHA-256。
- 事务失败时可以删除临时文件，不影响正式存储。
- 去重命中时可以直接删除本次临时文件，只新增逻辑记录。

### 23. chunked 请求体怎么处理？

chunked 请求没有固定 `Content-Length`，body 由多个 chunk 组成，每个 chunk 前有十六进制长度。

处理逻辑：

- 解析 chunk size。
- 读取对应长度数据。
- 跳过 CRLF。
- 遇到 size 为 0 的 chunk 表示结束。
- 拼出完整 body 或边读边写入上传临时文件。

### 24. 文件下载怎么返回？

下载前先鉴权和检查文件元数据，再根据 `physical_files.stored_name` 定位磁盘文件，构造文件响应。

需要设置：

- `Content-Type`
- `Content-Length`
- `Content-Disposition`
- 合理的状态码和错误响应

当前项目还没有完整 Range 下载，这是生产化边界之一。

### 25. 为什么要支持 HEAD/OPTIONS？

HEAD 可用于健康检查或探测资源元信息，不返回 body。OPTIONS 可用于接口能力或跨域预检场景。即使功能不复杂，支持它们能让 HTTP 行为更完整。

## 4. 认证与会话

### 26. 密码怎么存？

密码不保存明文。项目使用 PBKDF2-HMAC-SHA256，配合随机 salt 和迭代次数生成密码哈希。

这样即使数据库泄露，也不能直接得到用户密码。随机 salt 可以防止相同密码产生相同哈希，也能抵抗彩虹表。

### 27. 为什么不用简单 SHA256(password)？

简单 SHA256 太快，攻击者可以高速暴力枚举。PBKDF2 的价值在于通过多轮迭代增加破解成本。

更生产化的方案还可以使用 Argon2、bcrypt、scrypt。

### 28. session token 怎么生成？

登录成功后使用安全随机数生成 token，再写入 `user_sessions` 表。客户端后续通过：

```text
Authorization: Bearer <token>
```

访问私有接口。

回答重点：

- token 不能可预测。
- token 有过期时间。
- 登出会删除当前 session。
- 登出全部设备会删除用户所有 session。

### 29. 为什么用 Bearer Token，不用纯 Cookie Session？

Bearer Token 更适合前后端分离 API，前端可以把 token 放在请求头里。Cookie Session 也可以做，但要额外处理 CSRF、SameSite、跨域等问题。

项目里选择 Bearer Token 是为了让 API 调用和 curl 测试更直接。

### 30. 登录/注册限流怎么做？

Atlas 只保留业务适配层，通用限流逻辑来自独立项目 Redis-Limiter。

限流维度：

- 登录 IP。
- 登录用户名。
- 注册 IP。

Redis 正常时走分布式令牌桶；Redis 不可用时可以走本地 fallback。

### 31. 为什么登录要按 IP 和用户名两个维度限流？

只按 IP 限流会被代理池绕过。只按用户名限流会让攻击者故意刷某个用户名造成账号级 DoS。

两个维度结合可以同时限制：

- 同一个来源 IP 的暴力请求。
- 同一个用户名被反复尝试密码。

### 32. make_session_token 面试怎么讲？

可以说：

> `make_session_token` 的核心不是字符串拼接，而是使用安全随机源生成足够长度的不可预测 token，再编码成适合 HTTP 传输的字符串。它的安全目标是防止猜测、碰撞和重放后长期有效，所以还要配合过期时间、服务端存储和登出删除。

## 5. Redis-Limiter 集成

### 33. Atlas 和 Redis-Limiter 是什么关系？

Redis-Limiter 是独立的通用限流组件。Atlas 是它的一个消费者。

```text
Redis-Limiter
  -> C++ core
  -> Redis Lua
  -> TokenBucket / SlidingWindow
  -> fallback

Atlas
  -> 登录/注册限流业务 key
  -> HTTP 429 响应适配
  -> 配置读取
```

Atlas 不再复制 Redis-Limiter 源码，而是通过 CMake 链接外部 `redis_limiter::core`。

### 34. 为什么要把限流组件拆出去？

拆出去的好处：

- 限流组件可以被多个项目复用。
- Atlas 只关心业务 key 和 HTTP 响应，不关心算法细节。
- Redis-Limiter 可以单独演进 Python binding、FastAPI demo、benchmark、metrics。
- 简历上可以把 Atlas 和 Redis-Limiter 写成两个边界清晰的项目。

### 35. Redis-Limiter 是否必须单独部署？

不一定。

- C++ 项目可以直接链接 `redis_limiter::core`，不需要部署限流服务。
- Python 项目可以直接 `import redis_limiter`，也不需要单独服务。
- 如果未来要让 Java/Go/Node 等语言统一接入，可以再封装 HTTP/gRPC 限流服务，那时才需要部署。

### 36. Redis 限流为什么用 Lua？

限流需要“检查 + 扣减”原子完成。如果用普通命令：

```text
GET current
判断是否超限
SET current
```

中间可能被并发请求插入，导致超发。

Lua 脚本在 Redis 中单线程原子执行，可以把读取、计算、扣减、设置 TTL 放进一个脚本里。

### 37. 为什么用 Redis TIME？

多实例服务部署在不同机器上，本地时钟可能有偏差。限流算法依赖时间，如果每个实例用自己的本地时间，会影响窗口判断和令牌补充。

使用 Redis `TIME` 可以让所有实例基于同一个时间源计算。

### 38. 令牌桶和滑动窗口有什么区别？

令牌桶：

- 控制平均速率。
- 允许短时突发。
- 适合登录、短信、API QPS。

滑动窗口：

- 严格限制最近一段时间内最多多少次。
- 语义更直观。
- 高 QPS 长窗口下 Redis ZSET 成本更高。

### 39. Redis 挂了怎么办？

Redis-Limiter 支持三种 fallback：

- `LocalTokenBucket`：默认推荐，Redis 挂了切本地令牌桶，单机仍有保护。
- `FailOpen`：直接放行，可用性优先，但失去限流。
- `FailClosed`：直接拒绝，安全优先，但影响业务。

Atlas 登录/注册这种场景通常适合本地 fallback 或偏保守策略。

### 40. allow() 是否幂等？

不是。`allow()` 会消耗额度，同一个请求重复调用会重复扣令牌。

如果业务有重试，需要业务层提供幂等设计，例如 request id、幂等表、去重 key。限流组件只负责额度判断，不知道业务请求是否重复。

### 41. Redis 命令失败后为什么不盲目重试 Lua 写脚本？

如果脚本已经发到 Redis，但客户端因为网络异常没收到响应，那么无法确定 Redis 是否已经扣减额度。

此时盲目重试可能重复扣减。项目只在连接阶段做有限重试，对已经发出的限流写操作不盲目重放。

## 6. 数据库设计

### 42. 主要有哪些表？

核心表：

- `users`：用户账号、密码哈希、状态。
- `user_sessions`：Bearer Token 会话。
- `folders`：目录树。
- `files`：用户视角的逻辑文件。
- `physical_files`：磁盘真实文件对象。
- `file_shares`：分享 token、访问码、过期和下载次数。
- `operation_logs`：操作审计。
- `schema_migrations`：迁移版本。

### 43. 为什么要拆 `files` 和 `physical_files`？

因为用户看到的“文件”和磁盘上的“物理对象”不是一回事。

`files` 表表示：

- 哪个用户拥有这个文件。
- 文件名是什么。
- 在哪个目录。
- 是否删除、是否公开。

`physical_files` 表表示：

- 磁盘上真实存储的对象。
- SHA-256。
- 存储文件名。
- 引用计数。

拆开后才能支持去重、回收站、恢复、引用计数和多逻辑文件复用同一物理内容。

### 44. 为什么用 SHA-256 去重？

SHA-256 用于标识文件内容。相同内容产生相同 hash，可以复用同一条 `physical_files`。

好处：

- 节省磁盘空间。
- 为秒传 API 留扩展空间。
- 物理文件生命周期由引用计数控制。

边界：

- 计算 SHA-256 需要读完整文件。
- 理论上 hash 有碰撞概率，但工程上极低。
- 真正生产系统可结合 size、hash、对象存储 etag 等共同判断。

### 45. `ref_count` 怎么保证准确？

`files` 是逻辑引用事实来源。`physical_files.ref_count` 不由应用代码手动增减，而是由 MySQL trigger 在 `files` insert/delete 后维护。

优点：

- 引用变化和逻辑文件变化处于同一个 DB 事务。
- 减少应用层漏加、漏减、异常路径漂移。
- 并发删除和上传时更可靠。

### 46. `ref_count` 包含回收站文件吗？

包含。回收站文件仍然有 `files` 逻辑记录，仍占用容量和磁盘引用。只有永久删除 `files` 记录后，trigger 才减少 `ref_count`。

### 47. 为什么不用应用层手动维护 `ref_count`？

应用层手动维护容易出现：

- 插入逻辑文件成功但加引用失败。
- 删除逻辑文件成功但减引用失败。
- 并发上传/删除导致引用漂移。
- 异常返回路径漏处理。

trigger 把引用计数维护放到数据库内部，和 `files` 表变更绑定，可靠性更高。

### 48. 数据库迁移怎么管理？

通过 `migrations/` 下版本化 SQL 和 `schema_migrations` 表记录已执行版本。这样新环境可以自动初始化，老环境可以按版本升级。

面试可强调：

> 我没有把建表逻辑写死在服务启动里，而是用迁移脚本管理 schema 演进，避免应用启动时动态 ALTER 带来的不可控行为。

## 7. 文件上传与一致性

### 49. 上传完整流程是什么？

```text
multipart 接收文件
  -> 写入 .tmp 临时文件
  -> 计算 size 和 SHA-256
  -> 开启 MySQL 事务
  -> 锁用户行检查 quota
  -> 检查目录和文件名冲突
  -> 查找或插入 physical_files
  -> 插入 files 逻辑文件
  -> commit
  -> 临时文件转正式文件或去重命中后删除临时文件
```

### 50. 为什么上传配额检查要放在服务端？

前端 preflight 只能改善体验，不能作为安全边界。用户可以绕过前端直接调用 API。

服务端必须在真正落库前重新检查：

- 单文件大小限制。
- 用户总容量限制。
- 目录是否存在。
- 同名文件是否冲突。

### 51. 并发上传怎么防止 quota 穿透？

问题是多个请求同时看到相同剩余额度，全部判断通过。

解决：

- 上传事务内 `SELECT user FOR UPDATE` 锁住用户行。
- 同一用户上传配额检查串行化。
- 每次在锁内重新计算已用容量。
- 插入文件后再提交。

测试脚本验证 10 个并发上传在 100B quota 下只成功 3 个 30B 文件，最终 90B，不超额。

### 52. 并发上传相同内容怎么只生成一份物理文件？

依靠三层保护：

- `physical_files.sha256` 唯一索引。
- 事务中按 SHA-256 查询物理文件。
- 并发插入撞唯一约束时回查已有记录。

即使两个请求同时认为不存在，最终也只有一个插入成功，另一个复用已有物理文件。

### 53. DB 成功但磁盘删除失败怎么办？

这是本地磁盘方案的生产化边界。MySQL 事务不能覆盖文件系统操作。

项目采用：

- 永久删除时先在 DB 事务内删除逻辑记录和无引用物理记录。
- DB commit 后再删除磁盘物理文件。
- 如果磁盘删除失败，用户视角已经正确，后续由 storage checker 发现 orphan 文件并补偿清理。

### 54. 为什么不先删磁盘再 commit DB？

如果先删磁盘，再 commit DB 失败，就会出现 DB 仍引用一个已经不存在的磁盘文件，用户下载会失败。

先 commit DB 再删磁盘的风险是留下 orphan 文件，这比“DB 引用丢失文件”更容易补偿。

### 55. 上传失败会不会留下临时文件？

正常设计中失败路径会清理临时文件。项目还通过 failpoint 验证提交前失败：

- DB 回滚。
- `files` 无残留。
- `physical_files` 无残留。
- 磁盘无 orphan。

### 56. storage checker 检查什么？

主要检查：

- DB 记录引用的物理文件是否存在。
- 磁盘上是否有 DB 不再引用的 orphan 文件。
- `physical_files.ref_count` 是否和 `files` 统计一致。
- 临时目录是否有过期残留。

它是 DB/磁盘非事务边界的补偿工具。

## 8. 回收站与分享

### 57. 普通删除和永久删除有什么区别？

普通删除是软删除，设置 `deleted_at`，文件进入回收站。此时逻辑文件仍存在，仍占用容量，`ref_count` 不减少。

永久删除才会删除 `files` 记录，trigger 减少 `ref_count`。如果物理文件没有其他引用，再删除 `physical_files` 和磁盘对象。

### 58. 恢复文件时要注意什么？

需要检查：

- 文件属于当前用户。
- 文件确实在回收站。
- 原目录是否还存在。
- 同目录下是否有同名活跃文件。

如果原目录不存在，可以恢复到根目录或做冲突命名。

### 59. 分享链接怎么设计？

分享表保存：

- 分享 token。
- 访问码 hash。
- 过期时间。
- 最大下载次数。
- 当前下载次数。
- 对应文件 id。

访问分享时校验 token、过期时间、访问码和下载次数，再返回元数据或文件下载。

### 60. 分享下载次数并发怎么保证？

应该在事务里锁住分享记录，检查当前下载次数是否小于最大次数，再增加下载次数。

如果不加锁，多个并发请求可能同时看到还剩 1 次，导致超发下载。

## 9. 日志、审计与错误处理

### 61. 操作日志记录什么？

操作日志记录用户、动作、资源、结果、IP、User-Agent 和详情。它适合用于审计和问题排查。

例如：

- 登录成功/失败。
- 上传、下载、删除、恢复。
- 分享创建和访问。
- 限流拒绝。

### 62. 普通日志和操作审计有什么区别？

普通日志面向开发和运维，记录服务运行状态、错误堆栈、调试信息。

操作审计面向业务追踪，记录用户做了什么、对哪个资源、结果如何。

### 63. 错误响应怎么设计？

应该统一 JSON 格式，例如：

```json
{ "code": 400, "message": "bad request" }
```

好处：

- 前端处理一致。
- curl/测试脚本容易判断。
- 不同 controller 不会返回混乱格式。

## 10. 测试与验证

### 64. 项目有哪些测试？

主要有：

- CTest 单元测试：parser、HTTP message、multipart、file service、share service、auth rate limiter。
- smoke 测试：注册、登录、私有 API、文件主链路、分享、chunked。
- 并发测试：上传同内容、quota race、share race。
- 故障注入：上传事务提交前失败。
- storage checker：DB/磁盘一致性巡检。
- benchmark：wrk 压测和结果 gate。

### 65. 为什么要写专项脚本，不只写单元测试？

单元测试适合验证纯函数和局部逻辑，但文件上传一致性涉及：

- HTTP 请求。
- MySQL 事务。
- 磁盘文件。
- 并发请求。
- Docker 环境。

这些更适合用集成脚本和专项脚本验证。

### 66. 你怎么证明并发上传没有问题？

用 `scripts/test_upload_race_consistency.sh`：

- 并发上传同一内容。
- 断言只有一条 `physical_files`。
- 断言 `ref_count` 等于逻辑文件数。
- 并发永久删除后 DB 和磁盘都清理干净。

### 67. 你怎么证明 quota 不会被打穿？

用 `scripts/test_upload_quota_race.sh`：

- 设置较小 quota。
- 并发上传多个文件。
- 断言成功数量和最终容量都不超过 quota。
- 失败请求返回明确错误。

### 68. benchmark 怎么看？

不能只看单次 QPS。要看：

- 是否有非 2xx/3xx 响应。
- 是否有 socket error。
- 服务和 MySQL 容器是否重启。
- 是否有 SIGSEGV。
- 延迟 P95/P99。
- 结果是否通过 gate。

更稳妥的说法：

> benchmark 用于对比和发现瓶颈，不直接包装成生产容量承诺。

## 11. C++ 与工程实现

### 69. 项目里用了哪些 C++ 特性？

主要是 C++17、RAII、标准库容器、线程同步、智能指针、字符串处理、封装类等。

可以重点讲：

- RAII 管理文件、连接、锁。
- `std::string` / `std::vector` 管理缓冲。
- `std::mutex` / 条件变量用于线程同步。
- 禁止拷贝的连接对象避免资源重复释放。

### 70. RAII 在项目里有什么价值？

RAII 可以把资源释放绑定到对象生命周期，避免异常路径漏释放。

典型资源：

- FILE*。
- MYSQL_RES*。
- Redis 连接。
- mutex lock。
- 上传临时文件。

面试回答：

> C++ 服务最怕异常路径资源泄漏，所以我更倾向把资源封装成对象，通过析构自动释放，而不是每个 return 分支手动清理。

### 71. 日志模块为什么要异步？

同步日志每次写文件都可能阻塞业务线程。异步日志把日志先放到队列，由后台线程批量写文件，降低请求路径阻塞。

边界：

- 队列满了要决定丢弃、阻塞或退化同步写。
- 退出时要 flush。
- 日志线程和文件轮转要保证线程安全。

### 72. 你用到了哪些设计模式？

可以结合项目说，不要硬套：

- Reactor：事件驱动网络模型。
- Thread Pool：复用工作线程处理业务任务。
- Repository：封装 SQL 访问。
- Factory：Redis-Limiter 创建不同限流器。
- Strategy：限流 fallback 策略 `LocalTokenBucket` / `FailOpen` / `FailClosed`。
- Adapter：Atlas 的 `service/rate_limit` 把通用限流组件适配到登录/注册业务。
- Singleton：日志模块单例。

## 12. 生产化与扩展

### 73. 如果要支持多实例部署，最大问题是什么？

最大问题是本地磁盘。多个实例各自有本地 `webroot/uploads`，请求打到不同机器时可能找不到文件。

解决方向：

- 使用对象存储 S3/MinIO/OSS。
- 后端服务无状态化。
- 文件元数据仍在 MySQL。
- 物理文件删除改成异步任务。

### 74. 如何把本地磁盘换成对象存储？

抽象 `Storage` 接口：

```text
put(temp_file) -> object_key
get(object_key) -> stream
delete(object_key)
exists(object_key)
```

实现：

- `LocalStorage`
- `S3Storage` / `MinioStorage` / `OSSStorage`

上传流程改为对象存储写入，DB 只保存 object key、size、sha256。

### 75. 如何做断点续传/分片上传？

需要新增上传会话：

- `upload_sessions`
- `upload_parts`

流程：

```text
create upload session
upload part N
record part hash/size
complete upload
merge parts or compose object
calculate final sha256
insert files metadata
```

还要处理过期 session 清理、part 重传幂等、并发 complete。

### 76. 如何做秒传？

客户端先计算文件 SHA-256，调用 preflight：

```text
POST /api/drive/files/preflight
sha256 + size + filename
```

如果服务端已有相同 `physical_files`，可以直接新增 `files` 逻辑记录，不再上传内容。

安全边界：

- 不能只信客户端 hash。
- 需要校验 size、权限、quota。
- 真正上传时仍以服务端计算 hash 为准。

### 77. 如何做 Range 下载？

解析 `Range: bytes=start-end`，返回：

- `206 Partial Content`
- `Content-Range`
- 对应字节范围 body

需要支持断点续传、视频预览、大文件下载。当前项目还没有，这是可承认的边界。

### 78. 如何做异步清理任务？

可以引入 outbox 表：

```text
cleanup_tasks(id, type, object_key, status, retry_count, next_run_at)
```

DB 事务内写入 cleanup task，后台 worker 异步删除磁盘或对象存储文件。失败则重试并告警。

这样比同步删除更可靠，也更适合对象存储。

### 79. MySQL 怎么做高可用？

生产上可以考虑：

- 主从复制。
- 自动备份和恢复演练。
- 连接池健康检查。
- 读写分离。
- 慢查询监控。
- 关键表索引优化。

当前项目只覆盖单 MySQL 开发/演示环境。

### 80. Redis 怎么做高可用？

可以使用：

- Redis Sentinel。
- Redis Cluster。
- 云 Redis。
- 客户端重连和超时控制。
- 限流 fallback。

当前 Redis-Limiter 默认单 Redis 接入，生产需要高可用拓扑。

## 13. 高频压力问题

### 81. 面试官问“机器挂了怎么办”怎么答？

当前本地磁盘版本：

> 如果机器挂了，本地磁盘上的文件会不可用，所以当前方案不适合生产多机部署。我会主动把这是边界说清楚。生产方案应该把文件放到对象存储，服务实例无状态化，MySQL 保存元数据，对象存储保证文件持久性。

### 82. 面试官问“磁盘删失败怎么办”怎么答？

> DB commit 后删除磁盘失败会留下 orphan 文件，但不会影响用户视角，因为 DB 已经没有引用。项目里用 storage checker 发现并清理 orphan。更生产化的方案是 outbox + cleanup worker + 重试 + 告警。

### 83. 面试官问“DB 成功但文件移动失败怎么办”怎么答？

要区分阶段：

- commit 前失败：DB rollback，清理临时文件。
- commit 后失败：可能出现 DB 引用但磁盘缺失，这是更严重的问题。

当前要尽量把正式文件准备放在 commit 前可控阶段，并在失败时 rollback。生产上对象存储可以用状态机：uploading -> committed -> active，后台补偿异常状态。

### 84. 面试官问“你这个项目最大难点是什么”怎么答？

建议答文件一致性和并发控制：

> 最大难点不是接口本身，而是文件元数据、物理文件和磁盘对象三者的一致性。尤其是并发上传相同内容、quota 竞争、回收站和永久删除。我的做法是把 DB 内一致性交给事务、行锁、唯一索引和 trigger，把 DB/磁盘边界通过顺序约束和巡检补偿兜住，并用专项脚本验证。

### 85. 面试官问“哪里体现高并发”怎么答？

不要只说“用了 epoll 所以高并发”。更好的答法：

- 网络层用 epoll + 非阻塞 IO 管大量连接。
- 线程池限制业务并发，避免每连接一线程。
- 数据层用连接池复用 MySQL 连接。
- 并发正确性用行锁、唯一索引和事务保护。
- benchmark 用 gate 验证非 2xx、socket error、重启和延迟。

### 86. 面试官问“为什么不用 Nginx/Go/Java 框架”怎么答？

> 如果目标是最快上线，肯定用成熟框架。但这个项目目标是展示 C++ Linux 网络编程和后端工程能力，所以我自己实现 epoll、HTTP parser、线程池和文件处理。生产环境可以让 Nginx 做反向代理，后端专注 API。

### 87. 面试官问“项目有什么 bug 或踩坑”怎么答？

可以讲这些：

- `ref_count` 应用层手动维护容易漂移，后来改成 trigger。
- 并发 quota 检查会穿透，后来加用户行锁。
- DB 和磁盘不能原子提交，后来加 storage checker 和失败清理。
- benchmark 不能只看 QPS，后来加非 2xx、socket error、容器重启 gate。
- 限流组件内嵌在 Atlas 不利于复用，后来拆成独立 Redis-Limiter。

### 88. 面试官问“如果让你继续做，你下一步做什么”怎么答？

优先级：

1. 抽象 Storage，接 MinIO/S3/OSS。
2. 引入 outbox + cleanup worker。
3. 支持 Range 下载、分片上传、秒传 API。
4. Redis Sentinel/Cluster 和 MySQL 备份恢复。
5. 增加统一 metrics、trace、告警。
6. 完善权限协作和文件预览。

## 14. Redis-Limiter 独立项目追问

### 89. Redis-Limiter 项目一句话怎么介绍？

Redis-Limiter 是一个基于 C++17、hiredis、Redis Lua 和 pybind11 的可复用分布式限流组件。它支持令牌桶、滑动窗口、Redis TIME、SCRIPT LOAD/EVALSHA、连接池、故障降级、Python/FastAPI 接入、Docker 验证和压测报告。

### 90. Redis-Limiter 和 Atlas 能分开写简历吗？

可以。它们边界不同：

- Atlas：C++ Linux 网盘系统，重点是 WebServer、HTTP、MySQL、文件一致性。
- Redis-Limiter：通用限流组件，重点是 Redis Lua、限流算法、fallback、pybind11、压测。

不要说 Atlas 复制了限流代码。现在更好的说法是 Atlas 通过外部 `redis_limiter::core` 接入。

### 91. Redis-Limiter 为什么用 C++ 写，还提供 Python binding？

C++ 负责高性能核心和 hiredis 封装；pybind11 让 Python 服务也能低成本接入同一套限流能力。

这体现的是组件复用：

- C++ 服务直接链接 core。
- Python 服务 import 扩展模块。
- 未来可以继续封装 HTTP/gRPC 服务给更多语言。

### 92. Redis-Limiter 压测怎么说？

可以说报告中热点 key 严格有效性压测下理论放行和实际放行一致，`over_issued=0`，说明 Lua 原子扣减在该测试条件下没有超发。

注意不要夸大：

> 这些数字是 Docker 短压测快照，不等于生产容量。它主要证明功能有效性和验证链路完整。

## 15. 反问和收尾

### 93. 如果面试官让你选一个点展开，选什么？

推荐选“文件一致性”。

原因：

- 比单纯 epoll 更贴近真实后端。
- 能讲数据模型、事务、并发、异常、补偿。
- 能自然承认生产边界。

开头可以说：

> 我最想展开文件一致性，因为这里不是简单上传下载，而是涉及逻辑文件、物理文件、磁盘对象、去重、回收站和并发删除。

### 94. 如果面试官偏底层，怎么转向网络模型？

可以说：

> 底层网络模型是主从 Reactor。主 Reactor accept，新连接分发给 SubReactor，SubReactor 用 epoll 管读写事件，业务任务交给线程池。这里关键是非阻塞 IO、ET 读到 EAGAIN、EPOLLONESHOT 防止同连接被多线程并发处理。

### 95. 如果面试官偏数据库，怎么转向一致性？

可以说：

> 数据库里我把 `files` 作为事实来源，`physical_files` 表只表示真实物理对象，引用计数由 trigger 维护。上传和删除路径用事务、用户行锁、物理文件行锁和唯一索引处理并发，DB 和磁盘的非事务边界用 storage checker 补偿。

### 96. 如果面试官偏架构，怎么讲边界？

可以说：

> 当前架构适合单机演示和面试项目。真正生产化需要把本地磁盘换成对象存储，把同步清理换成 outbox worker，把 Redis/MySQL 做高可用，并补齐监控、告警、备份恢复和容量治理。

### 97. 如何避免被问倒？

不要把项目说成“生产级网盘”或“高并发平台”。要主动承认：

- 本地磁盘不是多机方案。
- DB/磁盘没有原子事务。
- Redis 单点需要高可用。
- benchmark 是测试环境快照。
- 分片上传、Range 下载、预览、协作权限还没做。

主动承认边界，反而更像真实工程经验。

### 98. 最终 1 分钟标准回答

> Atlas 是我用 C++17 做的 Linux 网盘后端。底层是非阻塞 socket、epoll、主从 Reactor 和线程池，上层实现 HTTP/1.1、chunked、multipart、MySQL 连接池、账号会话、文件上传下载、目录、回收站、公开分享和操作审计。文件部分采用逻辑文件和物理文件双表，通过 SHA-256 去重，`files` 表作为事实来源，`physical_files.ref_count` 由 MySQL trigger 维护。上传路径用事务、用户行锁和唯一索引处理并发配额和同内容去重。DB 和磁盘不是同一个事务资源，所以我把磁盘删除放在 DB commit 后，并用 storage checker 和 failpoint 脚本做补偿验证。登录/注册限流接入了我独立实现的 Redis-Limiter 组件，Atlas 只保留业务适配层。这个项目我不会包装成生产级网盘，生产化下一步会做对象存储、异步清理任务、Range/分片上传、Redis/MySQL 高可用和监控告警。

