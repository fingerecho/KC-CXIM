#!/usr/bin/env bash
# ============================================================================
# show_svc_status.sh：压测期间实时监控服务端与 DB 状态
# ----------------------------------------------------------------------------
# 实时（默认）模式：每隔 --interval 秒刷新一次，持续展示，Ctrl+C 退出。
# 一次性模式：加 --once，输出一屏后退出。
#
# 监控内容：
#   1) MySQL PROCESSLIST / Threads_connected / Threads_running
#   2) InnoDB 锁等待（TRANSACTIONS 段）
#   3) 服务端线程级 CPU（top -H）
#   4) 服务端 TCP 连接与 listen 队列（ss）
#
# DB 连接信息优先从 DBConfig.ini 的 [db] 段读取：
#   [db]
#   host=127.0.0.1
#   port=3306
#   dbname=CXIMDB
#   user=devuser
#   password=devpass
#
# 服务端 PID 获取顺序：
#   1) --svc-pid 显式指定
#   2) pidof $PROC_NAME（默认 tcp_echo_server，可用 --proc-name 改）
#   3) pgrep -f $PROC_HINT（--proc 指定）
#   4) ss -lntp 从 TCP 端口反查（需要权限）
#
# 用法：
#   ./show_svc_status.sh
#   ./show_svc_status.sh --interval 2
#   ./show_svc_status.sh --once
#   ./show_svc_status.sh --config /path/DBConfig.ini
#   ./show_svc_status.sh --proc-name tcp_echo_server
#   ./show_svc_status.sh --svc-pid 12345 --tcp-port 8888
#   ./show_svc_status.sh --mysql-pass 'override'
#
# 依赖：mysql 客户端、top、ss（iproute2）、pidof（sysvinit-utils）。
# ============================================================================

set -u

CONFIG_FILE="./DBConfig.ini"
SVC_PID=""
TCP_PORT="8888"
MYSQL_HOST="127.0.0.1"
MYSQL_PORT="3306"
MYSQL_USER="root"
MYSQL_PASS=""
MYSQL_DB=""
TOP_SECONDS=1
PROC_HINT=""
PROC_NAME="tcp_echo_server"
INTERVAL=3
ONCE=0

usage() {
    sed -n '2,34p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

load_db_config() {
    local file="$1"
    if [[ ! -f "$file" ]]; then
        echo "[warn] 配置文件不存在: $file，使用默认/命令行参数" >&2
        return
    fi
    local in_db=0 line key val
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        line="$(echo "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
        [[ -z "$line" ]] && continue
        [[ "$line" == \;* || "$line" == \#* ]] && continue
        if [[ "$line" =~ ^\[(.+)\]$ ]]; then
            [[ "${BASH_REMATCH[1]}" == "db" ]] && in_db=1 || in_db=0
            continue
        fi
        [[ $in_db -eq 1 ]] || continue
        if [[ "$line" == *=* ]]; then
            key="${line%%=*}"; val="${line#*=}"
            key="$(echo "$key" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
            val="$(echo "$val" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
            case "$key" in
                host)     MYSQL_HOST="$val" ;;
                port)     MYSQL_PORT="$val" ;;
                dbname)   MYSQL_DB="$val" ;;
                user)     MYSQL_USER="$val" ;;
                password) MYSQL_PASS="$val" ;;
            esac
        fi
    done < "$file"
}

