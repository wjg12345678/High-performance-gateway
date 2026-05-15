# Atlas WebServer 生产化加固与升级路线

这份文档不是重复介绍 Atlas 已经实现了什么，而是回答一个更难的问题：如果把当前项目从“面试项目 / 单机可运行系统”继续往真实网盘服务推进，应该先补什么，为什么补，怎么补，以及面试时怎样主动承认边界。

当前 Atlas 的价值在于：底层 WebServer 不是直接套框架，业务层也不是简单 CRUD，已经覆盖了账号会话、文件上传下载、目录、回收站、分享、操作审计、去重、配额、限流、迁移、smoke 和 benchmark。它适合作为 C++ Linux 后端项目展示工程能力。它当前不应该包装成完整生产级网盘，因为文件存储仍然依赖本地磁盘，DB 与磁盘没有分布式事务，多实例部署和后台任务体系还没有完全补齐。

面试时最稳的说法是：

```text
这个项目已经实现了网盘后端核心闭环，重点展示 C++ 网络编程、HTTP 协议处理、MySQL 事务、文件一致性和工程验证能力。当前定位是可完整演示的后端系统，不把它夸成生产级网盘。生产化还需要对象存储、多实例部署、异步清理、断点续传、Range 下载、权限协作和更完整的可观测体系。
```

## 1. 当前生产化能力盘点

| 方向 | 当前能力 | 生产化评价 |
| --- | --- | --- |
| 网络模型 | `epoll`、非阻塞 socket、主从 Reactor、线程池、连接超时、Keep-Alive | 能展示高并发服务端基础，但还缺全量线上连接指标和限流熔断联动 |
| HTTP 协议 | HTTP/1.1、JSON、form、chunked、multipart、HEAD/OPTIONS、文件下载响应 | 面试项目完整度较高，生产还需更严格的协议兼容性和安全头 |
| 认证会话 | PBKDF2 密码哈希、随机 token、Bearer Token、会话表、登出 | 可用，但生产建议存 token hash、增加设备管理、风控和 token 轮换 |
| 文件业务 | 上传、下载、目录、回收站、恢复、永久删除、公开分享 | 核心闭环已具备，生产还需分片、断点续传、Range、预览和权限协作 |
| 存储一致性 | 临时文件、SHA-256 去重、物理文件引用计数、失败清理、巡检脚本 | 设计方向正确，但本地磁盘和 DB 无法原子提交，需要补偿任务 |
| 数据库 | 版本化迁移、外键、唯一索引、trigger 维护 `ref_count` | 能支撑演示和测试，生产需备份、索引审计、慢 SQL、主从和恢复演练 |
| 限流 | 登录/注册维度接入外部 Redis-Limiter | 解耦合理，生产需统一规则中心和更完整的风控策略 |
| 测试 | 单测、smoke、并发上传、配额、分享、清理、benchmark | 面试加分明显，生产还需压力、混沌、兼容性和长稳测试 |

## 2. 第一优先级：本地磁盘改对象存储

当前最大生产化边界是本地磁盘。单机部署时，本地磁盘可以保存上传文件；一旦多实例部署，用户请求可能落到不同机器，本地文件就不可见。

典型问题：

```text
用户上传文件到 A 机器
DB 记录写入成功
下一次下载请求被负载均衡打到 B 机器
B 机器本地没有这个文件
下载失败
```

仅靠 Nginx sticky session 不能根治这个问题。sticky 只能让同一个用户尽量打到同一台机器，但机器宕机、扩容、迁移、后台任务、公开分享下载都会打破这个假设。真实网盘应该把文件对象放到共享存储层，例如 S3、MinIO、OSS、COS 或自建对象存储。

### 2.1 目标架构

```text
Atlas WebServer 多实例
   |
   +--> MySQL：文件元数据、用户、目录、分享、审计
   |
   +--> Redis-Limiter：登录/注册限流
   |
   +--> Object Storage：真实文件对象
```

