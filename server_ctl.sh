#!/bin/zsh

set -eu

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
CONFIG_FILE=${CONFIG_FILE:-"$ROOT_DIR/server.conf"}
SERVER_BIN=${SERVER_BIN:-"$ROOT_DIR/build/server"}

read_config_value() {
    local key="$1"
    local default_value="$2"
    if [ ! -f "$CONFIG_FILE" ]; then
        printf '%s\n' "$default_value"
        return
    fi

    local value
    value=$(awk -F= -v key="$key" '
        $0 !~ /^[[:space:]]*#/ && $1 ~ /^[[:space:]]*[^[:space:]]+/ {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", $1)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
            if ($1 == key) {
                print $2
                exit
            }
        }
    ' "$CONFIG_FILE")

    if [ -n "${value:-}" ]; then
        printf '%s\n' "$value"
    else
        printf '%s\n' "$default_value"
    fi
}

PID_FILE=$(read_config_value "pid_file" "./atlas-webserver.pid")
DAEMON_MODE=$(read_config_value "daemon_mode" "0")

case "$PID_FILE" in
    /*) ;;
    *) PID_FILE="$ROOT_DIR/$PID_FILE" ;;
esac

is_running() {
    if [ ! -f "$PID_FILE" ]; then
        return 1
    fi

    local pid
    pid=$(tr -d '[:space:]' < "$PID_FILE")
    if [ -z "$pid" ]; then
        rm -f "$PID_FILE"
        return 1
    fi

    if kill -0 "$pid" 2>/dev/null; then
        return 0
    fi

    rm -f "$PID_FILE"
    return 1
}

current_pid() {
    tr -d '[:space:]' < "$PID_FILE"
}

start_server() {
    if [ ! -x "$SERVER_BIN" ]; then
        echo "server binary not found: $SERVER_BIN"
        exit 1
    fi

    if is_running; then
        echo "server already running, pid=$(current_pid)"
        exit 0
    fi

    if [ "$DAEMON_MODE" != "1" ]; then
        echo "daemon_mode is not enabled in $CONFIG_FILE"
        echo "set daemon_mode=1 before using server_ctl.sh start"
        exit 1
    fi

    "$SERVER_BIN" -f "$CONFIG_FILE"

    sleep 1
    if is_running; then
        echo "server started, pid=$(current_pid)"
        exit 0
    fi

    echo "server start failed"
    exit 1
}

stop_server() {
    if ! is_running; then
        echo "server not running"
        exit 0
    fi

    local pid
    pid=$(current_pid)
    kill -TERM "$pid"

    local i=0
    while [ $i -lt 20 ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$PID_FILE"
            echo "server stopped"
            exit 0
        fi
        sleep 1
        i=$((i + 1))
    done

    echo "server stop timeout, pid=$pid"
    exit 1
}

reload_server() {
    if ! is_running; then
        echo "server not running"
        exit 1
    fi

    local pid
    pid=$(current_pid)
    kill -HUP "$pid"
    echo "reload signal sent, pid=$pid"
}

status_server() {
    if is_running; then
        echo "server is running, pid=$(current_pid)"
    else
        echo "server is stopped"
    fi
}

restart_server() {
    if is_running; then
        stop_server
    fi
    start_server
}

ACTION=${1:-status}

case "$ACTION" in
    start)
        start_server
        ;;
    stop)
        stop_server
        ;;
    restart)
        restart_server
        ;;
    reload)
        reload_server
        ;;
    status)
        status_server
        ;;
    *)
        echo "usage: $0 {start|stop|restart|reload|status}"
        exit 1
        ;;
esac