HOST_SET=0; PORT_SET=0; USER_SET=0; PASS_SET=0; DB_SET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)     CONFIG_FILE="$2"; shift 2 ;;
        --svc-pid)    SVC_PID="$2"; shift 2 ;;
        --tcp-port)   TCP_PORT="$2"; shift 2 ;;
        --mysql-host) MYSQL_HOST="$2"; HOST_SET=1; shift 2 ;;
        --mysql-port) MYSQL_PORT="$2"; PORT_SET=1; shift 2 ;;
        --mysql-user) MYSQL_USER="$2"; USER_SET=1; shift 2 ;;
        --mysql-pass) MYSQL_PASS="$2"; PASS_SET=1; shift 2 ;;
        --mysql-db)   MYSQL_DB="$2";   DB_SET=1;   shift 2 ;;
        --top-sec)    TOP_SECONDS="$2"; shift 2 ;;
        --proc)       PROC_HINT="$2"; shift 2 ;;
        --proc-name)  PROC_NAME="$2"; shift 2 ;;
        --interval)   INTERVAL="$2"; shift 2 ;;
        --once)       ONCE=1; shift ;;
        -h|--help)    usage ;;
        *) echo "未知参数: $1"; usage ;;
    esac
done

SAVED_HOST="$MYSQL_HOST"; SAVED_PORT="$MYSQL_PORT"
SAVED_USER="$MYSQL_USER"; SAVED_PASS="$MYSQL_PASS"; SAVED_DB="$MYSQL_DB"
load_db_config "$CONFIG_FILE"
[[ $HOST_SET -eq 1 ]] && MYSQL_HOST="$SAVED_HOST"
[[ $PORT_SET -eq 1 ]] && MYSQL_PORT="$SAVED_PORT"
[[ $USER_SET -eq 1 ]] && MYSQL_USER="$SAVED_USER"
[[ $PASS_SET -eq 1 ]] && MYSQL_PASS="$SAVED_PASS"
[[ $DB_SET   -eq 1 ]] && MYSQL_DB="$SAVED_DB"

hr() { printf '%s\n' "=================================================================="; }
section() { echo; hr; echo ">> $*"; hr; }
have() { command -v "$1" >/dev/null 2>&1; }

mysql_exec() {
    local args=(-h "$MYSQL_HOST" -P "$MYSQL_PORT" -u "$MYSQL_USER" --batch --raw)
    [[ -n "$MYSQL_DB" ]] && args+=("$MYSQL_DB")
    if [[ -n "$MYSQL_PASS" ]]; then
        MYSQL_PWD="$MYSQL_PASS" mysql "${args[@]}" -e "$1" 2>&1
    else
        mysql "${args[@]}" -e "$1" 2>&1
    fi
}

resolve_pid() {
    if [[ -n "$SVC_PID" ]]; then echo "$SVC_PID"; return; fi
    if [[ -n "$PROC_NAME" ]] && have pidof; then
        local p
        p="$(pidof "$PROC_NAME" 2>/dev/null | awk '{print $1}')"
        if [[ -n "$p" ]]; then echo "$p"; return; fi
    fi
    if [[ -n "$PROC_HINT" ]]; then pgrep -f "$PROC_HINT" | head -n1; return; fi
    if have ss; then
        ss -lntp 2>/dev/null | awk -v p=":$TCP_PORT" '$4 ~ p {print $NF}' \
            | sed -n 's/.*pid=\([0-9]\+\).*/\1/p' | head -n1
    fi
}

print_mysql_proc() {
    if ! have mysql; then echo "[跳过] 未找到 mysql 客户端"; return; fi
    echo "-- Threads_connected / Threads_running --"
    mysql_exec "SHOW STATUS LIKE 'Threads_connected'; SHOW STATUS LIKE 'Threads_running';" \
        | awk 'NR>1 || /Threads/'
    echo
    echo "-- PROCESSLIST 各 State 数量 Top 10 --"
    mysql_exec "SELECT COALESCE(State,'(null)') AS state, COUNT(*) AS cnt
                FROM information_schema.PROCESSLIST
                GROUP BY state ORDER BY cnt DESC LIMIT 10;" | awk 'NR>1 || /state/'
    echo
    echo "-- 非 Sleep 查询 Top 10（按 Time 降序）--"
    mysql_exec "SELECT ID, USER, DB, COMMAND, TIME, STATE, LEFT(INFO, 100) AS INFO
                FROM information_schema.PROCESSLIST
                WHERE Command <> 'Sleep' ORDER BY Time DESC LIMIT 10;" | awk 'NR>1 || /^ID/'
    echo
    echo "-- 锁等待相关会话 --"
    mysql_exec "SELECT ID, TIME, STATE, LEFT(INFO,100) AS INFO
                FROM information_schema.PROCESSLIST
                WHERE State LIKE '%lock%' OR State LIKE '%Waiting%'
                ORDER BY Time DESC LIMIT 10;" | awk 'NR>1 || /^ID/'
}

