# 项目最终复盘

本文档用于收尾阶段复盘：把项目已经解决的工程问题、可复现验证入口、剩余边界和清洁项集中放在一个地方，方便面试前快速过一遍。

## 当前定位

Atlas WebServer 当前不是单纯的教学 WebServer，而是一个 C++ Linux 网盘后端：

- 底层保留 `epoll`、非阻塞 socket、主从 Reactor、线程池、HTTP/1.1 parser、chunked/multipart 解析、MySQL C API 和可选 HTTPS。
- 上层提供注册登录、Bearer Token 会话、网盘文件、目录、回收站、公开分享、上传配额、操作审计、数据库迁移和 Docker Compose 部署。
- 前端已拆到兄弟目录 `../Atlas-Frontend`，后端仓库只保留 API、上传存储和部署示例。

## 已解决的核心工程问题

| 问题 | 原风险 | 当前方案 | 验证入口 |
| --- | --- | --- | --- |
| HTTP 与业务耦合 | 业务逻辑堆在连接对象里，controller 难测试、难扩展 | 引入 `HttpRequest`、`HttpResponse`、`RequestContext`，请求流收敛为 `HttpConnection -> Router -> Controller -> Service -> Repository` | 编译、单测、smoke |
| 文件引用计数漂移 | 应用层手动增减 `ref_count`，异常和并发路径容易不一致 | `files` 作为事实来源，MySQL trigger 维护 `physical_files.ref_count` | `scripts/test_ref_count_consistency.sh` |
| 并发上传同内容 | 多请求可能插入重复物理文件或引用计数异常 | SHA-256 唯一约束、事务、物理文件行锁和唯一冲突回查 | `scripts/test_upload_race_consistency.sh` |
| 上传配额并发穿透 | 多个并发请求都看到剩余额度，最终超额 | 上传事务内锁用户行，重新计算已用容量 | `scripts/test_upload_quota_race.sh` |
| DB 与磁盘非事务一致性 | DB 回滚/提交和磁盘文件移动删除不能原子化 | commit 后删除磁盘，失败时由 storage checker 兜底发现/清理 orphan | `scripts/check_storage_consistency.sh --dry-run` |
| 上传失败半成品 | 文件已落盘但事务提交前失败，可能残留 DB 或磁盘垃圾 | failpoint 验证失败路径回滚和临时文件清理 | `scripts/test_upload_failure_cleanup.sh` |
| 退出后旧页面体验 | 浏览器返回、bfcache、多标签可能保留旧页面 | 受保护页面在 mounted、`pageshow`、`focus`、`visibilitychange` 检查本地 token，私有 API 401/403 兜底跳登录 | 前端本机预览 |

## 默认验证入口

后端默认回归：

```bash
docker compose exec -T backend ./scripts/run_smoke_suite.sh
```

当前默认 smoke 覆盖：

- auth
- private-api
- files
- drive
- ref-count
- upload-race
- storage-consistency
- share
- share-race
- chunked-api

后端本地构建和单测：

```bash
BUILD_DIR=build-assess ./scripts/run_unit_tests.sh
cmake --build build-assess --target server atlas-unit-tests --parallel 2
```

专项验证：

```bash
docker compose exec -T backend ./scripts/test_upload_quota_race.sh
docker compose exec -T backend ./scripts/test_upload_failure_cleanup.sh
docker compose exec -T backend ./scripts/check_storage_consistency.sh --dry-run
```

前端本机预览：

```bash
cd ../Atlas-Frontend
VITE_DEV_API_TARGET=http://127.0.0.1 npm run dev -- --host 0.0.0.0
```

前端构建：

```bash
cd ../Atlas-Frontend
npm run build
```

格式检查：

```bash
./scripts/format_check.sh check
```

当前环境缺少 `clang-format` 时，该命令会失败；这是工具链缺失，不代表构建或业务测试失败。

## 最终验收记录

验收时间：2026-05-10

