# Atlas WebServer 文档导航

这份文档是 Atlas 后端仓库的阅读入口。仓库里的资料已经很多，不建议从第一篇读到最后一篇。更高效的方式是先按目标选择路径：跑项目、理解架构、准备面试、读源码、补生产化边界。

## 1. 先看哪几篇

第一次打开项目，按这个顺序：

```text
1. README.md
2. docs/quickstart-5min.md
3. docs/architecture.md
4. docs/request-sequence.md
5. docs/project-study-guide-complete.md
```

不要一开始逐行读所有代码。Atlas 涉及 WebServer、HTTP、MySQL、文件存储、限流、测试和部署，直接逐行读会很慢。先跑主链路，再带着问题读核心模块。

## 2. 文档分类

| 目标 | 推荐文档 |
| --- | --- |
| 快速跑通项目 | [quickstart-5min.md](quickstart-5min.md) |
| 查接口字段 | [api.md](api.md) |
| 理解整体架构 | [architecture.md](architecture.md)、[request-sequence.md](request-sequence.md) |
| 理解文件模块 | [file-module.md](file-module.md) |
| 理解数据库迁移 | [database-migrations.md](database-migrations.md) |
| 理解目录结构 | [project-structure.md](project-structure.md) |
| 系统学习 | [project-study-guide-complete.md](project-study-guide-complete.md) |
| 面试问答 | [interview-qna-complete.md](interview-qna-complete.md) |
| 面试展示 | [interview-highlights.md](interview-highlights.md) |
| 简历表达 | [resume-and-pitch.md](resume-and-pitch.md) |
| 项目复盘 | [final-review.md](final-review.md) |
| 性能验证 | [benchmark.md](benchmark.md)、[perf-flamegraph.md](perf-flamegraph.md) |
| 生产化路线 | [production-hardening-roadmap.md](production-hardening-roadmap.md) |
| 排障运维 | [ops-debug-runbook.md](ops-debug-runbook.md) |
| 答辩攻防 | [interview-defense-playbook.md](interview-defense-playbook.md) |

## 3. 面试准备路径

### 3.1 只有 30 分钟

只看：

```text
1. README.md 的项目定位、核心能力、系统架构
2. docs/interview-highlights.md
3. docs/resume-and-pitch.md
4. docs/interview-defense-playbook.md 的最终 2 分钟答辩稿
```

目标是能讲清楚：

- 这是什么项目。
- 为什么不是普通 WebServer demo。
- WebServer 底层怎么做。
- 文件上传和一致性难点在哪里。
- 当前生产化边界是什么。

### 3.2 有 2 小时

按这个顺序：

```text
1. README.md
2. docs/request-sequence.md
3. docs/file-module.md
4. docs/interview-qna-complete.md
5. docs/interview-defense-playbook.md
```

重点背熟：

- Reactor + ThreadPool。
- HTTP parser / multipart / chunked。
- `files` 和 `physical_files` 拆表。
- SHA-256 去重和 `ref_count`。
- DB 和磁盘不能原子提交。
- Redis-Limiter 为什么拆出去。

### 3.3 有 1 天

按学习路线完整过一遍：

```text
1. docs/project-study-guide-complete.md
2. docs/architecture.md
3. docs/request-sequence.md
4. docs/file-module.md
5. docs/database-migrations.md
6. docs/interview-qna-complete.md
7. docs/production-hardening-roadmap.md
8. docs/ops-debug-runbook.md
```

当天一定要自己跑一遍：注册、登录、上传、列表、下载、删除、回收站、恢复、分享、限流和一致性检查。

## 4. 源码阅读路径

Atlas 源码不要按目录字母顺序读，按请求链路读。

### 4.1 服务启动

```text
app/main.cpp
app/config.cpp
app/webserver.cpp
app/webserver_sub_reactor.cpp
infra/threadpool/threadpool.h
infra/db/sql_connection_pool.cpp
```

要回答：配置怎么加载、listenfd 怎么创建、MainReactor 和 SubReactor 怎么协作、线程池什么时候介入、MySQL 连接池怎么被业务线程使用。

### 4.2 HTTP 链路

```text
http/core/connection.cpp
http/core/parser.cpp
http/core/http_message.cpp
http/core/response.cpp
http/router/router.cpp
```

要回答：半包粘包怎么处理、什么时候认为请求完整、keep-alive 怎么处理、读写事件怎么切换、路由怎么分发到 controller。

### 4.3 认证链路

```text
http/controllers/auth_controller.cpp
service/auth/auth_service.cpp
repo/mysql/user_repository.cpp
repo/mysql/session_repository.cpp
service/rate_limit/auth_rate_limiter.cpp
```

要回答：密码怎么存、token 怎么生成、session 怎么过期、登录/注册限流 key 怎么设计、Atlas 和 Redis-Limiter 怎么解耦。

### 4.4 文件链路

```text
http/controllers/file_controller.cpp
http/files/multipart_parser.cpp
service/files/upload_service.cpp
service/files/file_service.cpp
service/files/share_service.cpp
repo/mysql/file_repository.cpp
migrations/
```

要回答：上传为什么先落临时文件、SHA-256 什么时候计算、并发同内容上传怎么去重、配额怎么防穿透、回收站和永久删除有什么区别、DB 成功但磁盘失败怎么办。

## 5. 最重要的 8 个问题

1. Atlas 底层 WebServer 模型是什么？
2. MainReactor、SubReactor、线程池分别做什么？
3. HTTP 半包、chunked、multipart 怎么处理？
4. `make_session_token` 为什么要用安全随机数？
5. `files` 和 `physical_files` 为什么拆表？
6. 并发上传同一文件和并发配额怎么保证正确？
7. DB 和磁盘为什么不能原子提交，当前怎么处理，生产怎么改？
8. Redis-Limiter 为什么作为独立项目，而不是复制进 Atlas？

## 6. 生产化边界怎么学

优先看：

```text
docs/production-hardening-roadmap.md
docs/ops-debug-runbook.md
docs/interview-defense-playbook.md
```

必须主动承认：当前本地磁盘不适合多实例，DB 和磁盘不能原子提交，断点续传、分片上传、Range 下载、对象存储、文件预览、权限协作还不是完整生产能力。

正确表达不是“我都做了”，而是：

```text
我已经做了核心链路和关键一致性处理，生产化还需要对象存储、outbox 补偿、后台任务、分片上传、Range 下载和完整可观测性。
```

## 7. 演示前检查

```text
1. README 能打开
2. docs/quickstart-5min.md 命令能跑
3. /healthz 成功
4. 注册登录成功
5. 上传下载成功
6. 删除恢复成功
7. 分享成功
8. 限流脚本能触发 429
9. storage consistency checker 无异常
10. git status 干净
```

## 8. 最终建议

Atlas 的学习重点不是背所有接口，而是抓住三条主线：

```text
WebServer 底层：epoll + Reactor + ThreadPool
网盘业务一致性：上传、去重、配额、回收站、分享
工程边界：本地磁盘、多实例、DB/磁盘非原子、生产化补偿
```

只要这三条能讲清楚，项目就能支撑大多数面试追问。