数据库只记录对象 key、sha256、size、content_type、ref_count 等元数据；文件字节流放对象存储。这样任意 Atlas 实例都可以读写同一份物理对象。

### 2.2 建议新增抽象

当前已有 `infra/storage/storage.h` 和 `infra/storage/storage.cpp`，可以沿着这个方向扩展成稳定接口：

```cpp
class ObjectStorage {
public:
    virtual PutResult PutObject(const std::string& key,
                                const std::filesystem::path& local_tmp,
                                const PutOptions& options) = 0;
    virtual GetResult GetObject(const std::string& key,
                                const RangeRequest* range) = 0;
    virtual DeleteResult DeleteObject(const std::string& key) = 0;
    virtual HeadResult HeadObject(const std::string& key) = 0;
};
```

实现层可以先保留 `LocalStorage`，再新增 `S3Storage` 或 `MinioStorage`。业务层只依赖接口，不关心对象最终落在本地磁盘还是对象存储。

### 2.3 上传链路怎么改

当前上传思路是：先写临时文件，计算 SHA-256，再进入 DB 事务，最后移动到正式位置。

对象存储版本建议改成：

```text
1. 接收 multipart/chunked 请求，写入本地临时文件或流式写对象存储临时 key
2. 计算 sha256、size、mime
3. 开启 MySQL 事务
4. 锁用户行，检查配额
5. 查 physical_files 是否已有相同 sha256
6. 如果不存在，预留 physical_files 记录，状态为 pending
7. 提交或准备提交对象写入
8. 对象存储 put 成功后，将 physical_files 状态改为 active
9. 插入 files 逻辑文件记录
10. 失败路径写入 outbox，由后台任务补偿清理
```

这里最关键的是：不要假装 DB 和对象存储可以原子提交。它们天然不是同一个事务系统。正确做法是使用状态字段和补偿任务，把中间状态变成可恢复状态。

### 2.4 physical_files 状态扩展

建议把 `physical_files` 从单纯元数据表升级为带状态的对象表：

| 字段 | 含义 |
| --- | --- |
| `id` | 物理文件 id |
| `sha256` | 内容哈希 |
| `size` | 文件大小 |
| `storage_backend` | `local` / `s3` / `minio` |
| `object_key` | 对象存储 key |
| `ref_count` | 逻辑文件引用数 |
| `status` | `pending` / `active` / `deleting` / `delete_failed` |
| `created_at` | 创建时间 |
| `updated_at` | 更新时间 |

`pending` 记录可以被后台清理；`delete_failed` 记录可以重试删除；`deleting` 可以避免多个清理任务重复删同一个对象。

## 3. 第二优先级：DB 与文件的最终一致性补偿

当前永久删除流程已经采用了更合理的顺序：先提交 DB，再删磁盘。原因是 DB 是用户可见真相，不能先删物理文件后 DB 回滚，否则用户仍能看到文件记录但文件已经没了。

但这样会留下一个边界：

```text
DB 已删除最后一个 logical file
physical_files.ref_count 变为 0
commit 成功
磁盘删除失败
结果：DB 看不到文件，但磁盘上还有孤儿文件
```

这个问题不能靠“多写几个 if”解决，应该引入后台清理任务。

### 3.1 outbox 表

建议新增 `storage_outbox`：

| 字段 | 含义 |
| --- | --- |
| `id` | 任务 id |
| `task_type` | `delete_object` / `move_object` / `cleanup_tmp` |
| `object_key` | 要处理的对象 |
| `physical_id` | 关联物理文件 |
| `status` | `pending` / `running` / `done` / `failed` |
| `retry_count` | 重试次数 |
| `next_retry_at` | 下次重试时间 |
| `last_error` | 最近错误 |
| `created_at` | 创建时间 |
| `updated_at` | 更新时间 |

删除物理对象时，不直接依赖请求线程一次成功，而是在 DB 事务内写出 outbox 任务。后台 worker 执行任务，失败则指数退避重试。

