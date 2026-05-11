# 简历与面试话术

本文档用于把项目压缩成简历 bullet、30 秒开场和 STAR 故事。详细工程说明见 [interview-highlights.md](interview-highlights.md)。

## 简历项目描述

**Atlas WebServer｜C++ Linux 网盘后端**

基于 C++17、epoll、线程池、HTTP/1.1、MySQL 和 Redis 实现的网盘型 WebServer，支持账号会话、登录/注册限流、文件上传下载、SHA-256 去重、回收站、公开分享、上传配额、审计日志、数据库迁移、Docker Compose 和自动化回归测试。

## 简历最终可用版

项目名称：

**Atlas｜C++ Linux 网盘系统**

技术栈：

`C++17`、`Linux`、`epoll`、`Reactor`、`pthread`、`HTTP/1.1`、`MySQL`、`Redis`、`Docker Compose`、`Vue`、`Vite`

一句话描述：

基于 C++17、epoll、Reactor、线程池、MySQL、Redis 和 Vue/Vite 实现的前后端分离网盘系统，支持账号会话、登录/注册限流、文件上传下载、目录、回收站、公开分享、操作审计、Docker Compose 部署和自动化回归验证。

推荐放简历的 4 条 bullet：

- 实现非阻塞 HTTP 服务，支持 HTTP/1.1、keep-alive、chunked body、multipart 上传解析和 MySQL 连接池，提供注册登录、文件管理、目录、回收站、公开分享等网盘 API。
- 重构 HTTP 与业务边界，引入 `HttpRequest`、`RequestContext`、`HttpResponse`，将认证、文件、分享和操作日志按 controller / service / repository 分层，降低连接对象与业务逻辑耦合。
- 设计 SHA-256 物理文件去重模型，以 `files` 表作为逻辑引用事实来源，通过 MySQL trigger 维护 `physical_files.ref_count`，避免应用层手动维护引用计数导致漂移。
- 在上传、配额校验和永久删除路径中使用事务、用户行锁、物理文件行锁和唯一约束处理并发一致性，并通过 smoke、并发上传、quota race、failpoint 和 storage checker 脚本验证异常路径。

## 简历 Bullet

偏工程完整度版本：

- 基于 C++17 实现主从 Reactor WebServer，使用非阻塞 socket、epoll、线程池、HTTP/1.1 parser、chunked/multipart 解析、MySQL 连接池和 Redis 限流支撑网盘 API。
- 将 HTTP 连接层与业务层解耦，引入 `HttpRequest`、`HttpResponse`、`RequestContext`，把文件、认证、操作日志拆分为 controller / service / repository / infra 分层。
- 设计 SHA-256 文件去重模型，使用 `files` 表表示用户逻辑文件、`physical_files` 表表示物理文件，支持回收站、恢复、永久删除和公开分享。
- 将 `physical_files.ref_count` 从应用层手动维护改为 MySQL trigger 维护，并在上传、永久删除路径中使用事务、用户行锁和物理文件行锁保证并发一致性。
- 增加 DB/磁盘一致性巡检与修复脚本，覆盖 orphan 文件、缺失磁盘文件、`ref_count` 漂移等场景，并通过并发上传、配额竞争、故障注入专项脚本验证异常路径。

偏后端并发版本：

- 针对并发上传相同内容的竞态问题，结合 SHA-256 唯一约束、`SELECT ... FOR UPDATE` 和事务回查机制，保证多请求只生成一条物理文件记录。
- 针对用户容量 quota 并发穿透问题，在上传事务内锁定用户行并重新计算已用容量，专项测试验证 10 个并发上传下最终用量不超过 quota。
- 针对 DB 与本地文件系统非事务资源一致性问题，将磁盘删除放在 DB commit 后执行，并用 storage checker 兜底清理 orphan 文件。
- 针对上传失败路径引入默认关闭的 failpoint，验证物理文件落盘后、事务提交前失败时，DB 回滚且磁盘无残留。

压缩到 3 条版本：

- 实现 C++17 Linux WebServer，采用 epoll、主从 Reactor、线程池、HTTP/1.1 parser、chunked/multipart 上传解析、MySQL 连接池和 Redis 限流，支撑网盘文件 API。
- 重构 HTTP 与业务边界，引入 request / response / context 模型，并按 controller / service / repository / infra 拆分认证、文件、分享和操作日志模块。
- 设计文件去重与一致性机制：SHA-256 物理文件复用、trigger 维护 `ref_count`、事务和行锁保护并发上传/删除/配额，配套 storage checker、race test、quota test 和 failpoint 回归。

## 30 秒开场

> 我的主力项目是 Atlas WebServer，一个 C++ Linux 网盘后端。底层是非阻塞 socket、epoll、主从 Reactor、线程池和 MySQL 连接池，上层支持账号会话、文件上传下载、目录、回收站、分享和上传配额。我重点做了两块工程化改造：第一是把 `HttpConnection` 和业务解耦成 request / context / response 模型，按 controller、service、repository 分层；第二是文件一致性和并发控制，用 SHA-256 做物理文件去重，`ref_count` 交给 MySQL trigger 维护，上传和删除路径加事务和行锁，并补了并发上传、quota race、故障注入和 DB/磁盘巡检脚本来验证。

## 1 分钟展开

