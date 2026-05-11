# 请求时序图

这份文档专门描述一次请求是如何从进入监听 socket、到完成鉴权、业务处理、数据库操作、响应返回的。

## 通用请求时序

```mermaid
sequenceDiagram
    participant C as Client
    participant MR as Main Reactor
    participant SR as SubReactor
    participant TP as Thread Pool
    participant HC as http_conn
    participant DB as MySQL

    C->>MR: TCP connect
    MR->>MR: accept()
    MR->>SR: dispatch(connfd)
    SR->>SR: register_connection + refresh_timer
    C->>SR: HTTP request
    SR->>HC: read_once()
    SR->>TP: append_p(http_conn*)
    TP->>HC: process()
    HC->>HC: parse_request_line / headers / body
    HC->>HC: middleware_request_log
    HC->>HC: middleware_auth
    HC->>DB: query if needed
    DB-->>HC: result
    HC->>HC: route_request + process_write
    TP-->>SR: response ready
    SR->>C: write()/sendfile()/SSL_write()
```

## 登录请求时序

```mermaid
sequenceDiagram
    participant C as Client
    participant SR as SubReactor
    participant TP as Thread Pool
    participant HC as http_conn
    participant DB as MySQL

    C->>SR: POST /api/login
    SR->>TP: append_p()
    TP->>HC: process()
    HC->>HC: parse_post_body()
    HC->>DB: SELECT password_hash FROM users
    DB-->>HC: users row
    HC->>HC: verify_user_password(PBKDF2)
    HC->>HC: make_session_token()
    HC->>DB: INSERT/UPDATE user_sessions(user_id)
    HC->>DB: DELETE old user_sessions for same user_id
    HC->>DB: INSERT operation_logs(login)
    HC-->>SR: JSON response with token
    SR-->>C: 200 OK
```

## 文件上传请求时序

```mermaid
sequenceDiagram
    participant C as Client
    participant SR as SubReactor
    participant TP as Thread Pool
    participant HC as http_conn
    participant FS as webroot/uploads
    participant DB as MySQL

    C->>SR: POST /api/drive/files/upload
    Note over C,SR: Authorization + JSON body
    SR->>TP: append_p()
    TP->>HC: process()
    HC->>HC: parse_post_body()
    HC->>HC: middleware_auth()
    HC->>DB: lookup_session(token)
    DB-->>HC: username
    HC->>FS: write file content
    HC->>DB: INSERT files
    HC->>DB: INSERT operation_logs(upload)
    HC-->>SR: JSON response(file id)
    SR-->>C: 200 OK
```

## 文件下载请求时序

```mermaid
sequenceDiagram
    participant C as Client
    participant SR as SubReactor
    participant TP as Thread Pool
    participant HC as http_conn
    participant DB as MySQL
    participant FS as webroot/uploads

    C->>SR: GET /api/drive/files/:id/download
    SR->>TP: append_p()
    TP->>HC: process()
    HC->>HC: middleware_auth()
    HC->>DB: lookup_session(token)
    DB-->>HC: username
    HC->>DB: SELECT file metadata
    DB-->>HC: owner + stored_name
    HC->>HC: owner check
    HC->>FS: open(file)
    HC->>DB: INSERT operation_logs(download)
    TP-->>SR: FILE_REQUEST
    SR-->>C: response headers + file body
```

## 超时连接回收时序

```mermaid
sequenceDiagram
    participant SR as SubReactor
    participant HT as HeapTimer
    participant HC as http_conn

    loop periodic scan
        SR->>HT: scan_timeout()
        HT->>HC: check last_active
        alt expired
            HC->>HC: close_conn()
        else active
            HT->>HT: add_or_update()
        end
    end
```

## 关键说明

- 主 Reactor 只负责接入，不承担业务执行，避免监听线程被耗时逻辑阻塞。
- SubReactor 负责连接级读写事件和超时管理，线程池负责业务解析和数据库访问。
- `http_conn` 是请求处理核心，串联了解析、鉴权、路由、数据库操作和响应拼装。
- 文件服务按“文件内容落磁盘、元数据和审计落 MySQL”的方式拆分。
- 私有接口统一走 `middleware_auth()`，token 先查带过期时间的内存缓存，再回落到 `user_sessions`。
- session 接近过期时会自动刷新，`logout` 支持当前 token 或当前用户全会话失效。
