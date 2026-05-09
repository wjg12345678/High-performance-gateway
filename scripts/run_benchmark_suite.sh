#!/bin/sh

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BASE_URL="${BASE_URL:-http://127.0.0.1:9006}"
THREADS="${THREADS:-4}"
DURATION="${DURATION:-15s}"
CONCURRENCY_SET="${CONCURRENCY_SET:-50 100 200}"
USER_NAME="${USER_NAME:-upload_fix_manual}"
PASSWORD="${PASSWORD:-123456}"
REPORT_DIR="${REPORT_DIR:-$ROOT_DIR/reports/benchmarks/$(date +%Y%m%d_%H%M%S)}"
COMPOSE_CMD="${COMPOSE_CMD:-docker compose}"
suite_failed=0

mkdir -p "$REPORT_DIR"

resolve_container_id() {
    service="$1"
    container_id="$($COMPOSE_CMD ps -q "$service" 2>/dev/null | head -n 1)"
    if [ -z "$container_id" ]; then
        echo "failed to resolve container id for service: $service" >&2
        exit 1
    fi
    printf '%s\n' "$container_id"
}

resolve_container_name() {
    container_id="$1"
    docker inspect --format '{{.Name}}' "$container_id" 2>/dev/null | sed 's#^/##'
}

restart_count() {
    container_id="$1"
    docker inspect --format '{{.RestartCount}}' "$container_id" 2>/dev/null || echo "0"
}

socket_error_total() {
    errors="$1"
    if [ "$errors" = "none" ]; then
        echo "0"
        return
    fi
    printf '%s\n' "$errors" | awk '
        {
            for (i = 2; i <= NF; i += 2) {
                gsub(/,/, "", $i)
                sum += $i + 0
            }
            print sum + 0
        }
    '
}

has_sigsegv_since() {
    container_id="$1"
    since_ts="$2"
    if docker logs --since "$since_ts" "$container_id" 2>&1 | grep -q 'server received SIGSEGV'; then
        echo "1"
    else
        echo "0"
    fi
}

WEB_CONTAINER_ID="$(resolve_container_id web)"
MYSQL_CONTAINER_ID="$(resolve_container_id mysql)"
WEB_CONTAINER_NAME="$(resolve_container_name "$WEB_CONTAINER_ID")"
MYSQL_CONTAINER_NAME="$(resolve_container_name "$MYSQL_CONTAINER_ID")"

login_json="$(curl -sS -X POST "$BASE_URL/api/login" \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$USER_NAME\",\"passwd\":\"$PASSWORD\"}")"
TOKEN="$(printf '%s\n' "$login_json" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"

if [ -z "$TOKEN" ]; then
    echo "failed to obtain token: $login_json" >&2
    exit 1
fi

echo "report_dir=$REPORT_DIR"
echo "base_url=$BASE_URL"
echo "threads=$THREADS"
echo "duration=$DURATION"
echo "concurrency_set=$CONCURRENCY_SET"
echo "user_name=$USER_NAME"
echo "web_container_id=$WEB_CONTAINER_ID"
echo "mysql_container_id=$MYSQL_CONTAINER_ID"
echo "web_container_name=$WEB_CONTAINER_NAME"
echo "mysql_container_name=$MYSQL_CONTAINER_NAME"

printf '%s\n' "endpoint,method,concurrency,duration,threads,request_size_bytes,db_hit,https,requests_per_sec,latency_avg_ms,latency_p50_ms,latency_p90_ms,latency_p95_ms,latency_p99_ms,transfer_per_sec_mb,errors,socket_error_total,web_restart_delta,mysql_restart_delta,sigsegv_detected,valid,invalid_reason,web_cpu_peak,web_mem_peak,mysql_cpu_peak,mysql_mem_peak" > "$REPORT_DIR/results.csv"

