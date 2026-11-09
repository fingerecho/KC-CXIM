#!/usr/bin/env bash
# ============================================================================
# 注册/登录 + 双用户聊天 + 离线补推 一键验收脚本
# ----------------------------------------------------------------------------
# 覆盖的业务链路：
#   1) 通过 HTTP 注册两个用户；
#   2) 通过 HTTP 登录两个用户，拿到 JWT；
#   3) 两个用户各自建立 TCP 长连接，用 JWT 完成 TCP 登录；
#   4) 双方在线互发消息；
#   5) 登出（关闭 TCP 连接）其中一个用户；
#   6) 另一个用户继续给离线用户发消息（应落库为离线消息）；
#   7) 离线用户重新登录，服务端补推离线消息；
#   8) 双方再次互发消息；
#   9) 最后两个用户都登出。
#
# 用法: ./verify_tcp_http.sh [tcp_port] [build_dir] [http_port]
#   默认: 8888                $(pwd)/build  8080
#
# 说明：TCP 协议没有独立的 Logout 消息类型，"登出"等价于关闭长连接，
#       服务端在 socket 关闭时会从在线表移除该用户。
# ============================================================================
set -euo pipefail

TCP_PORT="${1:-8888}"
BUILD_DIR="${2:-$(pwd)/build}"
HTTP_PORT="${3:-8080}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${DB_CONFIG:-${PROJECT_DIR}/DBConfig.ini}"
JWT_FILE="${JWT_CONFIG:-${PROJECT_DIR}/JWTConfig.json}"

# ---------------------------------------------------------------------------
# 定位可执行文件（与 verify_tcp.sh 保持一致）
# ---------------------------------------------------------------------------
SERVER_BIN=''
if [[ "$(uname -s)" == Linux* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/tcp_echo_server"
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    SERVER_BIN="${BUILD_DIR}/bin/Debug/tcp_echo_server.exe"
fi

if [[ ! -x "$SERVER_BIN" ]]; then
    echo "[verify] 找不到服务器可执行文件: $SERVER_BIN"
    echo "  bash build_step.sh"
    exit 1
fi

# ---------------------------------------------------------------------------
# Python：负责 HTTP 注册/登录 + TCP 帧协议的聊天流程
# ---------------------------------------------------------------------------
PYTHON="$(command -v python3 || command -v python || true)"
if [[ -z "$PYTHON" ]]; then
    echo "[verify] 找不到 python3/python，无法执行 TCP 帧协议测试"
    exit 1
fi

# Windows 下 OpenSSL 运行时 DLL 在 vcpkg 目录，加入 PATH 才能启动服务器。
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* || "$(uname -s)" == CYGWIN* ]]; then
    export PATH="${PROJECT_DIR}/vcpkg_installed/x64-windows/bin:${PATH}"
fi

export DB_CONFIG="$CONFIG_FILE"
export JWT_CONFIG="$JWT_FILE"
export TCP_PORT HTTP_PORT
# 强制 Python 用 UTF-8 输出，避免 Windows 下中文提示乱码。
export PYTHONIOENCODING="utf-8"

echo "[verify] 启动服务器 tcp=:$TCP_PORT http=:$HTTP_PORT"
# 切到项目根目录，保证相对路径（如 JWTConfig.json）可被找到。
( cd "$PROJECT_DIR" && exec "$SERVER_BIN" "$TCP_PORT" "$CONFIG_FILE" "$HTTP_PORT" ) \
    > "${TMPDIR:-/tmp}/verify_tcp_http_server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

# 等待 TCP 与 HTTP 两个端口就绪（最多约 10 秒）。
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
    echo "[verify] 服务器端口未就绪，日志如下："
    cat "${TMPDIR:-/tmp}/verify_tcp_http_server.log" || true
    exit 1
fi

echo "[verify] 运行 HTTP + TCP 业务流程"
set +e
"$PYTHON" - <<'PY'
import json, os, socket, struct, time, random, urllib.request, urllib.error

TCP_PORT = int(os.environ["TCP_PORT"])
HTTP_PORT = int(os.environ["HTTP_PORT"])
BASE = "http://127.0.0.1:%d" % HTTP_PORT
PASSWORD = "secret123"

fail = 0
def check(cond, desc, extra=""):
    global fail
    if cond:
        print("  PASS:", desc)
    else:
        print("  FAIL:", desc, extra)
        fail += 1

