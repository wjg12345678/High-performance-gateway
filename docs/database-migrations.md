# 数据库迁移

本项目使用 `migrations/` 下的版本化 SQL 管理表结构，应用启动时只做 schema 可用性检查，不再执行 `CREATE TABLE` / `ALTER TABLE`。

## 迁移入口

本地 MySQL：

```bash
TWS_DB_HOST=127.0.0.1 \
TWS_DB_PORT=3306 \
TWS_DB_USER=root \
TWS_DB_PASSWORD=your_password \
TWS_DB_NAME=qgydb \
./scripts/migrate_db.sh
```

Docker Compose 暴露到宿主机的 MySQL 端口是 `3307`：

```bash
TWS_DB_HOST=127.0.0.1 \
TWS_DB_PORT=3307 \
TWS_DB_USER=root \
TWS_DB_PASSWORD=root \
TWS_DB_NAME=qgydb \
./scripts/migrate_db.sh
```

`docker compose up -d --build` 启动 Web 容器时也会先运行该脚本，成功后再启动 `./build/server`。

## 目录约定

- `migrations/001_init_schema.sql`：完整初始化 schema，用于空库或补齐基础表。
- `migrations/002_upgrade_existing_drive_dedup.sql`：把旧文件表升级到网盘目录和 SHA-256 去重模型。
- `schema_migrations`：记录已执行版本，迁移脚本会跳过已经应用的 SQL。
- `docker/mysql/init.sql`：Docker 新 MySQL 数据卷的初始化脚本，包含最新完整 schema，并预标记已应用迁移。

## 开发约定

1. 新增表、列、索引时，新建递增编号 SQL，例如 `003_add_xxx.sql`。
2. SQL 尽量写成可重复执行的安全形式，已存在对象要跳过。
3. 迁移成功后由 `scripts/migrate_db.sh` 写入 `schema_migrations`。
4. 不要把业务 schema 变更重新塞回 `app/webserver.cpp` 的启动流程。

如果服务日志出现 `database schema is not migrated`，先执行迁移脚本，再重启服务。
