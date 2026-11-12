#!/usr/bin/env bash
# ============================================================================
# show_svc_monitor.sh：KC-CXIM 服务端实时状态监控台
# ----------------------------------------------------------------------------
# 与 show_svc_status.sh 的区别：
#   * 采用 ANSI 光标定位做“局部刷新”，只重绘发生变化的行，
#     不再每次 clear/整屏重绘，长时间观察时不会闪烁。
#   * 固定面板布局 + 颜色分级，适合压测时放在旁边盯着看。
#
# 监控内容：
#   1) 服务进程：PID / CPU / 内存 / 线程数 / FD / 运行时长
#   2) 连接：TCP listen 队列、各状态连接数、HTTP 探测
#   3) MySQL：连接数、运行线程、QPS、慢查询、锁等待、死锁、Buffer 命中率
#   4) MySQL 会话状态 Top 5（带柱状条）
#   5) IM 业务表近似行数
#   6) 当前锁等待 / Waiting 会话
#
# 用法：
#   ./show_svc_monitor.sh
#   ./show_svc_monitor.sh --interval 2
#   ./show_svc_monitor.sh --once
#   ./show_svc_monitor.sh --config ../DBConfig.ini
#   ./show_svc_monitor.sh --svc-host 110.40.206.253 --http-port 8080 --tcp-port 8888
#   ./show_svc_monitor.sh --proc-name tcp_echo_server --svc-pid 12345
#   ./show_svc_monitor.sh --mysql-pass 'override'
#   ./show_svc_monitor.sh --ascii        # 终端不支持 UTF-8 时使用 ASCII 边框
#
# DB 连接信息优先从 DBConfig.ini 的 [db] 段读取。
# 服务端 PID 获取顺序：
#   --svc-pid -> pidof $PROC_NAME -> --proc 提示 -> pgrep -> ss 反查。
#
# 依赖：bash 4+、mysql 客户端（可选）、curl（可选）、ss/iproute2（可选）。
# ============================================================================

set -u

CONFIG_FILE="./DBConfig.ini"
SVC_PID=""
SVC_HOST="127.0.0.1"
TCP_PORT="8888"
HTTP_PORT="8080"
MYSQL_HOST="127.0.0.1"
MYSQL_PORT="3306"
MYSQL_USER="root"
MYSQL_PASS=""
MYSQL_DB=""
PROC_HINT=""
PROC_NAME="tcp_echo_server"
INTERVAL=2
ONCE=0
NO_COLOR=0
USE_UNICODE=1

usage() {
    grep '^#' "$0" | sed -n '2,60p' | sed 's/^# \{0,1\}//'
    exit 1
}

have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# 参数解析
# ---------------------------------------------------------------------------
HOST_SET=0; PORT_SET=0; USER_SET=0; PASS_SET=0; DB_SET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)     CONFIG_FILE="$2"; shift 2 ;;
        --svc-pid)    SVC_PID="$2"; shift 2 ;;
        --svc-host)   SVC_HOST="$2"; shift 2 ;;
        --tcp-port)   TCP_PORT="$2"; shift 2 ;;
        --http-port)  HTTP_PORT="$2"; shift 2 ;;
        --mysql-host) MYSQL_HOST="$2"; HOST_SET=1; shift 2 ;;
        --mysql-port) MYSQL_PORT="$2"; PORT_SET=1; shift 2 ;;
        --mysql-user) MYSQL_USER="$2"; USER_SET=1; shift 2 ;;
        --mysql-pass) MYSQL_PASS="$2"; PASS_SET=1; shift 2 ;;
        --mysql-db)   MYSQL_DB="$2";   DB_SET=1;   shift 2 ;;
        --proc)       PROC_HINT="$2"; shift 2 ;;
        --proc-name)  PROC_NAME="$2"; shift 2 ;;
        --interval)   INTERVAL="$2"; shift 2 ;;
        --once)       ONCE=1; shift ;;
        --no-color)   NO_COLOR=1; shift ;;
        --ascii)      USE_UNICODE=0; shift ;;
        -h|--help)    usage ;;
        *) echo "未知参数: $1" >&2; usage ;;
    esac
done

[[ "$INTERVAL" =~ ^[0-9]+$ ]] || INTERVAL=2
(( INTERVAL < 1 )) && INTERVAL=1