> 这个项目最开始是一个传统 C++ WebServer，后面我把它扩展成网盘后端。底层保留了网络编程能力，包括 epoll、线程池、HTTP parser、chunked 和 multipart 上传。业务上支持注册登录、Bearer Token 会话、文件上传下载、回收站、分享链接和审计日志。
>
> 我重点解决的是文件模块的一致性问题。相同内容的文件会按 SHA-256 去重，多个用户逻辑文件复用一条物理文件记录。之前如果在业务代码里手动维护引用计数，上传、删除或者并发异常时很容易出错，所以我改成 `files` 表作为事实来源，`physical_files.ref_count` 由 MySQL trigger 维护。上传路径里用事务包住配额检查、物理文件查找或创建、逻辑文件插入，并用用户行锁防止 quota 并发穿透，用物理文件行锁和唯一约束防止重复物理文件。
>
> DB 和磁盘没法做到一个原子事务，所以我把磁盘删除放在 DB commit 后，并补了巡检脚本清理 orphan 文件。为了证明这些设计有效，我还写了并发上传、并发删除、quota race 和 failpoint 故障注入脚本，能在 Docker 环境里复现。

## STAR 故事

### S: Situation

项目从课程式 WebServer 演进成网盘后端后，文件模块不再只是简单上传下载，而是引入了目录、回收站、去重、分享和配额。此时 `files` 逻辑记录、`physical_files` 物理记录和磁盘文件三者之间存在一致性问题。

### T: Task

需要保证：

- 并发上传同一内容不会生成多份物理文件
- `ref_count` 不会因为异常或并发出现漂移
- 用户上传 quota 不会被并发请求绕过
- DB 和磁盘之间出现非事务边界时能被巡检和补偿
- 上传失败时不能留下半条 DB 记录或 orphan 文件

### A: Action

采取的措施：

- 将业务层从 `HttpConnection` 中拆出，建立 request / context / response 模型
- 用 `files` 表表示逻辑引用，`physical_files` 表表示物理文件
- 用 SHA-256 唯一约束实现物理文件去重
- 用 MySQL trigger 维护 `physical_files.ref_count`
- 上传事务内锁用户行，串行化 quota 检查
- 上传事务内锁同 SHA-256 物理文件行，并处理唯一约束竞争
- 永久删除时锁 `files` 记录，DB commit 后再删磁盘文件
- 增加 storage checker 检查 DB/磁盘不一致
- 增加 race、quota、failure cleanup 专项测试

### R: Result

结果：

- 12 个并发上传同一内容只生成 1 条物理文件记录，`ref_count = 12`
- 并发永久删除后逻辑文件、物理记录和磁盘文件都被清理
- 10 个并发上传在 `100B` quota 下只成功 3 个，最终用量 `90B`
- failpoint 模拟提交前失败后，`files`、`physical_files` 和磁盘均无残留
- 默认 smoke、专项测试和 storage consistency dry-run 都可在 Docker 容器内复现

## 面试官追问引导

### 面试官偏 C++ / Linux

重点说：

- 非阻塞 socket 和 epoll
- 主 Reactor accept，SubReactor 管连接
- 线程池处理业务，避免 Reactor 线程阻塞
- HTTP parser、chunked、multipart 流式上传
- 连接超时定时器和 keep-alive

引导句：

> 如果您更关注底层实现，我可以展开 Reactor、线程池和 HTTP parser；如果更关注后端工程，我可以展开文件一致性和并发控制。

### 面试官偏后端工程

重点说：

- controller / service / repository / infra 分层
- 数据库迁移
- 事务和行锁
- 幂等和补偿
- smoke、race、failpoint 测试

引导句：

> 这个项目比较有价值的地方是我没有只停在 CRUD，而是补了并发和失败路径验证。

### 面试官偏数据库

重点说：

- `files` 是事实来源
- trigger 维护 `ref_count`
- `FOR UPDATE` 锁用户行和物理文件行
- 唯一约束处理并发插入
- DB 和磁盘非事务边界用补偿

引导句：

> 这里我主要用 MySQL 事务保证库内一致性，用补偿脚本处理库外资源，也就是本地磁盘文件。

## 项目不足的答法

不要说“没有不足”。建议主动讲：

- 当前文件存储是本地磁盘，不适合多机部署；生产环境更适合对象存储
- DB 和磁盘无法原子提交，目前靠顺序约束和巡检补偿
- 还不支持断点续传和分片上传
- failpoint 现在通过环境变量控制，只适合测试环境
- 默认 smoke 复用测试库，长期运行会留下测试用户和操作日志

更好的收尾：

> 这些不足我没有回避，而是先用明确的工程边界和测试脚本兜住。下一步如果继续做，我会把本地磁盘替换成对象存储，并引入异步清理任务或 outbox 表。

## 可复现命令

默认 smoke：

```bash
docker compose exec -T backend ./scripts/run_smoke_suite.sh
```

并发上传/删除：

```bash
docker compose exec -T backend ./scripts/test_upload_race_consistency.sh
```

配额并发竞争：

```bash
docker compose exec -T backend ./scripts/test_upload_quota_race.sh
```

故障注入：

```bash
docker compose exec -T backend ./scripts/test_upload_failure_cleanup.sh
```

DB/磁盘巡检：

```bash
docker compose exec -T backend ./scripts/check_storage_consistency.sh --dry-run
```
