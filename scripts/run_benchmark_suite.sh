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

mkdir -p "$REPORT_DIR"

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

printf '%s\n' "endpoint,method,concurrency,duration,threads,request_size_bytes,db_hit,https,requests_per_sec,latency_avg_ms,latency_p50_ms,latency_p90_ms,latency_p95_ms,latency_p99_ms,transfer_per_sec_mb,errors,web_cpu_peak,web_mem_peak,mysql_cpu_peak,mysql_mem_peak" > "$REPORT_DIR/results.csv"

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

    : > "$stats_out"
    (
        while :; do
            docker stats --no-stream --format '{{.Name}},{{.CPUPerc}},{{.MemUsage}}' \
              atlas-webserver-web-1 atlas-webserver-mysql-1 >> "$stats_out" 2>/dev/null || true
            sleep 1
        done
    ) &
    stats_pid=$!

    if [ -n "$script_path" ]; then
        TOKEN="$TOKEN" wrk -t"$THREADS" -c"$concurrency" -d"$DURATION" --latency -s "$script_path" "$url" > "$wrk_out"
    else
        wrk -t"$THREADS" -c"$concurrency" -d"$DURATION" --latency "$url" > "$wrk_out"
    fi

    kill "$stats_pid" >/dev/null 2>&1 || true
    wait "$stats_pid" 2>/dev/null || true

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

    web_cpu_peak="$(awk -F',' '$1=="atlas-webserver-web-1" {gsub(/%/,"",$2); if ($2+0>max) max=$2+0} END {if (max=="") max=0; print max}' "$stats_out")"
    mysql_cpu_peak="$(awk -F',' '$1=="atlas-webserver-mysql-1" {gsub(/%/,"",$2); if ($2+0>max) max=$2+0} END {if (max=="") max=0; print max}' "$stats_out")"
    web_mem_peak="$(awk -F',' '$1=="atlas-webserver-web-1" {if (match($3, /[0-9.]+MiB/)) {v=substr($3, RSTART, RLENGTH-3)+0; if (v>max) max=v}} END {if (max=="") max=0; print max}' "$stats_out")"
    mysql_mem_peak="$(awk -F',' '$1=="atlas-webserver-mysql-1" {if (match($3, /[0-9.]+MiB/)) {v=substr($3, RSTART, RLENGTH-3)+0; if (v>max) max=v}} END {if (max=="") max=0; print max}' "$stats_out")"

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

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s",%s,%s,%s,%s\n' \
      "$name" "$method" "$concurrency" "$DURATION" "$THREADS" "$request_size" "$db_hit" "0" \
      "$requests_per_sec" "$latency_avg_ms" "$p50_ms" "$p90_ms" "$p95_ms" "$p99_ms" "$transfer_mb" "$errors" \
      "$web_cpu_peak" "$web_mem_peak" "$mysql_cpu_peak" "$mysql_mem_peak" >> "$REPORT_DIR/results.csv"
}

for c in $CONCURRENCY_SET; do
    run_case "healthz" "GET" "$BASE_URL/healthz" "$c" "$ROOT_DIR/test_pressure/healthz.lua" "0" "0"
    run_case "static_index" "GET" "$BASE_URL/" "$c" "" "0" "0"
    run_case "login" "POST" "$BASE_URL/api/login" "$c" "$ROOT_DIR/test_pressure/login.lua" "48" "1"
    run_case "private_ping" "GET" "$BASE_URL/api/private/ping" "$c" "$ROOT_DIR/test_pressure/private_ping.lua" "0" "1"
    run_case "private_files" "GET" "$BASE_URL/api/private/files" "$c" "$ROOT_DIR/test_pressure/private_files.lua" "0" "1"
    run_case "private_upload" "POST" "$BASE_URL/api/private/files" "$c" "$ROOT_DIR/test_pressure/private_upload.lua" "101" "1"
done

echo "done: $REPORT_DIR/results.csv"
