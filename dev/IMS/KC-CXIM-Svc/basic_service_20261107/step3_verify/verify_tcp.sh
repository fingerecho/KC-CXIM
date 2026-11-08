#!/usr/bin/env bash
# ============================================================================
# 一键验收脚本（数据库接入版）：
#   1) 读取 DBConfig.ini，导入 step3_verify/seed_test_data.sql（清空测试数据）；
#   2) 后台启动服务器；
#   3) 运行自动化测试客户端；
#   4) 检查关键输出。
# 用法: ./verify_tcp.sh [port] [build_dir]
# ============================================================================
set -euo pipefail

PORT="${1:-8888}"
BUILD_DIR="${2:-$(pwd)/build}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${DB_CONFIG:-${PROJECT_DIR}/DBConfig.ini}"
SEED_SQL="${SCRIPT_DIR}/seed_test_data.sql"

# ---------------------------------------------------------------------------
# 极简 INI 解析（只取 [db] 段）
# ---------------------------------------------------------------------------
DB_HOST=''
DB_PORT=''
DB_NAME=''
DB_USER=''
DB_PASS=''

trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

parse_db_config() {
    local file="$1"
    local section='' line key val
    while IFS= read -r line || [[ -n "$line" ]]; do
        line="${line%$'\r'}"
        line="$(trim "$line")"
        [[ -z "$line" ]] && continue
        [[ "$line" == \#* || "$line" == \;* ]] && continue

        if [[ "$line" == \[*\] ]]; then
            section="${line#[}"; section="${section%]}"
            section="$(printf '%s' "$section" | tr '[:upper:]' '[:lower:]')"
            continue
        fi
        [[ "$section" == "db" ]] || continue
        [[ "$line" == *=* ]] || continue

        key="$(trim "${line%%=*}")"
        val="$(trim "${line#*=}")"
        key="$(printf '%s' "$key" | tr '[:upper:]' '[:lower:]')"

        case "$key" in
            host)     DB_HOST="$val" ;;
            port)     DB_PORT="$val" ;;
            dbname)   DB_NAME="$val" ;;
            user)     DB_USER="$val" ;;
            password) DB_PASS="$val" ;;
        esac
    done < "$file"
}

seed_db() {
    [[ -f "$SEED_SQL" ]] || { echo "[verify] 缺少 seed_test_data.sql，跳过"; return 0; }
    [[ -f "$CONFIG_FILE" ]] || { echo "[verify] 缺少 DBConfig.ini，跳过 seed"; return 0; }

    parse_db_config "$CONFIG_FILE"
    if [[ -z "$DB_NAME" || -z "$DB_USER" ]]; then
        echo "[verify] DBConfig.ini 缺少 dbname/user，跳过 seed"
        return 0
    fi

    # 优先使用项目自带的 seed_db（不依赖本机是否有 mysql 客户端）。
    if [[ -n "${SEED_BIN:-}" && -x "$SEED_BIN" ]]; then
        echo "[verify] 使用 seed_db 导入 schema + 测试数据（会清空测试表）..."
        if "$SEED_BIN" "$CONFIG_FILE" "${PROJECT_DIR}/db/schema.sql" "$SEED_SQL"; then
            echo "[verify] seed OK"
            return 0
        else
            echo "[verify] seed FAILED：请检查远程数据库连接与账号权限"
            return 1
        fi
    fi

    # 回退：如果本机有 mariadb/mysql 客户端，就用它导入测试数据。
    local client=''
    client="$(command -v mariadb || command -v mysql || true)"
    if [[ -z "$client" ]]; then
        echo "[verify] 既没有 seed_db 也没有 mariadb/mysql 客户端，无法准备测试数据"
        return 1
    fi

    echo "[verify] 导入测试数据（会清空测试表）..."
    if MYSQL_PWD="$DB_PASS" "$client" \
        -h "${DB_HOST:-127.0.0.1}" \
        -P "${DB_PORT:-3306}" \
        -u "$DB_USER" \
        "$DB_NAME" < "$SEED_SQL"; then
        echo "[verify] seed OK"
    else
        echo "[verify] seed FAILED：请确认 db/schema.sql 已导入、账号有权限"
        return 1
    fi
}

# ---------------------------------------------------------------------------
# 定位可执行文件
# ---------------------------------------------------------------------------
SEED_BIN=''
if [[ "$(uname -s)" == Linux* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/tcp_echo_server"
    CLIENT_BIN="${BUILD_DIR}/bin/test_client"
    SEED_BIN="${BUILD_DIR}/bin/seed_db"
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/Debug/tcp_echo_server.exe"
    CLIENT_BIN="${BUILD_DIR}/bin/Debug/test_client.exe"
    SEED_BIN="${BUILD_DIR}/bin/Debug/seed_db.exe"
fi

if [[ ! -x "$SERVER_BIN" || ! -x "$CLIENT_BIN" ]]; then
    echo "[verify] 找不到可执行文件，请先构建:"
    echo "  bash build_step.sh"
    exit 1
fi

# 先准备测试数据，再启动服务器。
seed_db

echo "[verify] 启动服务器 :$PORT"
# 显式把配置文件路径传给服务器，避免依赖当前工作目录。
"$SERVER_BIN" "$PORT" "$CONFIG_FILE" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# 等端口就绪（最多约 5 秒）
for _ in $(seq 1 50); do
    if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
    sleep 0.1
done

echo "[verify] 运行测试客户端"
if command -v timeout >/dev/null 2>&1; then
    OUT="$(timeout 60 "$CLIENT_BIN" "$PORT" || true)"
else
    OUT="$("$CLIENT_BIN" "$PORT" || true)"
fi

echo "$OUT"

# ---------------------------------------------------------------------------
# 断言
# ---------------------------------------------------------------------------
fail=0
check() {
    local pattern="$1" desc="$2"
    # 用 here-string 避免 `echo | grep -q` 在 pipefail 下的 SIGPIPE 误判。
    if grep -q "$pattern" <<<"$OUT"; then
        echo "  PASS: $desc"
    else
        echo "  FAIL: $desc (missing: $pattern)"
        fail=1
    fi
}

check "A ack:"                              "A 登录成功"
check "B ack:"                              "B 登录成功"
check "B recv: hi B"                        "A -> B 在线消息"
check "duplicate ignored"                   "client_msg_id 去重"
check "offline delivered: offline-1"        "离线消息落库并补推"
check "history count: 2"                    "历史分页（两条）"
check "invalid user rejected"               "非法用户被拒绝"
check "after-reconnect"                     "断线重连并继续发送"

if [[ $fail -eq 0 ]]; then
    echo "[verify] ALL PASS"
else
    echo "[verify] SOME FAILED"
    exit 1
fi