### 3.2 巡检任务

除了 outbox，还需要定期巡检：

| 巡检项 | 检查逻辑 |
| --- | --- |
| DB 有记录但对象不存在 | 标记异常，阻止下载，进入人工或自动修复 |
| 对象存在但 DB 无 active 记录 | 如果超过保护期，加入删除任务 |
| `physical_files.ref_count` 异常 | 与 `files` 表实际引用重新对账 |
| tmp 文件过期 | 删除超过 TTL 的临时文件 |
| pending 对象卡住 | 根据状态和时间决定回滚或激活 |

当前项目已有 `scripts/check_storage_consistency.sh` 和相关专项测试，这个方向是对的。生产化要把脚本升级成可调度、可观测、可重试的后台任务。

## 4. 第三优先级：断点续传和分片上传

普通 multipart 上传适合小文件。真实网盘需要支持大文件、网络中断恢复和客户端并发上传，因此需要分片上传。

### 4.1 API 设计

建议拆成四个阶段：

```text
POST /api/drive/uploads/init
PUT  /api/drive/uploads/:upload_id/parts/:part_no
POST /api/drive/uploads/:upload_id/complete
DELETE /api/drive/uploads/:upload_id
```

`init` 返回 `upload_id`，并记录用户、目标目录、文件名、预期大小、预期 sha256、part_size、过期时间。

`upload part` 只上传某个分片，服务端保存分片 hash、size 和状态。这个接口天然需要幂等：同一个 `upload_id + part_no` 重复上传，如果内容 hash 相同就返回成功，如果 hash 不同就返回冲突。

`complete` 校验所有分片都存在，按顺序合并或调用对象存储 multipart complete，然后进入原来的去重、配额和逻辑文件创建流程。

### 4.2 表设计

建议新增：

```text
upload_sessions
  id
  user_id
  folder_id
  filename
  total_size
  expected_sha256
  part_size
  status
  expires_at

upload_parts
  upload_id
  part_no
  size
  sha256
  object_key
  status
```

需要唯一约束：

```sql
UNIQUE(upload_id, part_no)
```

这样可以保证同一个分片不会插入多条记录。

### 4.3 幂等和并发

`complete` 要防止两个客户端同时提交：

```text
SELECT upload_session FOR UPDATE
如果 status = completed，直接返回已经生成的 file_id
如果 status = uploading，切到 completing
校验 parts
合并
创建 files
status = completed
```

这类状态机比“请求来了就合并”更稳，因为客户端重试、浏览器刷新、代理重发都可能发生。

## 5. 第四优先级：秒传 API

项目已经有 SHA-256 去重和 `physical_files` 表，这是做秒传的基础。秒传不是凭空相信客户端，而是客户端先上报 hash，服务端检查自己是否已有物理文件。

建议 API：

```text
POST /api/drive/files/instant-upload
{
  "folder_id": 0,
  "filename": "demo.zip",
  "sha256": "...",
  "size": 123456
}
```

服务端逻辑：

```text
1. 鉴权
2. 校验 filename、folder、size
3. 锁用户行检查配额
4. 查 physical_files where sha256=? and size=? and status='active'
5. 如果不存在，返回 404 / need_upload
6. 如果存在，插入 files 逻辑记录
7. trigger 增加 ref_count
8. 返回文件元数据
```

注意：秒传不能只看 hash，不看 size；也不能信任客户端声称的 mime。真正下载展示时仍然要以服务端保存的元数据为准。

## 6. 第五优先级：Range 下载和大文件传输

当前下载可以返回完整文件。真实网盘通常需要 Range：

```http
Range: bytes=0-1048575
```

用于：

- 浏览器视频预览
- 断点下载
- 下载器多线程下载
- 大文件失败后恢复

服务端要支持：

| 状态码 | 场景 |
| --- | --- |
| `200 OK` | 没有 Range，返回完整文件 |
| `206 Partial Content` | Range 合法，返回部分内容 |
| `416 Range Not Satisfiable` | Range 越界 |