| 项目 | 命令 | 结果 |
| --- | --- | --- |
| 后端本地构建 | `BUILD_DIR=build-final ./build.sh` | 通过；仅有 `write/getcwd` 返回值未使用 warning |
| 后端单测 | `BUILD_DIR=build-final-test ./scripts/run_unit_tests.sh` | 通过；`parser_chunked` 1/1 passed |
| 后端容器冒烟 | `docker compose exec -T backend ./scripts/run_smoke_suite.sh` | 通过；覆盖 auth、private-api、files、drive、ref-count、upload-race、storage-consistency、share、share-race、chunked-api |
| 前端构建 | `npm run build` | 通过；生成 `/files`、`/welcome`、`/login`、`/register`、`/share` 等页面产物 |
| 公网健康检查 | `curl -I -sS --max-time 5 http://111.230.49.177/healthz` | 通过；HTTP 200 |
| 公网页面检查 | `curl -I -sS --max-time 5 http://111.230.49.177/welcome`、`curl -I -sS --max-time 5 http://111.230.49.177/files` | 通过；均为 HTTP 200 |
| 公网前端产物检查 | `curl -sS --max-time 5 http://111.230.49.177/files`，再检查 `/assets/files-BYyh94dI.css` | 通过；线上 `/files` 已包含 `console-layout`、`workspace-head`、`view-tabs`、`card-actions` |
| 格式检查 | `./scripts/format_check.sh check` | 未通过；当前环境缺少 `clang-format`，属于工具链缺口 |

补充验收时间：2026-05-11

| 项目 | 命令 | 结果 |
| --- | --- | --- |
| 后端单测 | `BUILD_DIR=build-assess ./scripts/run_unit_tests.sh` | 通过；7/7 passed |
| 后端构建 | `cmake --build build-assess --target server atlas-unit-tests --parallel 2` | 通过 |
| 容器构建 | `docker compose up -d --build backend` | 通过；backend / mysql 均 healthy |
| 上传专项 | `test_files.sh`、`test_drive.sh`、`test_upload_race_consistency.sh`、`test_upload_quota_race.sh`、`test_upload_failure_cleanup.sh` | 通过 |
| 后端完整冒烟 | `docker compose exec -T backend ./scripts/run_smoke_suite.sh` | 通过；10/10 |

## 面试讲法

一段话版本：

> 这个项目最有价值的不是简单 CRUD，而是我把一个传统 C++ WebServer 演进成了网盘后端，并补上了工程边界。HTTP 层从 `HttpConnection` 中拆出 request / response / context 模型，业务按 controller、service、repository 分层。文件模块用 SHA-256 做物理文件去重，`files` 表作为事实来源，`physical_files.ref_count` 由 MySQL trigger 维护；上传和永久删除路径用事务、用户行锁和物理文件行锁保护并发一致性。DB 和磁盘不是同一个事务资源，所以我把磁盘删除放在 DB commit 后，并补 storage checker、并发上传、quota race 和 failpoint 脚本验证边界。

## 剩余边界

- `service/files/file_service.cpp` 已拆出 `upload_service` 和 `share_service`；后续可继续把目录、回收站、清理补偿拆成更小服务。
- DB commit 后磁盘删除失败目前由巡检脚本补偿，后续可演进为 outbox / cleanup job。
- controller/service 级单元测试还可以继续补，当前主要依赖 API smoke 和专项脚本。
- `init.sql` 和 `migrations/` 仍需要继续保持同步；破坏性开发重置已从自动迁移链路拆到 `scripts/dev_reset_schema.sql`。
- `HttpConnection` 已完成第一阶段解耦，后续如果继续瘦身，可再拆 parser state、response writer 和资源 RAII。

## 仓库清洁清单

提交前建议确认：

- `build/`、`build-*/`、`dist/`、`node_modules/` 是生成产物，不应进入提交。
- `webroot/uploads/` 只保留 `.gitkeep`，不要提交上传文件。
- 前端正式产物通过 Docker build 生成，不需要把 `../Atlas-Frontend/dist/` 提交到后端仓库。
- 如果需要清理本机生成目录，先确认没有正在运行的本地服务依赖这些目录。
