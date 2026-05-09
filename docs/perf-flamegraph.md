# perf + FlameGraph

## 采样入口

仓库内已经提供了一键脚本：

```bash
chmod +x scripts/profile_perf_flamegraph.sh
./scripts/profile_perf_flamegraph.sh
```

默认会：

- 用 `docker-compose.yml + docker-compose.perf.yml` 启动带 `perf` 能力的 `web` 服务
- 对容器内 `server` 进程执行 `perf record`
- 同时用 `wrk` 压测默认目标 `GET /healthz`
- 生成 `perf.data`、折叠栈和 `flamegraph.svg`

默认输出目录：

```text
reports/perf/<timestamp>/
```

常用参数示例：

```bash
TARGET_URL=http://127.0.0.1:9006/api/private/ping \
LUA_SCRIPT=./test_pressure/private_ping.lua \
CONNECTIONS=300 \
THREADS=4 \
DURATION=20s \
SAMPLE_FREQ=199 \
./scripts/profile_perf_flamegraph.sh
```

建议至少分别对这些场景单独采样一次，而不是只看 `GET /healthz`：

- 健康检查接口
- 首页静态资源
- 登录接口
- 私有文件列表接口

## 数据分析

一次完整的采样结果里，最值得先看的不是 SVG 本身，而是“吞吐、延迟、CPU 热点、等待热点”是否一致。

产物目录中的关键文件：

- `wrk.txt`：压测原始结果，用来确认 QPS、延迟分布、超时或错误情况
- `perf-record.txt`：确认 `perf` 是否真正采到了样本，是否有权限或符号相关警告
- `perf.data`：原始采样数据，后续所有分析都从这里展开
- `perf.unfolded`：展开后的调用栈，便于检查某个热点路径是否合理
- `perf.folded`：火焰图输入，适合快速聚合比较热点占比
- `flamegraph.svg`：最终可视化结果，适合找“最宽的栈”和主调用路径

读数据时建议按这个顺序：

1. 先看 `wrk.txt`，确认当前压测到底是在“CPU 打满”还是“延迟升高但 CPU 不忙”。
2. 再看 `flamegraph.svg`，确认热点是集中在用户态业务逻辑、系统调用，还是线程等待。
3. 最后回到 `perf.unfolded` 或 `perf-record.txt`，验证热点路径是否稳定，避免被一次偶发采样误导。

如果 `wrk` 显示吞吐不高、延迟却明显上升，而火焰图顶部大面积是等待函数，那么问题通常不在“某个纯计算函数太慢”，而在锁竞争、I/O 等待、事件循环空转或下游依赖。

## 结果分析

火焰图里横向越宽，表示累计 CPU 时间越高，优先级通常也越高。但“宽”不等于“一定要优化”，要先判断它是有效工作还是被动等待。

### 1. 热点集中在 `epoll_wait`、`futex`

这类图形通常说明线程大量时间花在等待，而不是执行繁重计算。

常见解释：

- `epoll_wait` 宽：请求量不足、连接空闲较多，或者主线程在等事件
- `futex` 宽：锁竞争、条件变量等待、线程同步开销较大

这类结果不要直接做微优化，应先回答两个问题：

- 当前瓶颈是不是 CPU？如果不是，继续抠函数级 CPU 优化收益很低
- 等待是正常现象还是异常现象？例如健康检查接口本来就可能出现较多等待，不代表程序有问题

### 2. 热点集中在 HTTP 解析、路由分发

这通常说明服务端已经进入请求处理热路径，CPU 确实在忙业务前半段。

应重点检查：

- 请求头和请求行解析是否存在重复扫描
- 路由匹配是否有不必要的字符串比较或拷贝
- 小对象分配是否过于频繁

如果 `GET /healthz` 这类轻接口都在解析阶段出现明显热点，说明基础请求处理链路还有压缩空间。

### 3. 热点集中在 JSON 拼装、字符串处理

这类热点往往意味着响应构造成本偏高，尤其在列表接口、登录返回体、错误响应拼装中常见。

优先关注：

- 是否重复进行字符串拼接
- 是否有可避免的序列化中间态
- 是否因为日志、错误信息、路径拼接引入额外分配

