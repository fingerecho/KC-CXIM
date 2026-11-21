#!/usr/bin/env bash
# ============================================================================
# /*************图片文件功能（七牛云直传）：文件模块一键验收脚本***************/
# ----------------------------------------------------------------------------
# 覆盖链路（对应 IMS_llm_prompt_word_info.txt 规划的"验收标准"1~7）：
#   1) 确保数据库已应用最新 db/schema.sql（包含 files 表；幂等，可重复执行）；
#   2) 启动 TCP + HTTP 服务（**显式 QINIU_ENABLED=0**：mock 存储模式，
#      不依赖七牛账号即可验收全部控制面行为；真实模式见 readme §5.17 切换说明）；
#   3) 先跑 qiniu 签名单测（三类签名逐字节断言，失败则不再往下）；
#   4) 运行 test_file_client：
#        upload-token → 直传(mock) → commit → 单聊 type=2 → 接收端 302 换取
#        → 下载核对 size/qetag → 群聊 type=4 → 离线补推 → 历史分页含元数据
#        → 去重 → 负向（超限/黑名单 mime/虚报 commit/冒用 file_id/外链改写/
#          非 JSON content/未登录 401）→ 删除。
#
# 用法: ./verify_file.sh [tcp_port] [build_dir] [http_port]
#   默认: tcp=8888  build=$(pwd)/build  http=8080
#
# 说明：
#   * 只执行 schema.sql（全部 CREATE TABLE IF NOT EXISTS），不动业务数据；
#   * 测试账号随机前缀 filA_/filB_，不污染固定账号；
#   * FILE_BASE_URL 显式设为 http://127.0.0.1:$HTTP_PORT：消息元数据里的
#     换取入口必须指向**客户端可达**的地址，验收在单机回环上跑就指回环。
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
# 定位可执行文件（与 verify_group.sh 保持一致）
# ---------------------------------------------------------------------------
SERVER_BIN=''
CLIENT_BIN=''
SIGNER_BIN=''
SEED_BIN=''
if [[ "$(uname -s)" == Linux* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/cxim-server"
    CLIENT_BIN="${BUILD_DIR}/bin/test_file_client"
    SIGNER_BIN="${BUILD_DIR}/bin/test_qiniu_signer"
    SEED_BIN="${BUILD_DIR}/bin/seed_db"
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/Release/cxim-server.exe"
    CLIENT_BIN="${BUILD_DIR}/bin/Release/test_file_client.exe"
    SIGNER_BIN="${BUILD_DIR}/bin/Release/test_qiniu_signer.exe"
    SEED_BIN="${BUILD_DIR}/bin/Release/seed_db.exe"
fi

if [[ ! -x "$SERVER_BIN" || ! -x "$CLIENT_BIN" ]]; then
    echo "[verify-file] 找不到可执行文件:"
    echo "  server=$SERVER_BIN"
    echo "  client=$CLIENT_BIN"
    echo "  先构建: bash build.sh"
    exit 1
fi

# Windows 下 OpenSSL 运行时 DLL 在 vcpkg 目录，加入 PATH 才能启动服务器。
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    export PATH="${PROJECT_DIR}/vcpkg_installed/x64-windows/bin:${PATH}"
fi

# ---------------------------------------------------------------------------
# 第 0 步：qiniu 三类签名单测（纯本地计算，失败说明签名实现被改坏，直接终止）
# ---------------------------------------------------------------------------
if [[ -x "$SIGNER_BIN" ]]; then
    echo "[verify-file] 运行签名独立单测 test_qiniu_signer ..."
    "$SIGNER_BIN" > "${TMPDIR:-/tmp}/verify_file_signer.log" 2>&1 || {
        echo "[verify-file] 签名单测失败，日志："
        cat "${TMPDIR:-/tmp}/verify_file_signer.log"
        exit 1
    }
    grep -q "ALL PASS" "${TMPDIR:-/tmp}/verify_file_signer.log" || {
        echo "[verify-file] 签名单测未通过（缺 ALL PASS），日志："
        cat "${TMPDIR:-/tmp}/verify_file_signer.log"
        exit 1
    }
    echo "[verify-file] 签名单测 ALL PASS"
else
    echo "[verify-file] 警告：未找到 $SIGNER_BIN，跳过签名单测"
fi

# ---------------------------------------------------------------------------
# 应用 schema：保证 files 表存在（可重复执行）
# ---------------------------------------------------------------------------
if [[ -x "$SEED_BIN" ]]; then
    echo "[verify-file] 应用 schema.sql ..."
    "$SEED_BIN" "$CONFIG_FILE" "$SCHEMA_SQL"
else
    echo "[verify-file] 未找到 seed_db，跳过 schema 自动导入（请确认 files 表已存在）"
fi

export DB_CONFIG="$CONFIG_FILE"
export JWT_CONFIG="$JWT_FILE"
export TCP_PORT HTTP_PORT

########## 图片文件功能（七牛云直传）：mock 模式验收 ##########
# QINIU_ENABLED=0：ObjectStore 用内存模拟七牛语义（凭证/存在性/签名下载/断连），
# 验收不需要七牛账号。真实模式验收 = 配好 QiniuConfig.json 后把这里改 1
# 并按 readme §5.17 手动跑一次 curl 时序（test_file_client 的 mock-put 步骤
# 在真实模式下应替换为向 uploadUrl 的表单直传）。
export QINIU_ENABLED=0
export FILE_BASE_URL="http://127.0.0.1:${HTTP_PORT}"
########## 图片文件功能：环境变量结束 ##########

# ---------------------------------------------------------------------------
# P0 校验：端口必须空着（同 verify_group.sh：否则测到的是"别人的服务器"）
# ---------------------------------------------------------------------------
for p in "$TCP_PORT" "$HTTP_PORT"; do
    if (echo > /dev/tcp/127.0.0.1/"$p") 2>/dev/null; then
        echo "[verify-file] 端口 :$p 已被占用，先停掉占用它的进程（或换端口）："
        echo "  Windows: netstat -ano | grep :$p   →  taskkill //PID <pid> //F"
        echo "  Linux  : ss -ltnp | grep :$p"
        exit 1
    fi
done

echo "[verify-file] 启动服务器 tcp=:$TCP_PORT http=:$HTTP_PORT (QINIU_ENABLED=0 mock)"
echo "[verify-file] 可执行文件 server=$SERVER_BIN"
( cd "$PROJECT_DIR" && exec "$SERVER_BIN" "$TCP_PORT" "$CONFIG_FILE" "$HTTP_PORT" ) \
    > "${TMPDIR:-/tmp}/verify_file_server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# 等 TCP 与 HTTP 两个端口就绪（最多约 10 秒），同时盯着我们自己的进程。
ready=0
for _ in $(seq 1 100); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[verify-file] 服务器进程（pid=$SERVER_PID）已退出，日志如下："
        cat "${TMPDIR:-/tmp}/verify_file_server.log" || true
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
    echo "[verify-file] 服务器端口未就绪，日志如下："
    cat "${TMPDIR:-/tmp}/verify_file_server.log" || true
    exit 1
fi

# 打印本次实际生效的运行期参数（含文件功能自述 [file] 行）。
echo "[verify-file] 服务端自述参数："
grep -E "\[db\] pool max|\[server\] (listening|io threads)|\[http\] listening|\[file\]" \
    "${TMPDIR:-/tmp}/verify_file_server.log" || echo "  (日志里没找到参数行)"

echo "[verify-file] 运行文件模块验收客户端 test_file_client"
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
    echo "[verify-file] 验收失败，服务器日志尾部："
    tail -n 60 "${TMPDIR:-/tmp}/verify_file_server.log" || true
    echo "[verify-file] 排查提示："
    echo "  * 先确认 files 表存在（本脚本开头会用 seed_db 应用 schema.sql）；"
    echo "  * 负向用例报 200 = 闸门失效：检查 FILE_MAX_SIZE_MB / FILE_TYPE_STRICT；"
    echo "  * 302/下载失败：确认 FILE_BASE_URL 与实际 HTTP_PORT 一致；"
    echo "  * mock 端点在 QINIU_ENABLED=1 时全部 404——本脚本必须跑 mock。"
    exit 1
fi

echo "[verify-file] ALL PASS"
# /*************图片文件功能（七牛云直传）：文件模块一键验收脚本结束***************/
