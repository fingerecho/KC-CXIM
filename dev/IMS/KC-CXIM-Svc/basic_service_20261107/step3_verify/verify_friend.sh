#!/usr/bin/env bash
# ============================================================================
# 好友模块一键验收脚本（BE-FRIEND-08）
# ----------------------------------------------------------------------------
# 覆盖链路：
#   1) 确保数据库已应用最新 db/schema.sql（包含 friend_requests 表）；
#   2) 启动 TCP + HTTP 服务；
#   3) 运行 test_friend_client：
#        HTTP 注册/登录/搜索/加好友/通过/拒绝/列表/备注/删除/拉黑
#        TCP  实时接收 FriendRequest / FriendRequestResult / FriendListChanged
#
# 用法: ./verify_friend.sh [tcp_port] [build_dir] [http_port]
#   默认: tcp=8888  build=$(pwd)/build  http=8080
#
# 说明：
#   * 本脚本不导入 seed_test_data.sql，避免清空你正在使用的业务表；
#     只执行 schema.sql（全部是 CREATE TABLE IF NOT EXISTS，可重复执行）。
#   * 测试账号由客户端随机生成，不会污染固定测试账号。
# ============================================================================
set -euo pipefail

TCP_PORT="${1:-8888}"
BUILD_DIR="${2:-$(pwd)/build}"
HTTP_PORT="${3:-8080}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${DB_CONFIG:-${PROJECT_DIR}/DBConfig.ini}"
JWT_FILE="${JWT_CONFIG:-${PROJECT_DIR}/JWTConfig.json}"
SCHEMA_SQL="${PROJECT_DIR}/db/schema.sql"

# ---------------------------------------------------------------------------
# 定位可执行文件（与 verify_tcp.sh / verify_tcp_http.sh 保持一致）
# ---------------------------------------------------------------------------
SERVER_BIN=''
CLIENT_BIN=''
SEED_BIN=''
if [[ "$(uname -s)" == Linux* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/tcp_echo_server"
    CLIENT_BIN="${BUILD_DIR}/bin/test_friend_client"
    SEED_BIN="${BUILD_DIR}/bin/seed_db"
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/Debug/tcp_echo_server.exe"
    CLIENT_BIN="${BUILD_DIR}/bin/Debug/test_friend_client.exe"
    SEED_BIN="${BUILD_DIR}/bin/Debug/seed_db.exe"
fi

if [[ ! -x "$SERVER_BIN" || ! -x "$CLIENT_BIN" ]]; then
    echo "[verify-friend] 找不到可执行文件:"
    echo "  server=$SERVER_BIN"
    echo "  client=$CLIENT_BIN"
    echo "  bash build_step.sh"
    exit 1
fi

# Windows 下 OpenSSL 运行时 DLL 在 vcpkg 目录，加入 PATH 才能启动服务器。
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    export PATH="${PROJECT_DIR}/vcpkg_installed/x64-windows/bin:${PATH}"
fi

# ---------------------------------------------------------------------------
# 应用 schema：保证 friend_requests 表存在（可重复执行）
# ---------------------------------------------------------------------------
if [[ -x "$SEED_BIN" ]]; then
    echo "[verify-friend] 应用 schema.sql ..."
    "$SEED_BIN" "$CONFIG_FILE" "$SCHEMA_SQL"
else
    echo "[verify-friend] 未找到 seed_db，跳过 schema 自动导入（请确认表已存在）"
fi

export DB_CONFIG="$CONFIG_FILE"
export JWT_CONFIG="$JWT_FILE"
export TCP_PORT HTTP_PORT

echo "[verify-friend] 启动服务器 tcp=:$TCP_PORT http=:$HTTP_PORT"
( cd "$PROJECT_DIR" && exec "$SERVER_BIN" "$TCP_PORT" "$CONFIG_FILE" "$HTTP_PORT" ) \
    > "${TMPDIR:-/tmp}/verify_friend_server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# 等 TCP 与 HTTP 两个端口就绪（最多约 10 秒）
ready=0
for _ in $(seq 1 100); do
    if (echo > /dev/tcp/127.0.0.1/"$TCP_PORT") 2>/dev/null &&
       (echo > /dev/tcp/127.0.0.1/"$HTTP_PORT") 2>/dev/null; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ $ready -ne 1 ]]; then
    echo "[verify-friend] 服务器端口未就绪，日志如下："
    cat "${TMPDIR:-/tmp}/verify_friend_server.log" || true
    exit 1
fi

echo "[verify-friend] 运行好友模块验收客户端"
set +e
if command -v timeout >/dev/null 2>&1; then
    OUT="$(timeout 120 "$CLIENT_BIN" "$HTTP_PORT" "$TCP_PORT" 2>&1)"
    RC=$?
else
    OUT="$("$CLIENT_BIN" "$HTTP_PORT" "$TCP_PORT" 2>&1)"
    RC=$?
fi
set -e

echo "$OUT"

if [[ $RC -ne 0 ]] || ! grep -q "ALL PASS" <<<"$OUT"; then
    echo "[verify-friend] 验收失败，服务器日志："
    cat "${TMPDIR:-/tmp}/verify_friend_server.log" || true
    exit 1
fi

echo "[verify-friend] ALL PASS"
