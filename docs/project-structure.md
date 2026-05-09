# Project Structure

当前目录按“启动层 / HTTP 适配 / 业务服务 / 数据访问 / 基础设施 / 静态资源”划分。

| 目录 | 职责 |
| --- | --- |
| `app/` | 程序入口、配置解析、`WebServer` 初始化、主 Reactor 和 SubReactor 编排 |
| `http/` | HTTP 协议解析、路由分发、响应组装、静态资源和文件接口适配 |
| `service/` | 认证、会话、文件管理、分享等业务编排 |
| `repo/mysql/` | MySQL Repository，集中管理 SQL 和结果映射 |
| `infra/db/` | MySQL 连接池 |
| `infra/storage/` | 上传目录、临时目录、落盘、文件移动、SHA-256 计算 |
| `infra/threadpool/` | 业务任务线程池与队列实现 |
| `infra/timer/` | 连接超时最小堆定时器 |
| `infra/log/` | 同步/异步日志实现 |
| `infra/lock/` | 互斥锁、信号量、条件变量封装 |
| `webroot/` | 静态页面、样式、媒体资源和上传目录 |
| `scripts/` | 冒烟测试、API 测试、benchmark、perf 脚本 |
| `tests/` | C++ 单元测试 |
| `docker/` | Docker 辅助文件和 MySQL 初始化 SQL |
| `deploy/` | Nginx 等部署示例 |

## 约定

- 新业务逻辑优先放在 `service/`，不要直接堆进 `http/core/`。
- 新 SQL 优先放在 `repo/mysql/`，HTTP 层不要拼接业务 SQL。
- 新基础组件放进 `infra/` 的对应子目录。
- 运行产物放在 `.gitignore` 已忽略的位置，例如 `reports/`、`ServerLog/`、`webroot/uploads/`。