如果火焰图显示热点分散在多个小型字符串函数里，通常说明问题不是单个“大函数”，而是整体对象生命周期和内存分配策略不够紧凑。

### 4. 热点集中在文件 I/O 或 MySQL 调用

这说明瓶颈已经离开纯 Web 层，进入静态资源访问或数据库交互。

应结合接口类型判断：

- 首页静态资源热点高：优先检查文件读取、缓存命中、sendfile/零拷贝路径
- 登录接口热点高：优先检查鉴权逻辑、数据库索引、查询次数
- 私有文件列表接口热点高：优先检查分页、目录遍历、SQL 结果集大小、JSON 输出成本

如果图上用户态业务函数不宽，但系统调用或数据库相关路径很宽，下一步更应该补充 I/O 与 SQL 侧指标，而不是继续只看 FlameGraph。

## 改进方向

看到热点后，建议按“先确认瓶颈类别，再做针对性改进”的顺序推进。

### CPU 型热点

适用于热点集中在解析、路由、序列化、字符串处理等用户态函数：

- 减少重复解析和重复拷贝
- 合并小块字符串拼装
- 降低临时对象与内存分配次数
- 复用缓冲区，缩短热点路径中的对象生命周期

这类优化最适合通过“同一接口、同一参数、前后两次 FlameGraph 对比”来验证。

### 等待型热点

适用于热点集中在 `epoll_wait`、`futex`、锁等待、条件变量等待：

- 缩小锁粒度
- 减少共享状态
- 检查线程模型是否引入多余同步
- 区分“正常空闲等待”和“异常竞争等待”

如果等待型热点明显，应该补充并发模型和锁竞争分析，而不是继续只提高 `perf record -F` 采样频率。

### I/O 型热点

适用于静态文件、磁盘访问、数据库交互相关路径：

- 为静态资源增加缓存或更直接的发送路径
- 减少阻塞式文件读取
- 检查 SQL 是否存在重复查询、全表扫描或返回过大结果集
- 将 FlameGraph 与数据库慢查询、接口耗时拆分一起看

这类问题通常需要“接口级 tracing + perf”联合判断，单靠火焰图只能看到 CPU 时间，无法完整解释等待来源。

## 示例火焰图

运行采样脚本后，结果默认生成到 `reports/perf/<timestamp>/`。仓库不再保留历史采样产物，避免把大体积 perf 数据和图片提交进源码树。

- 交互式 SVG：`reports/perf/<timestamp>/flamegraph.svg`
- 原始采样数据：`reports/perf/<timestamp>/perf.data`

GitHub 对带脚本的交互式 SVG 预览支持有限；如果要缩放、搜索和点击查看完整栈细节，请直接打开生成目录中的 `flamegraph.svg`。

## 常见问题

### 1. `perf_event_open` 权限错误

如果 Docker Desktop 的 Linux VM 对性能计数器限制较严，`perf` 可能仍然失败。当前仓库已在 `docker-compose.perf.yml` 中开启：

- `privileged: true`
- `cap_add: PERFMON/SYS_ADMIN/SYS_PTRACE`
- `seccomp:unconfined`

如果仍失败，通常是 Docker Desktop 内核能力限制，不是项目代码问题。

### 2. `perf not found for kernel ... linuxkit`

Docker Desktop 常见的 Linux 内核版本带有 `linuxkit` 后缀。Ubuntu 自带的 `perf` wrapper 会按当前内核版本查找完全匹配的工具包。仓库脚本已经绕过这个 wrapper，直接定位 `/usr/lib/linux-tools/.../perf` 真实二进制执行采样。

### 3. 火焰图没有业务符号

当前 `makefile` 在 `DEBUG=1` 时使用 `-g` 编译，默认足够支持符号解析。如果切到 `DEBUG=0`，火焰图可读性会明显变差。

### 4. 为什么不用 macOS 自带采样器

因为这个服务依赖 Linux 的 `epoll` 与容器运行方式，`perf` 更接近目标部署环境；如果只是想看 macOS 本地 CPU 热点，才考虑 `Instruments` / `xctrace`。
