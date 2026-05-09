#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MIGRATIONS_DIR="${MIGRATIONS_DIR:-$REPO_ROOT/migrations}"

DB_HOST="${TWS_DB_HOST:-127.0.0.1}"
DB_PORT="${TWS_DB_PORT:-3306}"
DB_USER="${TWS_DB_USER:-root}"
DB_PASSWORD="${TWS_DB_PASSWORD:-}"
DB_NAME="${TWS_DB_NAME:-qgydb}"

if [ ! -d "$MIGRATIONS_DIR" ]; then
    echo "migrations directory not found: $MIGRATIONS_DIR" >&2
    exit 1
fi

MYSQL_PWD="$DB_PASSWORD"
export MYSQL_PWD

mysql_base() {
    mysql \
        --protocol=tcp \
        -h "$DB_HOST" \
        -P "$DB_PORT" \
        -u "$DB_USER" \
        --default-character-set=utf8mb4 \
        "$@"
}

sql_escape() {
    printf '%s' "$1" | sed "s/'/''/g"
}

echo "[migrate] host=$DB_HOST port=$DB_PORT db=$DB_NAME user=$DB_USER"
mysql_base -e "CREATE DATABASE IF NOT EXISTS \`$DB_NAME\` DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
mysql_base "$DB_NAME" -e "CREATE TABLE IF NOT EXISTS schema_migrations (version VARCHAR(128) NOT NULL, applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (version)) ENGINE=InnoDB"

found=0
for migration in "$MIGRATIONS_DIR"/*.sql; do
    [ -e "$migration" ] || continue
    found=1
    version="$(basename "$migration" .sql)"
    version_sql="$(sql_escape "$version")"
    applied="$(mysql_base "$DB_NAME" -N -s -e "SELECT COUNT(*) FROM schema_migrations WHERE version='$version_sql'")"
    if [ "$applied" = "1" ]; then
        echo "[migrate] skip $version"
        continue
    fi

    echo "[migrate] apply $version"
    mysql_base "$DB_NAME" < "$migration"
    mysql_base "$DB_NAME" -e "INSERT IGNORE INTO schema_migrations(version) VALUES('$version_sql')"
    echo "[migrate] done $version"
done

if [ "$found" = "0" ]; then
    echo "no migration files found in $MIGRATIONS_DIR" >&2
    exit 1
fi

echo "[migrate] all done"
