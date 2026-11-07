#!/usr/bin/env bash
# ============================================================================
# 一键验证脚本：
#   1) 后台启动服务器；
#   2) 运行测试客户端；
#   3) 检查客户端输出是否符合预期。
# 用法: ./verify_tcp.sh [port] [build_dir]
# ============================================================================
set -euo pipefail   # 出错即退出 / 未定义变量报错 / 管道任一环节失败即失败

PORT="${1:-8888}"                                  # 端口，默认 8888
BUILD_DIR="${2:-$(pwd)/build}"                     # 构建目录，默认当前目录下 build

if [[ "$(uname -s)" == Linux* ]]; then
    # Windows(Debug) 下的可执行文件名；Linux 下去掉 .exe 和 Debug。
    SERVER_BIN="${BUILD_DIR}/bin/tcp_echo_server"
    CLIENT_BIN="${BUILD_DIR}/bin/test_client"
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    # Windows(Debug) 下的可执行文件名；Linux 下去掉 .exe 和 Debug。
    SERVER_BIN="${BUILD_DIR}/bin/Debug/tcp_echo_server.exe"
    CLIENT_BIN="${BUILD_DIR}/bin/Debug/test_client.exe"
fi

# 找不到二进制就提示先构建。
if [[ ! -x "$SERVER_BIN" || ! -x "$CLIENT_BIN" ]]; then
    echo "[verify] 找不到可执行文件，请先构建:"
    echo "  cmake -S . -B build && cmake --build build -j"
    exit 1
fi

echo "[verify] 启动服务器 :$PORT"
"$SERVER_BIN" "$PORT" &
SERVER_PID=$!
# 无论脚本正常结束还是异常退出，都确保把后台服务器杀掉。
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# 等待端口就绪：最多等 20 次，每次 0.1 秒（约 2 秒）。
for i in $(seq 1 20); do
    if (echo > /dev/tcp/127.0.0.1/"$PORT") 2>/dev/null; then break; fi
    sleep 0.1
done

echo "[verify] 运行测试客户端"
OUT=$("$CLIENT_BIN" "$PORT")

echo "$OUT"

# ---- 断言：检查关键输出 -----------------------------------------------------
fail=0
# 旧纯文本用例已关闭，这里主要验证 JSON 登录 + 转发。
echo "$OUT" | grep -q "A ack:"   || { echo "FAIL: A login ack"; fail=1; }
echo "$OUT" | grep -q "B ack:"   || { echo "FAIL: B login ack"; fail=1; }
echo "$OUT" | grep -q "B recv: .*hi B" || { echo "FAIL: A->B route"; fail=1; }

if [[ $fail -eq 0 ]]; then
    echo "[verify] ALL PASS"
else
    echo "[verify] SOME FAILED"
    exit 1
fi