print_innodb() {
    if ! have mysql; then echo "[跳过] 未找到 mysql 客户端"; return; fi
    local out
    out="$(mysql_exec "SHOW ENGINE INNODB STATUS\G")"
    if grep -q "LOCK WAIT" <<<"$out"; then
        echo "!! 检测到 LOCK WAIT，TRANSACTIONS 段原文："
        awk '/^TRANSACTIONS$/{f=1} f{print} /^FILE I\/O$/{f=0}' <<<"$out"
    else
        echo "未检测到 LOCK WAIT。TRANSACTIONS 段摘要："
        awk '/^TRANSACTIONS$/{f=1} f{print} /^FILE I\/O$/{f=0}' <<<"$out" | sed -n '1,40p'
    fi
}

print_threads() {
    local pid; pid="$(resolve_pid)"
    if [[ -z "$pid" ]]; then
        echo "[跳过] 未找到服务端 PID（试过 --svc-pid / pidof $PROC_NAME / --proc / ss 反查）"
        return
    fi
    if ! have top; then echo "[跳过] 未找到 top"; return; fi
    echo "PID=$pid（来源: $(if [[ -n "$SVC_PID" ]]; then echo "--svc-pid"; elif pidof "$PROC_NAME" >/dev/null 2>&1; then echo "pidof $PROC_NAME"; else echo "其他"; fi)），线程 CPU（前 10）"
    top -H -b -n 1 -d "$TOP_SECONDS" -p "$pid" \
        | awk 'BEGIN{s=0} /^ *PID/{s=1} s{print}' | head -n 15
}

print_tcp() {
    if ! have ss; then echo "[跳过] 未找到 ss"; return; fi
    local est
    est="$(ss -tn state established "( sport = :$TCP_PORT )" 2>/dev/null | tail -n +2 | wc -l)"
    echo "已建立连接数（established）: $est"
    echo
    echo "-- listen 队列（Recv-Q 持续 > 0 说明 accept 跟不上）--"
    ss -lnt | awk -v p=":$TCP_PORT" 'NR==1 || $4 ~ p'
    echo
    echo "-- 各状态连接数 --"
    ss -tan "( sport = :$TCP_PORT )" 2>/dev/null \
        | awk 'NR>1 {print $1}' | sort | uniq -c | sort -rn
}

render_once() {
    date '+检查时间: %F %T'
    echo "配置来源: ${CONFIG_FILE}"
    echo "目标: MySQL=${MYSQL_USER}@${MYSQL_HOST}:${MYSQL_PORT}/${MYSQL_DB:-<无>}  TCP端口=${TCP_PORT}  进程名=${PROC_NAME}"
    section "1) MySQL PROCESSLIST / 连接数 / 运行线程数"; print_mysql_proc
    section "2) InnoDB 事务与锁等待";                  print_innodb
    section "3) 服务端线程级 CPU";                     print_threads
    section "4) TCP 连接与 listen 队列";               print_tcp
    echo
}

if [[ "$ONCE" -eq 1 ]]; then
    render_once
    exit 0
fi

trap 'echo; echo "[exit] 已停止实时监控"; exit 0' INT TERM
echo "[info] 实时监控中，每 ${INTERVAL}s 刷新一次，Ctrl+C 退出"

while true; do
    clear 2>/dev/null || printf '\033[2J\033[H'
    render_once
    echo "[info] 下次刷新: ${INTERVAL}s 后（Ctrl+C 退出）"
    sleep "$INTERVAL"
done