# ---------------------------------------------------------------------------
# 配置读取（保持与 show_svc_status.sh 相同的 [db] 段解析规则）
# ---------------------------------------------------------------------------
load_db_config() {
    local file="$1"
    [[ -f "$file" ]] || return 0
    local in_db=0 line key val
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        line="$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
        [[ -z "$line" ]] && continue
        [[ "$line" == \;* || "$line" == \#* ]] && continue
        if [[ "$line" =~ ^\[(.+)\]$ ]]; then
            [[ "${BASH_REMATCH[1]}" == "db" ]] && in_db=1 || in_db=0
            continue
        fi
        [[ $in_db -eq 1 ]] || continue
        if [[ "$line" == *=* ]]; then
            key="${line%%=*}"; val="${line#*=}"
            key="$(printf '%s' "$key" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
            val="$(printf '%s' "$val" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
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

SAVED_HOST="$MYSQL_HOST"; SAVED_PORT="$MYSQL_PORT"
SAVED_USER="$MYSQL_USER"; SAVED_PASS="$MYSQL_PASS"; SAVED_DB="$MYSQL_DB"
load_db_config "$CONFIG_FILE"
[[ $HOST_SET -eq 1 ]] && MYSQL_HOST="$SAVED_HOST"
[[ $PORT_SET -eq 1 ]] && MYSQL_PORT="$SAVED_PORT"
[[ $USER_SET -eq 1 ]] && MYSQL_USER="$SAVED_USER"
[[ $PASS_SET -eq 1 ]] && MYSQL_PASS="$SAVED_PASS"
[[ $DB_SET   -eq 1 ]] && MYSQL_DB="$SAVED_DB"

# ---------------------------------------------------------------------------
# 颜色与边框
# ---------------------------------------------------------------------------
if [[ -t 1 && $NO_COLOR -eq 0 ]]; then
    C_RESET=$'\033[0m'; C_BOLD=$'\033[1m'; C_DIM=$'\033[2m'
    C_RED=$'\033[31m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'
    C_CYAN=$'\033[36m'; C_WHITE=$'\033[37m'
    C_TITLE=$'\033[1;36m'
    C_OK=$'\033[1;32m'; C_WARN=$'\033[1;33m'; C_ALERT=$'\033[1;31m'
else
    C_RESET=""; C_BOLD=""; C_DIM=""
    C_RED=""; C_GREEN=""; C_YELLOW=""; C_CYAN=""; C_WHITE=""
    C_TITLE=""; C_OK=""; C_WARN=""; C_ALERT=""
fi

if (( USE_UNICODE )); then
    B_TL='┌'; B_TR='┐'; B_BL='└'; B_BR='┘'; B_H='─'; B_V='│'
    D_TL='╔'; D_TR='╗'; D_BL='╚'; D_BR='╝'; D_H='═'; D_V='║'
else
    B_TL='+'; B_TR='+'; B_BL='+'; B_BR='+'; B_H='-'; B_V='|'
    D_TL='+'; D_TR='+'; D_BL='+'; D_BR='+'; D_H='='; D_V='|'
fi

repeat_char() {
    local c="$1" n="$2" out="" i=0
    (( n < 0 )) && n=0
    for ((i = 0; i < n; i++)); do out+="$c"; done
    printf '%s' "$out"
}

# 清除控制字符/非 ASCII，保证固定宽度对齐（中文等多字节字符统一替换为 ?）。
safe() {
    local s="${1//$'\r'/ }"
    s="${s//$'\n'/ }"
    s="${s//$'\t'/ }"
    printf '%s' "$s" | LC_ALL=C tr -c '[:print:]' '?'
}

fit_plain() {
    local s; s="$(safe "$1")"
    local max="$2"
    (( max < 0 )) && max=0
    if (( ${#s} > max )); then s="${s:0:max}"; fi
    printf '%s' "$s"
}

term_width() {
    local w="${COLUMNS:-}"
    if [[ -z "$w" ]] && have tput; then w="$(tput cols 2>/dev/null || true)"; fi
    [[ "$w" =~ ^[0-9]+$ ]] || w=110
    (( w > 120 )) && w=120
    (( w < 60 )) && w=60
    printf '%s' "$w"
}

panel_top() {
    local title=" $1 "
    local n=$((WIDTH - 3 - ${#title}))
    (( n < 0 )) && n=0
    printf '%s%s%s%s%s%s%s' "$B_TL" "$B_H" "$C_TITLE" "$title" "$C_RESET" \
        "$(repeat_char "$B_H" "$n")" "$B_TR"
}

panel_bottom() {
    printf '%s%s%s' "$B_BL" "$(repeat_char "$B_H" $((WIDTH - 2)))" "$B_BR"
}

panel_line() {
    local content; content="$(safe "$1")"
    local color="${2:-}"
    local inner=$((WIDTH - 4))
    (( inner < 1 )) && inner=1
    if (( ${#content} > inner )); then content="${content:0:inner}"; fi
    local pad=$((inner - ${#content}))
    local spaces; printf -v spaces '%*s' "$pad" ''
    printf '%s %s%s%s%s %s' "$B_V" "$color" "$content" "$C_RESET" "$spaces" "$B_V"
}

# ---------------------------------------------------------------------------
# 通用格式化
# ---------------------------------------------------------------------------
fmt_duration() {
    local s="${1:-0}"
    [[ "$s" =~ ^[0-9]+$ ]] || s=0
    local d=$((s / 86400)) h=$(((s % 86400) / 3600))
    local m=$(((s % 3600) / 60)) sec=$((s % 60))
    if (( d > 0 )); then
        printf '%dd %02d:%02d:%02d' "$d" "$h" "$m" "$sec"
    else
        printf '%02d:%02d:%02d' "$h" "$m" "$sec"
    fi
}

human_num() {
    awk -v n="${1:-0}" 'BEGIN{
        n += 0
        if (n >= 1000000000) printf "%.2fG", n/1000000000
        else if (n >= 1000000) printf "%.2fM", n/1000000
        else if (n >= 1000) printf "%.1fK", n/1000
        else printf "%d", n
    }'
}

now_ms() {
    if [[ -n "${EPOCHREALTIME:-}" ]]; then
        awk -v t="${EPOCHREALTIME/,/.}" 'BEGIN{printf "%d", t*1000}'
    else
        date +%s%3N
    fi
}

mysql_q() {
    local args=(-h "$MYSQL_HOST" -P "$MYSQL_PORT" -u "$MYSQL_USER"
                -N -B --raw --connect-timeout=1)
    [[ -n "$MYSQL_DB" ]] && args+=("$MYSQL_DB")
    if [[ -n "$MYSQL_PASS" ]]; then
        MYSQL_PWD="$MYSQL_PASS" mysql "${args[@]}" -e "$1"
    else
        mysql "${args[@]}" -e "$1"
    fi
}

resolve_pid() {
    if [[ -n "$SVC_PID" ]]; then printf '%s' "$SVC_PID"; return; fi
    if [[ -n "$PROC_NAME" ]] && have pidof; then
        local p; p="$(pidof "$PROC_NAME" 2>/dev/null | awk '{print $1}')"
        [[ -n "$p" ]] && { printf '%s' "$p"; return; }
    fi
    if [[ -n "$PROC_HINT" ]] && have pgrep; then
        pgrep -f "$PROC_HINT" 2>/dev/null | head -n1; return
    fi
    if [[ -n "$PROC_NAME" ]] && have pgrep; then
        local p; p="$(pgrep -f "$PROC_NAME" 2>/dev/null | head -n1)"
        [[ -n "$p" ]] && { printf '%s' "$p"; return; }
    fi
    if have ss; then
        ss -lntp 2>/dev/null | awk -v p=":$TCP_PORT" '$4 ~ p {print $NF}' \
            | sed -n 's/.*pid=\([0-9]\+\).*/\1/p' | head -n1
    fi
}

# ---------------------------------------------------------------------------
# 数据采集
# ---------------------------------------------------------------------------
PROC_OK=0; PID=""; CPU_PCT="0.0"; MEM_PCT="0.0"; RSS_KB=0
THREADS=0; FDS=0; PROC_UPTIME=0
PREV_PROC_TICKS=-1; PREV_PROC_MS=0

CONN_OK=0; LQ="0 / 0"; TCP_ESTAB=0; TCP_TW=0; TCP_CW=0; TCP_SYN=0
HTTP_OK=0; HTTP_RES="not probed"

MYSQL_OK=0; QPS="-"; BUFFER_HIT="-"; declare -A ST=()
PREV_Q=-1; PREV_T=0; START_DEADLOCKS=-1

STATE_N=0; STATE_NAME=(); STATE_CNT=()
TABLE_LINE1=""; TABLE_LINE2=""
LOCK_N=0; LOCK_LINES=()

LEVEL="OK"; ALERT_TEXT="none"

gather_process() {
    PROC_OK=0; PID=""; CPU_PCT="0.0"; MEM_PCT="0.0"; RSS_KB=0
    THREADS=0; FDS=0; PROC_UPTIME=0

    PID="$(resolve_pid)"
    if [[ -z "$PID" || ! -d "/proc/$PID" ]]; then
        # 非 /proc 环境（如 Git Bash）退化为 ps。
        if [[ -n "$PID" ]] && have ps; then
            local cpu mem
            cpu="$(ps -p "$PID" -o %cpu= 2>/dev/null | awk 'NR==1{print $1+0}')"
            mem="$(ps -p "$PID" -o %mem= 2>/dev/null | awk 'NR==1{print $1+0}')"
            if [[ -n "$cpu" ]]; then
                PROC_OK=1
                CPU_PCT="${cpu:-0.0}"; MEM_PCT="${mem:-0.0}"
                THREADS="$(ps -p "$PID" -o nlwp= 2>/dev/null | awk 'NR==1{print $1+0}')"
                return
            fi
        fi
        return
    fi

    PROC_OK=1
    local status; status="$(cat "/proc/$PID/status" 2>/dev/null || true)"
    THREADS="$(awk '/^Threads:/{print $2}' <<<"$status")"
    RSS_KB="$(awk '/^VmRSS:/{print $2}' <<<"$status")"
    FDS="$(ls "/proc/$PID/fd" 2>/dev/null | wc -l | tr -d ' ')"
    local mem_total; mem_total="$(awk '/^MemTotal:/{print $2}' /proc/meminfo 2>/dev/null)"
    MEM_PCT="$(awk -v r="${RSS_KB:-0}" -v t="${mem_total:-0}" \
        'BEGIN{ if (t > 0) printf "%.1f", r*100/t; else print "0.0" }')"

    local rest utime stime starttime ticks
    rest="$(sed -E 's/^[0-9]+ \(.*\) //' "/proc/$PID/stat" 2>/dev/null || true)"
    # 去掉 "pid (comm)" 后，字段整体前移 2：utime=$12, stime=$13, starttime=$20
    utime="$(awk '{print $12}' <<<"$rest")"
    stime="$(awk '{print $13}' <<<"$rest")"
    starttime="$(awk '{print $20}' <<<"$rest")"
    ticks="$(getconf CLK_TCK 2>/dev/null || echo 100)"
    [[ "$ticks" =~ ^[0-9]+$ ]] || ticks=100

    local uptime_now; uptime_now="$(awk '{print $1}' /proc/uptime 2>/dev/null || echo 0)"
    PROC_UPTIME="$(awk -v u="$uptime_now" -v s="${starttime:-0}" -v h="$ticks" \
        'BEGIN{ v = u - s/h; if (v < 0) v = 0; printf "%d", v }')"

    local total_ticks=$(( ${utime:-0} + ${stime:-0} ))
    local ms; ms="$(now_ms)"
    if (( PREV_PROC_TICKS >= 0 && PREV_PROC_MS > 0 )); then
        local dticks=$((total_ticks - PREV_PROC_TICKS))
        local dms=$((ms - PREV_PROC_MS))
        if (( dms > 0 )); then
            CPU_PCT="$(awk -v d="$dticks" -v m="$dms" -v h="$ticks" \
                'BEGIN{ printf "%.1f", d*100000/(h*m) }')"
        fi
    fi
    PREV_PROC_TICKS=$total_ticks
    PREV_PROC_MS=$ms
}

gather_conn() {
    CONN_OK=0; LQ="0 / 0"; TCP_ESTAB=0; TCP_TW=0; TCP_CW=0; TCP_SYN=0
    if ! have ss; then return; fi

    local listen
    listen="$(ss -lntH "( sport = :$TCP_PORT )" 2>/dev/null | head -n1)"
    if [[ -n "$listen" ]]; then
        LQ="$(awk '{print ($2+0)" / "($3+0)}' <<<"$listen")"
    fi

    local all
    all="$(ss -tanH "( sport = :$TCP_PORT )" 2>/dev/null || true)"
    if [[ -n "$all" ]]; then
        CONN_OK=1
        read -r TCP_ESTAB TCP_TW TCP_CW TCP_SYN <<<"$(awk '
            { c[$1]++ }
            END { printf "%d %d %d %d", c["ESTAB"]+0, c["TIME-WAIT"]+0,
                                         c["CLOSE-WAIT"]+0, c["SYN-SENT"]+0 }
        ' <<<"$all")"
    else
        CONN_OK=1
    fi
}

gather_http() {
    HTTP_OK=0; HTTP_RES="unreachable"
    if have curl; then
        local out code t ms
        out="$(curl -s -o /dev/null --max-time 1 \
            -w '%{http_code} %{time_total}' \
            "http://$SVC_HOST:$HTTP_PORT/" 2>/dev/null || true)"
        if [[ -n "$out" ]]; then
            code="${out%% *}"; t="${out##* }"
            if [[ "$code" =~ ^[0-9]{3}$ && "$code" != "000" ]]; then
                ms="$(awk -v t="$t" 'BEGIN{ printf "%.0f", t*1000 }')"
                HTTP_RES="HTTP $code  ${ms}ms"
                HTTP_OK=1
            fi
        fi
        return          # curl 已给出结论，不再重复探测，保持界面刷新流畅
    fi

    local t0 t1 ok=0
    t0="$(now_ms)"
    if have timeout; then
        timeout 1 bash -c "exec 3<>/dev/tcp/$SVC_HOST/$HTTP_PORT" 2>/dev/null && ok=1
    elif (exec 3<>"/dev/tcp/$SVC_HOST/$HTTP_PORT") 2>/dev/null; then
        ok=1
    fi
    if (( ok )); then
        t1="$(now_ms)"
        HTTP_RES="TCP open  $((t1 - t0))ms"
        HTTP_OK=1
    fi
}

gather_mysql() {
    MYSQL_OK=0; QPS="-"; BUFFER_HIT="-"; ST=()
    have mysql || return

    local out
    out="$(mysql_q "SELECT VARIABLE_NAME, VARIABLE_VALUE
                    FROM information_schema.GLOBAL_STATUS
                    WHERE VARIABLE_NAME IN (
                      'Threads_connected','Threads_running','Max_used_connections',
                      'Questions','Slow_queries','Aborted_connects',
                      'Innodb_row_lock_current_waits','Innodb_row_lock_waits',
                      'Innodb_row_lock_time_avg','Innodb_deadlocks',
                      'Innodb_buffer_pool_read_requests','Innodb_buffer_pool_reads',
                      'Uptime','Connections','Threads_created')" 2>/dev/null)" || return

    local k v
    while IFS=$'\t' read -r k v; do
        [[ -n "$k" ]] && ST["${k,,}"]="$v"
    done <<<"$out"

    (( ${#ST[@]} == 0 )) && return
    MYSQL_OK=1

    local rr="${ST[innodb_buffer_pool_read_requests]:-0}"
    local rd="${ST[innodb_buffer_pool_reads]:-0}"
    BUFFER_HIT="$(awk -v rr="$rr" -v rd="$rd" \
        'BEGIN{ if (rr > 0) printf "%.2f", (1 - rd/rr)*100; else print "-" }')"

    local q="${ST[questions]:-}"
    local ms; ms="$(now_ms)"
    if [[ "$q" =~ ^[0-9]+$ ]]; then
        if (( PREV_Q >= 0 && PREV_T > 0 )); then
            local dq=$((q - PREV_Q)) dt=$((ms - PREV_T))
            if (( dt > 0 )); then
                QPS="$(awk -v d="$dq" -v t="$dt" 'BEGIN{ printf "%.1f", d*1000/t }')"
            fi
        else
            # 首次采样（或 --once）：用 Questions/Uptime 估算平均 QPS。
            local up="${ST[uptime]:-0}"
            if [[ "$up" =~ ^[0-9]+$ ]] && (( up > 0 )); then
                QPS="$(awk -v n="$q" -v u="$up" 'BEGIN{ printf "%.1f", n/u }')"
            fi
        fi
        PREV_Q="$q"
        PREV_T="$ms"
    fi

    local dl="${ST[innodb_deadlocks]:-0}"
    if [[ "$dl" =~ ^[0-9]+$ ]]; then
        if (( START_DEADLOCKS < 0 )); then
            START_DEADLOCKS="$dl"
        elif (( dl < START_DEADLOCKS )); then
            START_DEADLOCKS="$dl"    # DB 重启后计数器归零，重新取基线
        fi
    fi
}

gather_states() {
    STATE_N=0; STATE_NAME=(); STATE_CNT=()
    (( MYSQL_OK )) || return
    local out
    out="$(mysql_q "SELECT COALESCE(STATE,'(null)') AS s, COUNT(*) AS c
                    FROM information_schema.PROCESSLIST
                    GROUP BY s ORDER BY c DESC LIMIT 5" 2>/dev/null)" || return
    local a b
    while IFS=$'\t' read -r a b; do
        [[ -z "$a" ]] && continue
        STATE_NAME[STATE_N]="$a"
        STATE_CNT[STATE_N]="$b"
        STATE_N=$((STATE_N + 1))
    done <<<"$out"
}

gather_tables() {
    TABLE_LINE1="(unavailable)"; TABLE_LINE2=""
    (( MYSQL_OK )) || return

    local schema_expr
    if [[ -n "$MYSQL_DB" ]]; then schema_expr="'$MYSQL_DB'"; else schema_expr="DATABASE()"; fi

    local out
    out="$(mysql_q "SELECT TABLE_NAME, COALESCE(TABLE_ROWS,0)
                    FROM information_schema.TABLES
                    WHERE TABLE_SCHEMA=$schema_expr
                      AND TABLE_NAME IN ('users','friends','friend_requests',
                        'conversations','conversation_members','messages',
                        'offline_messages')" 2>/dev/null)" || return

    local -A TR=()
    local a b
    while IFS=$'\t' read -r a b; do
        [[ -n "$a" ]] && TR["$a"]="$b"
    done <<<"$out"
    (( ${#TR[@]} == 0 )) && { TABLE_LINE1="(no matching tables)"; return; }

    TABLE_LINE1="users $(human_num "${TR[users]:-0}")  |  friends $(human_num "${TR[friends]:-0}")  |  conversations $(human_num "${TR[conversations]:-0}")  |  messages $(human_num "${TR[messages]:-0}")"
    TABLE_LINE2="friend_requests $(human_num "${TR[friend_requests]:-0}")  |  conversation_members $(human_num "${TR[conversation_members]:-0}")  |  offline_messages $(human_num "${TR[offline_messages]:-0}")"
}

gather_locks() {
    LOCK_N=0; LOCK_LINES=()
    (( MYSQL_OK )) || return
    local out
    out="$(mysql_q "SELECT ID, TIME, LEFT(COALESCE(STATE,''),20),
                           LEFT(COALESCE(INFO,''),40)
                    FROM information_schema.PROCESSLIST
                    WHERE STATE LIKE '%lock%' OR STATE LIKE '%Waiting%'
                    ORDER BY TIME DESC LIMIT 3" 2>/dev/null)" || return
    local id t st info
    while IFS=$'\t' read -r id t st info; do
        [[ -z "$id" ]] && continue
        LOCK_LINES[LOCK_N]="ID $id | Time ${t}s | $st | $info"
        LOCK_N=$((LOCK_N + 1))
    done <<<"$out"
}

compute_alerts() {
    LEVEL="OK"; ALERT_TEXT="none"
    local -a al=()

    (( PROC_OK )) || { LEVEL="ALERT"; al+=("service process not found"); }

    if (( MYSQL_OK )); then
        local lw="${ST[innodb_row_lock_current_waits]:-0}"
        local dl="${ST[innodb_deadlocks]:-0}"
        local tr="${ST[threads_running]:-0}"
        if [[ "$lw" =~ ^[0-9]+$ ]] && (( lw > 0 )); then
            LEVEL="ALERT"; al+=("InnoDB lock-wait=$lw")
        fi
        if (( START_DEADLOCKS >= 0 )) && [[ "$dl" =~ ^[0-9]+$ ]] \
           && (( dl > START_DEADLOCKS )); then
            al+=("deadlocks +$((dl - START_DEADLOCKS))")
            [[ "$LEVEL" == "OK" ]] && LEVEL="WARN"
        fi
        if [[ "$tr" =~ ^[0-9]+$ ]] && (( tr > 10 )); then
            al+=("mysql running=$tr")
            [[ "$LEVEL" == "OK" ]] && LEVEL="WARN"
        fi
    else
        LEVEL="ALERT"; al+=("mysql unavailable")
    fi

    (( HTTP_OK )) || {
        [[ "$LEVEL" == "OK" || "$LEVEL" == "WARN" ]] && LEVEL="WARN"
        al+=("http probe failed")
    }
    (( LOCK_N > 0 )) && { LEVEL="ALERT"; al+=("lock-wait sessions=$LOCK_N"); }

    if (( ${#al[@]} > 0 )); then
        local joined="" a
        for a in "${al[@]}"; do
            [[ -n "$joined" ]] && joined+="; "
            joined+="$a"
        done
        ALERT_TEXT="$joined"
    fi
}

stv() { local k="${1,,}"; printf '%s' "${ST[$k]:-${2:-0}}"; }

# ---------------------------------------------------------------------------
# 绘制
# ---------------------------------------------------------------------------
FRAME=(); PREV=(); FIRST=1; PREV_WIDTH=0

build_frame() {
    gather_process
    gather_conn
    gather_http
    gather_mysql
    gather_states
    gather_tables
    gather_locks
    compute_alerts

    local now; now="$(date '+%F %T')"
    FRAME=()

    # ---- 头部 ----
    FRAME+=("$C_TITLE${D_TL}$(repeat_char "$D_H" $((WIDTH - 2)))${D_TR}$C_RESET")
    local hplain hpad hsp hcolor
    hplain="$(fit_plain "  KC-CXIM SERVICE MONITOR   STATUS:$LEVEL   $now   refresh=${INTERVAL}s" $((WIDTH - 2)))"
    hpad=$((WIDTH - 2 - ${#hplain}))
    printf -v hsp '%*s' "$hpad" ''
    hcolor="$C_OK"
    [[ "$LEVEL" == "WARN"  ]] && hcolor="$C_WARN"
    [[ "$LEVEL" == "ALERT" ]] && hcolor="$C_ALERT"
    FRAME+=("$D_V$hcolor$hplain$hsp$C_RESET$D_V")
    FRAME+=("$C_TITLE${D_BL}$(repeat_char "$D_H" $((WIDTH - 2)))${D_BR}$C_RESET")

    # ---- 服务进程 ----
    FRAME+=("$(panel_top 'SERVICE PROCESS')")
    if (( PROC_OK )); then
        local mem_mb upfmt pcolor
        mem_mb="$(awk -v k="${RSS_KB:-0}" 'BEGIN{ printf "%.1f", k/1024 }')"
        upfmt="$(fmt_duration "${PROC_UPTIME:-0}")"
        pcolor="$C_GREEN"
        awk -v c="${CPU_PCT:-0}" 'BEGIN{exit !(c >= 70)}' && pcolor="$C_RED"
        awk -v c="${CPU_PCT:-0}" 'BEGIN{exit !(c >= 30 && c < 70)}' && pcolor="$C_YELLOW"
        FRAME+=("$(panel_line "PID $PID | CPU ${CPU_PCT}% | MEM ${MEM_PCT}% (${mem_mb} MB) | THREADS ${THREADS:-0} | FDs ${FDS:-0} | UPTIME $upfmt" "$pcolor")")
    else
        FRAME+=("$(panel_line "Service process '$PROC_NAME' not found. Try --svc-pid <pid>." "$C_ALERT")")
    fi
    FRAME+=("$(panel_bottom)")

    # ---- 连接 ----
    FRAME+=("$(panel_top 'CONNECTIONS')")
    if (( CONN_OK )); then
        FRAME+=("$(panel_line "TCP :$TCP_PORT   listen-q $LQ   ESTAB $TCP_ESTAB   TIME-WAIT $TCP_TW   CLOSE-WAIT $TCP_CW   SYN-SENT $TCP_SYN" "$C_WHITE")")
    else
        FRAME+=("$(panel_line "TCP :$TCP_PORT   [ss unavailable / no data]" "$C_WARN")")
    fi
    local hcolor="$C_GREEN"
    (( HTTP_OK )) || hcolor="$C_ALERT"
    FRAME+=("$(panel_line "HTTP :$HTTP_PORT   $HTTP_RES   (root 404 is normal: service alive)" "$hcolor")")
    FRAME+=("$(panel_bottom)")

    # ---- MySQL 概览 ----
    FRAME+=("$(panel_top 'MYSQL OVERVIEW')")
    if (( MYSQL_OK )); then
        local mcolor="$C_GREEN"
        local lw; lw="$(stv Innodb_row_lock_current_waits 0)"
        [[ "$lw" =~ ^[0-9]+$ ]] && (( lw > 0 )) && mcolor="$C_ALERT"
        FRAME+=("$(panel_line "CONN $(stv Threads_connected) | RUNNING $(stv Threads_running) | MAX-USED $(stv Max_used_connections) | QPS $QPS | SLOW $(stv Slow_queries) | ABORTED $(stv Aborted_connects)" "$mcolor")")
        FRAME+=("$(panel_line "LOCK-WAIT $(stv Innodb_row_lock_current_waits) cur / $(stv Innodb_row_lock_waits) total | DEADLOCKS $(stv Innodb_deadlocks 0) | AVG-LOCK $(stv Innodb_row_lock_time_avg 0)ms | BUFFER-HIT ${BUFFER_HIT}% | UPTIME $(fmt_duration "$(stv Uptime 0)")" "$mcolor")")
    else
        FRAME+=("$(panel_line "MySQL unreachable: ${MYSQL_USER}@${MYSQL_HOST}:${MYSQL_PORT}/${MYSQL_DB:-<none>}" "$C_ALERT")")
        FRAME+=("$(panel_line "Check DBConfig.ini / mysql client / network." "$C_DIM")")
    fi
    FRAME+=("$(panel_bottom)")

    # ---- 会话状态 Top 5 ----
    FRAME+=("$(panel_top 'MYSQL SESSION STATES (TOP 5)')")
    local smax=1
    local i
    for ((i = 0; i < STATE_N; i++)); do
        [[ "${STATE_CNT[i]}" =~ ^[0-9]+$ ]] || continue
        (( STATE_CNT[i] > smax )) && smax="${STATE_CNT[i]}"
    done
    for ((i = 0; i < 5; i++)); do
        if (( i < STATE_N )); then
            local sname="${STATE_NAME[i]}" scnt="${STATE_CNT[i]}" bar row scol
            local blen=0
            if [[ "$scnt" =~ ^[0-9]+$ ]] && (( smax > 0 )); then
                blen=$(( scnt * 20 / smax ))
            fi
            bar="$(repeat_char '#' "$blen")"
            printf -v row '%-28.28s %5s  %s' "$sname" "$scnt" "$bar"
            scol="$C_WHITE"
            [[ "$sname" == *lock* || "$sname" == *Lock* || "$sname" == *Waiting* ]] && scol="$C_WARN"
            FRAME+=("$(panel_line "$row" "$scol")")
        else
            FRAME+=("$(panel_line "" )")
        fi
    done
    FRAME+=("$(panel_bottom)")

    # ---- 业务表规模 ----
    FRAME+=("$(panel_top 'IM TABLE SCALE (APPROX ROWS)')")
    FRAME+=("$(panel_line "$TABLE_LINE1" "$C_WHITE")")
    FRAME+=("$(panel_line "$TABLE_LINE2" "$C_WHITE")")
    FRAME+=("$(panel_bottom)")

    # ---- 锁等待会话 ----
    FRAME+=("$(panel_top 'LOCK WAIT SESSIONS (TOP 3)')")
    for ((i = 0; i < 3; i++)); do
        if (( i < LOCK_N )); then
            local lcolor="$C_YELLOW"
            [[ "${LOCK_LINES[i]}" =~ Time\ ([0-9]+)s ]] && (( BASH_REMATCH[1] > 5 )) && lcolor="$C_ALERT"
            FRAME+=("$(panel_line "${LOCK_LINES[i]}" "$lcolor")")
        elif (( i == 0 )); then
            FRAME+=("$(panel_line "No lock-wait / waiting sessions detected." "$C_GREEN")")
        else
            FRAME+=("$(panel_line "")")
        fi
    done
    FRAME+=("$(panel_bottom)")

    # ---- 底部：告警明细 + 刷新信息 ----
    local fcolor="$C_DIM"
    [[ "$LEVEL" == "WARN"  ]] && fcolor="$C_WARN"
    [[ "$LEVEL" == "ALERT" ]] && fcolor="$C_ALERT"

    local alert_line
    alert_line="$(fit_plain "ALERTS: $ALERT_TEXT" $((WIDTH - 2)))"
    FRAME+=("$fcolor$alert_line$C_RESET")

    local foot
    foot="$(fit_plain "Last update: $now   |   interval: ${INTERVAL}s   |   Ctrl+C to quit" $((WIDTH - 2)))"
    FRAME+=("$C_DIM$foot$C_RESET")
}

draw_frame() {
    local n=${#FRAME[@]}
    local pn=${#PREV[@]}
    local i

    if (( FIRST )); then
        printf '\033[2J\033[H'
        FIRST=0
    fi

    for ((i = 0; i < n; i++)); do
        if [[ "${FRAME[i]}" != "${PREV[i]:-}" ]]; then
            printf '\033[%d;1H%s\033[K' $((i + 1)) "${FRAME[i]}"
        fi
    done

    if (( n < pn )); then
        for ((i = n; i < pn; i++)); do
            printf '\033[%d;1H\033[K' $((i + 1))
        done
    fi

    PREV=("${FRAME[@]}")
    printf '\033[%d;1H' "$n"
}

cleanup() {
    printf '\033[?25h'          # 恢复光标
    printf '\033[%d;1H\n' "${#FRAME[@]}"
    echo "[exit] monitor stopped"
    exit 0
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
trap cleanup INT TERM

if (( ONCE )); then
    WIDTH="$(term_width)"
    build_frame
    for line in "${FRAME[@]}"; do printf '%s\n' "$line"; done
    exit 0
fi

printf '\033[?25l'              # 隐藏光标，避免刷新时闪烁
echo "[info] starting monitor, refresh every ${INTERVAL}s, Ctrl+C to quit"

while true; do
    WIDTH="$(term_width)"
    if (( WIDTH != PREV_WIDTH )); then
        FIRST=1
        PREV=()
        PREV_WIDTH="$WIDTH"
    fi
    build_frame
    draw_frame
    sleep "$INTERVAL"
done