run_case() {
    name="$1"
    method="$2"
    url="$3"
    concurrency="$4"
    script_path="$5"
    request_size="$6"
    db_hit="$7"

    wrk_out="$REPORT_DIR/${name}_c${concurrency}.wrk.txt"
    stats_out="$REPORT_DIR/${name}_c${concurrency}.stats.csv"
    gate_out="$REPORT_DIR/${name}_c${concurrency}.gate.txt"
    web_logs_out="$REPORT_DIR/${name}_c${concurrency}.web.log.txt"
    mysql_logs_out="$REPORT_DIR/${name}_c${concurrency}.mysql.log.txt"
    case_started_at="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    web_restart_before="$(restart_count "$WEB_CONTAINER_ID")"
    mysql_restart_before="$(restart_count "$MYSQL_CONTAINER_ID")"

    : > "$stats_out"
    (
        while :; do
            docker stats --no-stream --format '{{.Container}},{{.Name}},{{.CPUPerc}},{{.MemUsage}}' \
              "$WEB_CONTAINER_ID" "$MYSQL_CONTAINER_ID" >> "$stats_out" 2>/dev/null || true
            sleep 1
        done
    ) &
    stats_pid=$!

    wrk_status=0
    if [ -n "$script_path" ]; then
        TOKEN="$TOKEN" wrk -t"$THREADS" -c"$concurrency" -d"$DURATION" --latency -s "$script_path" "$url" > "$wrk_out" || wrk_status=$?
    else
        wrk -t"$THREADS" -c"$concurrency" -d"$DURATION" --latency "$url" > "$wrk_out" || wrk_status=$?
    fi

    kill "$stats_pid" >/dev/null 2>&1 || true
    wait "$stats_pid" 2>/dev/null || true
    if [ "$wrk_status" -ne 0 ]; then
        echo "wrk failed for case: $name c=$concurrency" >&2
        exit "$wrk_status"
    fi

    requests_per_sec="$(awk '/Requests\/sec:/ {print $2}' "$wrk_out")"
    transfer_per_sec="$(awk '/Transfer\/sec:/ {print $2 " " $3}' "$wrk_out")"
    latency_avg="$(awk '/Latency/ && $1=="Latency" {print $2 " " $3; exit}' "$wrk_out")"
    p50="$(awk '/50%/ {print $2 " " $3}' "$wrk_out")"
    p90="$(awk '/90%/ {print $2 " " $3}' "$wrk_out")"
    p99="$(awk '/99%/ {print $2 " " $3}' "$wrk_out")"
    errors="$(awk -F': ' '/Socket errors:/ {print $2}' "$wrk_out")"
    if [ -z "${errors:-}" ]; then
        errors="none"
    fi
    socket_errors_total="$(socket_error_total "$errors")"
    web_restart_after="$(restart_count "$WEB_CONTAINER_ID")"
    mysql_restart_after="$(restart_count "$MYSQL_CONTAINER_ID")"
    web_restart_delta=$((web_restart_after - web_restart_before))
    mysql_restart_delta=$((mysql_restart_after - mysql_restart_before))
    sigsegv_detected="$(has_sigsegv_since "$WEB_CONTAINER_ID" "$case_started_at")"
    case_finished_at="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

    valid="1"
    invalid_reason="none"
    captured_web_logs="0"
    captured_mysql_logs="0"
    if [ "$socket_errors_total" -ne 0 ] || [ "$web_restart_delta" -ne 0 ] || [ "$mysql_restart_delta" -ne 0 ] || [ "$sigsegv_detected" -ne 0 ]; then
        valid="0"
        invalid_reason=""
        if [ "$socket_errors_total" -ne 0 ]; then
            invalid_reason="${invalid_reason}socket_errors=$socket_errors_total;"
        fi
        if [ "$web_restart_delta" -ne 0 ]; then
            invalid_reason="${invalid_reason}web_restart_delta=$web_restart_delta;"
        fi
        if [ "$mysql_restart_delta" -ne 0 ]; then
            invalid_reason="${invalid_reason}mysql_restart_delta=$mysql_restart_delta;"
        fi
        if [ "$sigsegv_detected" -ne 0 ]; then
            invalid_reason="${invalid_reason}sigsegv_detected=1;"
        fi
        suite_failed=1
        echo "invalid benchmark case: $name c=$concurrency reason=$invalid_reason" >&2
        docker logs --since "$case_started_at" "$WEB_CONTAINER_ID" > "$web_logs_out" 2>&1 || true
        docker logs --since "$case_started_at" "$MYSQL_CONTAINER_ID" > "$mysql_logs_out" 2>&1 || true
        captured_web_logs="1"
        captured_mysql_logs="1"
    fi

    cat > "$gate_out" <<EOF
endpoint=$name
method=$method
concurrency=$concurrency
started_at=$case_started_at
finished_at=$case_finished_at
web_container_id=$WEB_CONTAINER_ID
web_container_name=$WEB_CONTAINER_NAME
mysql_container_id=$MYSQL_CONTAINER_ID
mysql_container_name=$MYSQL_CONTAINER_NAME
errors=$errors
socket_error_total=$socket_errors_total
web_restart_before=$web_restart_before
web_restart_after=$web_restart_after
web_restart_delta=$web_restart_delta
mysql_restart_before=$mysql_restart_before
mysql_restart_after=$mysql_restart_after
mysql_restart_delta=$mysql_restart_delta
sigsegv_detected=$sigsegv_detected
valid=$valid
invalid_reason=$invalid_reason
web_logs_captured=$captured_web_logs
web_logs_file=$(basename "$web_logs_out")
mysql_logs_captured=$captured_mysql_logs
mysql_logs_file=$(basename "$mysql_logs_out")
EOF

    web_cpu_peak="$(awk -F',' -v id="$WEB_CONTAINER_ID" '$1==id {gsub(/%/,"",$3); if ($3+0>max) max=$3+0} END {if (max=="") max=0; print max}' "$stats_out")"
    mysql_cpu_peak="$(awk -F',' -v id="$MYSQL_CONTAINER_ID" '$1==id {gsub(/%/,"",$3); if ($3+0>max) max=$3+0} END {if (max=="") max=0; print max}' "$stats_out")"
    web_mem_peak="$(awk -F',' -v id="$WEB_CONTAINER_ID" '$1==id {if (match($4, /[0-9.]+MiB/)) {v=substr($4, RSTART, RLENGTH-3)+0; if (v>max) max=v}} END {if (max=="") max=0; print max}' "$stats_out")"
    mysql_mem_peak="$(awk -F',' -v id="$MYSQL_CONTAINER_ID" '$1==id {if (match($4, /[0-9.]+MiB/)) {v=substr($4, RSTART, RLENGTH-3)+0; if (v>max) max=v}} END {if (max=="") max=0; print max}' "$stats_out")"

    latency_avg_ms="$(printf '%s\n' "$latency_avg" | awk '
        {v=$1; u=$2; if (u=="us") v/=1000; else if (u=="s") v*=1000; print v}
    ')"
    p50_ms="$(printf '%s\n' "$p50" | awk '
        {v=$1; u=$2; if (u=="us") v/=1000; else if (u=="s") v*=1000; print v}
    ')"
    p90_ms="$(printf '%s\n' "$p90" | awk '
        {v=$1; u=$2; if (u=="us") v/=1000; else if (u=="s") v*=1000; print v}
    ')"
    p95_ms="NA"
    p99_ms="$(printf '%s\n' "$p99" | awk '
        {v=$1; u=$2; if (u=="us") v/=1000; else if (u=="s") v*=1000; print v}
    ')"
    transfer_mb="$(printf '%s\n' "$transfer_per_sec" | awk '
        {v=$1; u=$2; if (u=="KB") v/=1024; else if (u=="GB") v*=1024; print v}
    ')"

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s",%s,%s,%s,%s,%s,"%s",%s,%s,%s,%s\n' \
      "$name" "$method" "$concurrency" "$DURATION" "$THREADS" "$request_size" "$db_hit" "0" \
      "$requests_per_sec" "$latency_avg_ms" "$p50_ms" "$p90_ms" "$p95_ms" "$p99_ms" "$transfer_mb" "$errors" \
      "$socket_errors_total" "$web_restart_delta" "$mysql_restart_delta" "$sigsegv_detected" "$valid" "$invalid_reason" \
      "$web_cpu_peak" "$web_mem_peak" "$mysql_cpu_peak" "$mysql_mem_peak" >> "$REPORT_DIR/results.csv"
}

for c in $CONCURRENCY_SET; do
    run_case "healthz" "GET" "$BASE_URL/healthz" "$c" "$ROOT_DIR/test_pressure/healthz.lua" "0" "0"
    run_case "static_index" "GET" "$BASE_URL/" "$c" "" "0" "0"
    run_case "login" "POST" "$BASE_URL/api/login" "$c" "$ROOT_DIR/test_pressure/login.lua" "48" "1"
    run_case "private_ping" "GET" "$BASE_URL/api/private/ping" "$c" "$ROOT_DIR/test_pressure/private_ping.lua" "0" "1"
    run_case "private_files" "GET" "$BASE_URL/api/private/files" "$c" "$ROOT_DIR/test_pressure/private_files.lua" "0" "1"
    run_case "private_upload" "POST" "$BASE_URL/api/private/files" "$c" "$ROOT_DIR/test_pressure/private_upload.lua" "336" "1"
done

echo "done: $REPORT_DIR/results.csv"
if [ "$suite_failed" -ne 0 ]; then
    echo "benchmark suite completed with invalid cases" >&2
    exit 1
fi
