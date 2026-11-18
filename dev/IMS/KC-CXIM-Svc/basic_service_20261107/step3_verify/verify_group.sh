#!/usr/bin/env bash
# ============================================================================
# /*************群聊功能：群聊模块一键验收脚本（BE-GROUP-08 等价的验收入口）***************/
# ----------------------------------------------------------------------------
# 覆盖链路：
#   1) 确保数据库已应用最新 db/schema.sql（包含 groups/group_members/group_requests）；
#   2) 启动 TCP + HTTP 服务；
#   3) 运行 test_group_client：
#        HTTP 建群/列表/详情/资料/邀请/移出/任命/转让/解散/入群申请
#        TCP  群消息实时投递、离线写扩散补推、client_msg_id 去重、群历史、群事件推送
#
# 用法: ./verify_group.sh [tcp_port] [build_dir] [http_port]
#   默认: tcp=8888  build=$(pwd)/build  http=8080
#
# 说明：
#   * 只执行 schema.sql（全部是 CREATE TABLE IF NOT EXISTS，可重复执行），
#     不导入 seed_test_data.sql，不会清空你正在使用的业务表；
#   * 测试账号由客户端随机生成（grpA_/grpB_/grpC_/grpD_ 前缀），不会污染固定账号。
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
# 定位可执行文件（与 verify_friend.sh 保持一致）
# ---------------------------------------------------------------------------
SERVER_BIN=''
CLIENT_BIN=''
SEED_BIN=''
if [[ "$(uname -s)" == Linux* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/cxim-server"
    CLIENT_BIN="${BUILD_DIR}/bin/test_group_client"
    SEED_BIN="${BUILD_DIR}/bin/seed_db"
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/Release/cxim-server.exe"
    CLIENT_BIN="${BUILD_DIR}/bin/Release/test_group_client.exe"
    SEED_BIN="${BUILD_DIR}/bin/Release/seed_db.exe"
fi

if [[ ! -x "$SERVER_BIN" || ! -x "$CLIENT_BIN" ]]; then
    echo "[verify-group] 找不到可执行文件:"
    echo "  server=$SERVER_BIN"
    echo "  client=$CLIENT_BIN"
    echo "  bash build.sh"
    exit 1
fi

# Windows 下 OpenSSL 运行时 DLL 在 vcpkg 目录，加入 PATH 才能启动服务器。
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    export PATH="${PROJECT_DIR}/vcpkg_installed/x64-windows/bin:${PATH}"
fi

# ---------------------------------------------------------------------------
# 应用 schema：保证 groups/group_members/group_requests 表存在（可重复执行）
# ---------------------------------------------------------------------------
if [[ -x "$SEED_BIN" ]]; then
    echo "[verify-group] 应用 schema.sql ..."
    "$SEED_BIN" "$CONFIG_FILE" "$SCHEMA_SQL"
else
    echo "[verify-group] 未找到 seed_db，跳过 schema 自动导入（请确认表已存在）"
fi

export DB_CONFIG="$CONFIG_FILE"
export JWT_CONFIG="$JWT_FILE"
export TCP_PORT HTTP_PORT

# ---------------------------------------------------------------------------
# P0 校验：端口必须空着（否则下面验收到的是"别人的服务器"）
# ---------------------------------------------------------------------------
# 这个脚本用 /dev/tcp 探端口来判断"自己起的服务就绪"。但如果端口已经被
# 上一个实例占着（手工起的、或者验收脚本上次没退干净的），那么：
#   * 本脚本的服务器会 bind 失败并退出；
#   * 探端口却立刻成功（应答的是那个老实例）；
#   * 于是"服务器就绪"通过，客户端连的是一个你可能根本不知道存在、
#     而且跑着旧二进制/旧环境变量的服务。
# 表现为"HTTP 明明在监听却全是奇怪的结果"，排查半天才发现测错了对象。
# 所以这里先要求两个端口都空着，并且就绪判定同时盯着我们自己那个进程是否还活着。
for p in "$TCP_PORT" "$HTTP_PORT"; do
    if (echo > /dev/tcp/127.0.0.1/"$p") 2>/dev/null; then
        echo "[verify-group] 端口 :$p 已被占用，先停掉占用它的进程（或换端口）："
        echo "  Windows: netstat -ano | grep :$p   →  taskkill //PID <pid> //F"
        echo "  Linux  : ss -ltnp | grep :$p"
        exit 1
    fi
done

echo "[verify-group] 启动服务器 tcp=:$TCP_PORT http=:$HTTP_PORT"
echo "[verify-group] 可执行文件 server=$SERVER_BIN"
( cd "$PROJECT_DIR" && exec "$SERVER_BIN" "$TCP_PORT" "$CONFIG_FILE" "$HTTP_PORT" ) \
    > "${TMPDIR:-/tmp}/verify_group_server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# 等 TCP 与 HTTP 两个端口就绪（最多约 10 秒）。
# 每轮都先确认**我们启动的那个进程还在**：它一死（端口被占、DB 连不上、
# 配置读错）就立刻失败并把日志打出来，而不是继续等一个陌生实例的端口。
ready=0
for _ in $(seq 1 100); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[verify-group] 服务器进程（pid=$SERVER_PID）已退出，日志如下："
        cat "${TMPDIR:-/tmp}/verify_group_server.log" || true
        exit 1
    fi
    if (echo > /dev/tcp/127.0.0.1/"$TCP_PORT") 2>/dev/null &&
       (echo > /dev/tcp/127.0.0.1/"$HTTP_PORT") 2>/dev/null; then
        ready=1
        break
    fi
    sleep 0.1
done
if [[ $ready -ne 1 ]]; then
    echo "[verify-group] 服务器端口未就绪，日志如下："
    cat "${TMPDIR:-/tmp}/verify_group_server.log" || true
    exit 1
fi

# ---------------------------------------------------------------------------
# 打印本次实际生效的运行期参数（readme §11.6 的第 3 条检查项）
# ---------------------------------------------------------------------------
# 这批性能改动全部由环境变量控制（BCRYPT_COST / DB_WORKER_COUNT /
# DB_HASH_WORKER_COUNT / OFFLINE_CURSOR / RELATION_CACHE_TTL / AUTH_RATE_* …）。
# 参数没生效时现象往往是"优化做了但曲线没变"，所以验收开头就把服务端的自述打出来，
# 省掉一轮"到底跑的是哪套配置"的争论。
echo "[verify-group] 服务端自述参数："
grep -E "\[db\] pool max|\[server\] (listening|io threads)|\[http\] listening" \
    "${TMPDIR:-/tmp}/verify_group_server.log" || echo "  (日志里没找到参数行)"

echo "[verify-group] 运行群聊模块验收客户端"
set +e
if command -v timeout >/dev/null 2>&1; then
    OUT="$(timeout 180 "$CLIENT_BIN" "$HTTP_PORT" "$TCP_PORT" 2>&1)"
    RC=$?
else
    OUT="$("$CLIENT_BIN" "$HTTP_PORT" "$TCP_PORT" 2>&1)"
    RC=$?
fi
set -e

echo "$OUT"

if [[ $RC -ne 0 ]] || ! grep -q "ALL PASS" <<<"$OUT"; then
    echo "[verify-group] 验收失败，服务器日志："
    cat "${TMPDIR:-/tmp}/verify_group_server.log" || true
    echo "[verify-group] 排查提示（readme §11.5 过载响应表 / §11.6 部署检查清单）："
    echo "  * 客户端报 HTTP 429/503 时，那是服务端在**正确地拒绝过载**，不是功能坏了；"
    echo "    本轮用的是验收账号（个位数用户），正常不该触发，触发了就检查"
    echo "    AUTH_RATE_PER_SEC / AUTH_IP_PER_SEC / HTTP_MAX_INFLIGHT 是否被调得过低。"
    echo "  * 若报 connection reset / 收不到响应：先确认你连的是本脚本刚起的那个进程"
    echo "    （上面已打印它的可执行文件与自述参数），以及用的是新构建的 Release 二进制。"
    echo "  * TCP 侧被踢（idle timeout）：检查 IDLE_TIMEOUT_SEC 与客户端心跳间隔的关系"
    echo "    （心跳必须显著小于空闲超时，建议 30s 对 60s）。"
    exit 1
fi

echo "[verify-group] ALL PASS"
# /*************群聊功能：群聊模块一键验收脚本结束***************/
