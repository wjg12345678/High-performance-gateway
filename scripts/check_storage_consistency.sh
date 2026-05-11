#!/bin/sh

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

DB_HOST="${TWS_DB_HOST:-127.0.0.1}"
DB_PORT="${TWS_DB_PORT:-3306}"
DB_USER="${TWS_DB_USER:-root}"
DB_PASSWORD="${TWS_DB_PASSWORD:-}"
DB_NAME="${TWS_DB_NAME:-qgydb}"
UPLOAD_ROOT="${UPLOAD_ROOT:-$SCRIPT_DIR/../webroot/uploads}"

MODE="check"
QUIET=0

usage() {
    cat <<EOF
Usage: $0 [--dry-run] [--fix-orphans] [--quiet]

Checks consistency between MySQL file metadata and webroot/uploads.

Options:
  --dry-run       Report inconsistencies without deleting anything. This is the default.
  --fix-orphans   Delete unreferenced physical_files rows and disk files with no files reference.
                  Refuses to run when referenced DB files are missing on disk or ref_count is wrong.
  --quiet         Only print the final summary.
  -h, --help      Show this help.

Environment:
  TWS_DB_HOST, TWS_DB_PORT, TWS_DB_USER, TWS_DB_PASSWORD, TWS_DB_NAME
  UPLOAD_ROOT
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run)
            MODE="check"
            ;;
        --fix-orphans)
            MODE="fix"
            ;;
        --quiet)
            QUIET=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

fail() {
    echo "$1" >&2
    exit 1
}

if ! command -v mysql >/dev/null 2>&1; then
    fail "mysql client is required"
fi

if ! command -v comm >/dev/null 2>&1; then
    fail "comm is required"
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
        --batch \
        --raw \
        --skip-column-names \
        "$DB_NAME" \
        "$@"
}

mysql_query() {
    mysql_base -e "$1"
}

if ! mysql_base -e "SELECT 1" >/dev/null 2>&1; then
    fail "cannot connect to MySQL host=$DB_HOST port=$DB_PORT db=$DB_NAME user=$DB_USER"
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT INT TERM

DB_PHYSICAL="$TMP_DIR/db_physical"
DB_REFERENCED="$TMP_DIR/db_referenced"
DISK_FILES="$TMP_DIR/disk_files"
REF_MISMATCH="$TMP_DIR/ref_mismatch"
STALE_PHYSICAL="$TMP_DIR/stale_physical"
MISSING_REFERENCED="$TMP_DIR/missing_referenced"
UNREFERENCED_DISK="$TMP_DIR/unreferenced_disk"

snapshot() {
    mysql_query "SELECT stored_name FROM physical_files WHERE stored_name<>'' ORDER BY stored_name" \
        | sort -u > "$DB_PHYSICAL"

    mysql_query "SELECT DISTINCT p.stored_name FROM physical_files p JOIN files f ON f.physical_id=p.id WHERE p.stored_name<>'' ORDER BY p.stored_name" \
        | sort -u > "$DB_REFERENCED"

    mysql_query "SELECT CONCAT_WS('\t', p.id, p.stored_name, p.ref_count, COUNT(f.id)) FROM physical_files p LEFT JOIN files f ON f.physical_id=p.id GROUP BY p.id, p.stored_name, p.ref_count HAVING p.ref_count<>COUNT(f.id) ORDER BY p.id" \
        > "$REF_MISMATCH"

    mysql_query "SELECT CONCAT_WS('\t', p.id, p.stored_name, p.ref_count, COUNT(f.id)) FROM physical_files p LEFT JOIN files f ON f.physical_id=p.id GROUP BY p.id, p.stored_name, p.ref_count HAVING COUNT(f.id)=0 ORDER BY p.id" \
        > "$STALE_PHYSICAL"

    if [ -d "$UPLOAD_ROOT" ]; then
        find "$UPLOAD_ROOT" -maxdepth 1 -type f ! -name '.gitkeep' -printf '%f\n' | sort -u > "$DISK_FILES"
    else
        : > "$DISK_FILES"
    fi

    comm -13 "$DISK_FILES" "$DB_REFERENCED" > "$MISSING_REFERENCED"
    comm -23 "$DISK_FILES" "$DB_REFERENCED" > "$UNREFERENCED_DISK"
}

line_count() {
    wc -l < "$1" | tr -d ' '
}

has_lines() {
    [ -s "$1" ]
}

print_file() {
    title="$1"
    file="$2"
    if [ "$QUIET" -eq 0 ] && has_lines "$file"; then
        echo "$title"
        sed 's/^/  /' "$file"
    fi
}

issue_count() {
    count=0
    count=$((count + $(line_count "$REF_MISMATCH")))
    count=$((count + $(line_count "$STALE_PHYSICAL")))
    count=$((count + $(line_count "$MISSING_REFERENCED")))
    count=$((count + $(line_count "$UNREFERENCED_DISK")))
    echo "$count"
}

report() {
    ref_mismatch_count="$(line_count "$REF_MISMATCH")"
    stale_physical_count="$(line_count "$STALE_PHYSICAL")"
    missing_referenced_count="$(line_count "$MISSING_REFERENCED")"
    unreferenced_disk_count="$(line_count "$UNREFERENCED_DISK")"

    print_file "ref_count mismatches: physical_id stored_name stored_ref actual_refs" "$REF_MISMATCH"
    print_file "unreferenced physical_files rows: physical_id stored_name stored_ref actual_refs" "$STALE_PHYSICAL"
    print_file "DB-referenced files missing on disk:" "$MISSING_REFERENCED"
    print_file "disk files with no files-table reference:" "$UNREFERENCED_DISK"

    total="$((ref_mismatch_count + stale_physical_count + missing_referenced_count + unreferenced_disk_count))"
    if [ "$total" -eq 0 ]; then
        echo "storage consistency ok: upload_root=$UPLOAD_ROOT"
    else
        echo "storage consistency issues: ref_mismatch=$ref_mismatch_count stale_physical=$stale_physical_count missing_disk=$missing_referenced_count unreferenced_disk=$unreferenced_disk_count"
    fi
}

delete_unreferenced_disk_files() {
    if [ ! -d "$UPLOAD_ROOT" ]; then
        return 0
    fi

    while IFS= read -r stored_name; do
        [ -n "$stored_name" ] || continue
        rm -f -- "$UPLOAD_ROOT/$stored_name"
        if [ "$QUIET" -eq 0 ]; then
            echo "removed orphan disk file: $stored_name"
        fi
    done < "$UNREFERENCED_DISK"
}

delete_stale_physical_rows() {
    mysql_query "DELETE FROM physical_files WHERE ref_count=0 AND NOT EXISTS (SELECT 1 FROM files WHERE files.physical_id=physical_files.id)" >/dev/null
}

snapshot

if [ "$MODE" = "fix" ]; then
    if has_lines "$REF_MISMATCH"; then
        report
        fail "ref_count mismatch found; run migrations/006_ref_count_triggers.sql or investigate before fixing storage orphans"
    fi
    if has_lines "$MISSING_REFERENCED"; then
        report
        fail "DB-referenced files are missing on disk; refusing destructive orphan cleanup"
    fi

    delete_stale_physical_rows
    snapshot

    if has_lines "$REF_MISMATCH" || has_lines "$MISSING_REFERENCED"; then
        report
        fail "inconsistency remains after stale physical row cleanup; refusing disk cleanup"
    fi

    delete_unreferenced_disk_files
    snapshot
fi

report
[ "$(issue_count)" -eq 0 ]