响应头要包含：

```http
Accept-Ranges: bytes
Content-Range: bytes start-end/total
Content-Length: part_size
ETag: "<sha256-or-version>"
```

如果接对象存储，可以把 Range 请求透传给对象存储，Atlas 只做鉴权、元数据检查和响应头适配。

## 7. 第六优先级：权限协作模型

当前项目支持用户自己的文件和公开分享。真实网盘还会有协作权限：

| 权限 | 能力 |
| --- | --- |
| owner | 所有操作 |
| editor | 上传、重命名、移动、删除到回收站 |
| viewer | 列表、下载、预览 |
| none | 无权限 |

建议新增：

```text
folder_members
file_permissions
share_links
share_access_logs
```

权限判断要统一放在 Service 层，不要散落在 Controller 中。Controller 只负责 HTTP 参数和响应，Service 负责“当前用户是否能对这个资源执行动作”。

## 8. 第七优先级：安全加固

生产环境必须补以下安全项：

| 项 | 当前情况 | 加固建议 |
| --- | --- | --- |
| token 存储 | 会话表保存 token | 改为保存 token hash，泄露 DB 后不能直接冒用 |
| HTTPS | 可选部署层处理 | 默认通过 Nginx / LB 终止 TLS |
| 文件名 | 已有基础校验 | 增加 Unicode normalization、保留字、路径穿越、长度限制 |
| 文件内容 | 当前不做扫描 | 接入 MIME sniff、病毒扫描、敏感内容检测 |
| 分享码 | 有 hash 设计 | 增加错误次数限制、访问审计、过期清理 |
| 公开文件 | 可公开下载 | 增加防盗链、下载限速、带宽保护 |
| API 限流 | 登录/注册限流 | 上传、分享、公开下载也要有限流和配额 |
| 审计 | 已有 operation_logs | 增加管理员查询、导出、保留期和脱敏 |

## 9. 第八优先级：可观测性

真实服务必须知道“现在是否正常”。建议指标分层：

### 9.1 HTTP 指标

- 请求总数
- 2xx / 4xx / 5xx 数量
- p50 / p95 / p99 延迟
- 当前连接数
- keep-alive 复用率
- 上传请求体大小分布
- 下载响应大小分布

### 9.2 业务指标

- 注册成功 / 失败
- 登录成功 / 失败 / 限流
- 上传成功 / 失败 / 平均大小
- 下载成功 / 失败
- 分享创建 / 访问 / 失败
- 回收站删除 / 恢复 / 永久删除
- 配额不足次数

### 9.3 存储一致性指标

- orphan object 数量
- missing object 数量
- pending physical file 数量
- outbox pending / failed 数量
- 临时文件过期数量
- `ref_count` 对账失败数量

### 9.4 数据库指标

- 连接池使用率
- 慢 SQL 数量
- 事务回滚数量
- 死锁数量
- 迁移版本

## 10. 多实例部署方案

生产部署推荐：

```text
                    +------------------+
Client -> Nginx/LB -> Atlas instance 1 |
                    | Atlas instance 2 | -> MySQL primary/replica
                    | Atlas instance 3 | -> Redis / Redis-Limiter
                    +------------------+ -> Object Storage
```

Atlas 实例应尽量无状态：

| 状态 | 放哪里 |
| --- | --- |
| 用户会话 | MySQL 或 Redis |
| 限流状态 | Redis |
| 文件元数据 | MySQL |
| 文件对象 | 对象存储 |
| 本地临时文件 | 短 TTL，只作为上传缓冲 |
| 后台任务状态 | MySQL outbox |

本地磁盘只保留临时文件和日志，不保存用户最终数据。这样实例重启、扩容、缩容都不会影响用户文件。

## 11. 迭代路线

### 11.1 一周内最值得补