def http_post(path, payload):
    data = json.dumps(payload, separators=(",", ":")).encode()
    req = urllib.request.Request(BASE + path, data=data,
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, json.load(e)

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf

def send_frame(sock, obj):
    body = json.dumps(obj, separators=(",", ":")).encode()
    sock.sendall(struct.pack("!I", len(body)) + body)

def recv_frame(sock):
    n = struct.unpack("!I", recv_exact(sock, 4))[0]
    return json.loads(recv_exact(sock, n))

class Conn:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", TCP_PORT), timeout=20)
        self.s.settimeout(20)
        self.buf = []

    def send(self, obj):
        send_frame(self.s, obj)

    def wait(self, pred, desc, timeout=20):
        for i, m in enumerate(self.buf):
            if pred(m):
                return self.buf.pop(i)
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.s.settimeout(max(0.1, deadline - time.time()))
            m = recv_frame(self.s)
            if pred(m):
                return m
            self.buf.append(m)
        raise TimeoutError("timeout waiting for %s; buffered=%r" % (desc, self.buf))

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

# ---- 1) 注册两个用户（用户名带随机后缀，避免与历史数据冲突）----------------
suffix = "%d%04d" % (int(time.time()), random.randint(0, 9999))
user_a = "verifyA_" + suffix
user_b = "verifyB_" + suffix

st, r = http_post("/api/register", {"username": user_a, "password": PASSWORD})
check(st == 200 and r.get("code") == 0, "HTTP 注册用户A", str(r))
uid_a = r.get("data", {}).get("userId")

st, r = http_post("/api/register", {"username": user_b, "password": PASSWORD})
check(st == 200 and r.get("code") == 0, "HTTP 注册用户B", str(r))
uid_b = r.get("data", {}).get("userId")

# ---- 2) HTTP 登录，获取 JWT -------------------------------------------------
st, r = http_post("/api/login", {"username": user_a, "password": PASSWORD})
check(st == 200 and r.get("code") == 0 and bool(r.get("data", {}).get("token")),
      "HTTP 登录用户A并签发 token", str(r))
token_a = r.get("data", {}).get("token")

st, r = http_post("/api/login", {"username": user_b, "password": PASSWORD})
check(st == 200 and r.get("code") == 0 and bool(r.get("data", {}).get("token")),
      "HTTP 登录用户B并签发 token", str(r))
token_b = r.get("data", {}).get("token")

# ---- 3) TCP 登录 -----------------------------------------------------------
def tcp_login(token, uid):
    c = Conn()
    c.send({"from": uid, "to": 0, "type": 100, "content": token})
    m = c.wait(lambda x: x.get("type") in (101, 102), "LoginAck")
    if m.get("type") != 101:
        raise RuntimeError("TCP 登录失败: " + str(m))
    return c

def send_text(c, to_uid, text, mid):
    c.send({"from": 0, "to": to_uid, "type": 1, "msg_id": mid, "content": text})
    m = c.wait(lambda x: x.get("type") in (113, 102), "SendAck/Error")
    if m.get("type") == 102:
        raise RuntimeError("发送失败: " + str(m.get("content")))
    return m

def wait_text(c, text):
    return c.wait(lambda m: m.get("type") == 1 and m.get("content") == text,
                  "text:" + text)

def ack_delivered(c, msg):
    c.send({"from": 0, "to": 0, "type": 112,
            "conversation_id": msg["conversation_id"],
            "seq": msg["seq"], "content": "delivered"})

a = tcp_login(token_a, uid_a)
check(True, "用户A TCP 登录成功")
b = tcp_login(token_b, uid_b)
check(True, "用户B TCP 登录成功")

# ---- 4) 双方在线互发消息 ---------------------------------------------------
mid = 1
send_text(a, uid_b, "A-to-B-online", mid); mid += 1
wait_text(b, "A-to-B-online")
check(True, "A -> B 在线消息送达")

send_text(b, uid_a, "B-to-A-online", mid); mid += 1
wait_text(a, "B-to-A-online")
check(True, "B -> A 在线消息送达")

# ---- 5) 登出 B（关闭长连接），等服务端从在线表移除 -------------------------
b.close()
time.sleep(1.0)
check(True, "用户B 登出（关闭 TCP 连接）")

# ---- 6) A 给离线的 B 发消息（应写入离线队列）-------------------------------
send_text(a, uid_b, "A-to-B-offline", mid); mid += 1
check(True, "A -> 离线B 消息已被服务端接收（SendAck sent）")

# ---- 7) B 重新登录，服务端补推离线消息 -------------------------------------
b = tcp_login(token_b, uid_b)
offline = wait_text(b, "A-to-B-offline")
check(True, "用户B 重新登录后收到离线消息补推")

# ---- 8) 双方再次互发消息 ---------------------------------------------------
send_text(a, uid_b, "A-to-B-again", mid); mid += 1
wait_text(b, "A-to-B-again")
check(True, "重登后 A -> B 消息送达")

send_text(b, uid_a, "B-to-A-again", mid); mid += 1
wait_text(a, "B-to-A-again")
check(True, "重登后 B -> A 消息送达")

# ---- 9) 回执离线消息并删除队列，然后两个用户都登出 -------------------------
# 回执放在最后，避免它的 DB 事务与前面紧接着的消息发送产生行锁竞争。
ack_delivered(b, offline)
time.sleep(0.5)
a.close()
b.close()
check(True, "两个用户登出（关闭 TCP 连接）")

if fail == 0:
    print("[verify] ALL PASS")
else:
    print("[verify] SOME FAILED (%d)" % fail)
    raise SystemExit(1)
PY
PY_RC=$?
set -e

if [[ $PY_RC -ne 0 ]]; then
    echo "[verify] 业务流程失败，服务器日志："
    cat "${TMPDIR:-/tmp}/verify_tcp_http_server.log" || true
    exit 1
fi

echo "[verify] HTTP + TCP 业务流程验收完成"
