# Release Notes

## v1.1 Benchmark & Documentation Refresh

本版本聚焦基准测试体系收敛、仓库文档规范化和交付材料整理，重点提升可复现性、可维护性和信息一致性。

### Highlights

- 重建 benchmark 方案，形成覆盖轻接口、静态资源、鉴权接口、文件查询和上传写路径的分层压测矩阵
- 新增统一 benchmark runner，固定并发、线程数、时长、请求体大小和容器资源采样方式
- 补充 `1000` 并发读场景样本，并加入同步/异步日志模式对比
- 重写仓库首页与性能文档，统一为工程交付口径
- 清理压测产物管理方式，降低临时文件误提交风险

### Benchmark Summary

- `GET /healthz` 在当前发布配置下可达到 `7.5k req/s` 量级
- `GET /api/private/ping` 在 Bearer Token 鉴权链路下可达到 `7.4k req/s` 量级
- `GET /api/drive/items` 在高并发下明显受 MySQL 影响，`500~1000` 并发区间开始出现稳定 `read/timeout` 错误
- `POST /api/login` 与 `POST /api/drive/files/upload` 仍是当前最重的两条写路径
- 在当前测试环境中，`TWS_LOG_WRITE=0` 明显优于默认异步日志模式

### Documentation Changes

- [README.md](README.md) 重组为产品化工程首页
- [docs/benchmark.md](docs/benchmark.md) 收敛为标准性能报告
- [docs/architecture.md](docs/architecture.md) 和 [docs/request-sequence.md](docs/request-sequence.md) 统一为运行时说明文档
- [docs/file-module.md](docs/file-module.md) 补齐接口边界、权限模型和存储说明

### Added Files

- `scripts/run_benchmark_suite.sh`
- `test_pressure/login.lua`
- `test_pressure/drive_upload.lua`

### Updated Files

- `README.md`
- `docs/benchmark.md`
- `docs/benchmark.csv`
- `docker-compose.yml`
- `.gitignore`

## v1.0 Service Baseline

本版本形成了 Atlas WebServer 的首个完整工程基线，覆盖服务启动、鉴权、文件业务、容器部署、验证脚本和基础性能材料。

### Core Changes

- 完成 HTTP 层职责拆分，降低单文件复杂度
- 形成 `Main-Reactor + Multi-SubReactor + Dynamic Thread Pool` 并发处理模型
- 补齐注册、登录、Token 鉴权、文件上传下载、权限控制、操作日志
- 增加统一页面资源和基础业务入口
- 支持 Docker Compose 启动、健康检查、MySQL 持久化
- 新增 smoke test、压测脚本、压测图表、架构文档和时序文档
- 完成配置体系收敛：默认读取 `server.conf`，支持环境变量和命令行覆盖

### Scope

- 当前主路径面向小文件稳定上传，不覆盖超大文件传输
- 更细粒度权限模型、监控告警和 CI/CD 仍属于后续演进项