| 优先级 | 任务 | 原因 |
| --- | --- | --- |
| P0 | 文档明确生产边界 | 避免面试被问“是不是生产级”时被动 |
| P0 | outbox 表和清理任务设计 | 解决 DB/磁盘非原子这个核心追问 |
| P1 | Range 下载 | 实现成本可控，网盘特征明显 |
| P1 | 秒传 API | 已有 SHA-256 去重基础，扩展自然 |
| P2 | token hash 存储 | 安全加分明显 |

### 11.2 两到三周内可做

| 任务 | 结果 |
| --- | --- |
| 对象存储接口抽象 | 从本地磁盘平滑迁移到 MinIO/S3 |
| 分片上传 | 支持大文件和断点续传 |
| 后台 worker | 清理临时文件、失败删除重试、巡检 |
| Prometheus 指标 | 可观测性从脚本升级为服务指标 |
| 权限协作第一版 | 支持 folder viewer/editor |

### 11.3 长期路线

| 方向 | 内容 |
| --- | --- |
| 高可用 | MySQL 主从、Redis Sentinel/Cluster、对象存储多副本 |
| 安全 | 病毒扫描、内容审核、下载风控、管理员审计 |
| 性能 | 零拷贝下载、对象存储直传、CDN、缓存元数据 |
| 产品 | 文件预览、在线文档、协作空间、版本历史 |
| 运维 | 灰度发布、备份恢复、容量报表、租户隔离 |

## 12. 面试表达边界

被问“这个项目生产能用吗”，不要回答“能”。更好的回答是：

```text
当前可以作为一个完整后端项目演示核心功能和关键工程问题，但我不会把它说成生产级网盘。它已经覆盖了 WebServer、上传下载、去重、配额、分享、审计和限流，但本地磁盘、多实例、DB 与文件非原子、后台异步任务、断点续传、Range 下载和对象存储这些是明确的生产化边界。我的升级顺序会先做对象存储抽象和 outbox 补偿，再做分片上传、Range 下载和可观测性。
```

被问“机器挂了怎么办”，可以答：

```text
当前单机本地磁盘版本下，如果机器挂了，服务不可用，未复制的本地文件也有风险。这正是我不会把它包装成生产级的原因。生产方案会把 Atlas 实例做成无状态，文件进入对象存储，元数据进入 MySQL，任务状态进入 outbox，实例挂掉后其他实例可以继续读写同一份对象数据。
```

被问“磁盘删失败怎么办”，可以答：

```text
不能为了磁盘删除成功而先删文件再提交 DB，因为那会导致用户还能看到记录但文件已经没了。正确顺序是先让 DB 进入用户可见的一致状态，再把物理删除做成可重试后台任务。失败不会影响用户逻辑视图，只会产生可观测、可补偿的孤儿对象。
```

## 13. 不建议做的方向

| 不建议 | 原因 |
| --- | --- |
| 立刻做复杂前端功能 | 当前面试重点在后端网络、存储一致性和工程验证 |
| 宣称完全生产级 | 容易被多实例、灾备、对象存储、任务补偿追问击穿 |
| 把所有功能都堆进 Controller | 会破坏现有分层，后续权限和一致性更难维护 |
| 盲目加微服务 | 当前体量下先把存储、任务、可观测做好更重要 |
| 只优化 QPS | 网盘项目更关键的是一致性、失败恢复和大文件链路 |

## 14. 最终结论

Atlas 后续最值得补的不是“再加几个 CRUD 接口”，而是把真实网盘最容易被质疑的工程边界补清楚：

- 多实例下文件怎么共享
- DB 成功但对象操作失败怎么补偿
- 大文件上传中断怎么恢复
- 下载如何支持 Range
- 秒传如何基于现有 SHA-256 去重自然扩展
- 后台清理和巡检如何可观测
- 安全、限流和审计如何覆盖高风险接口

如果时间有限，先做对象存储抽象、outbox 清理任务、Range 下载和秒传 API。这几项最能把项目从“功能闭环”推向“工程闭环”。
