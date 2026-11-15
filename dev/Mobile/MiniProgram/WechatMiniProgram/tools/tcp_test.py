# -*- coding: utf-8 -*-
"""
铖信 TCP 链路联调脚本（对接 KC-CXIM IMS，协议见 docs/IMS_readme.md）

测试内容：
1. HTTP 登录 A/B 获取 JWT（不存在则自动注册）
2. TCP 连接 + type=100 登录 -> 期望 101
3. B 向 A 发文本 -> A 收到 type=1，B 收到 SendAck=113，A 回 ACK=112
4. A/B 各自拉取历史消息 type=110 -> 111（content 二次 JSON 解析）
5. 心跳 type=3 保活验证

用法: python tools/tcp_test.py [host] [tcp_port] [http_port]
"""
import json
import socket
import struct
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "v0.fyping.cn"
TCP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 10001
HTTP_BASE = "http://%s:%s" % (HOST, sys.argv[3] if len(sys.argv) > 3 else "10002")

USERS = {"A": ("wx_test_a", "secret123"), "B": ("wx_test_b", "secret123")}

passed = []
failed = []


def check(name, ok, detail=""):
    (passed if ok else failed).append(name)
    print(("  [PASS] " if ok else "  [FAIL] ") + name + (("  | " + detail) if detail else ""))


def http_json(method, path, body=None, token=None):
    req = urllib.request.Request(HTTP_BASE + path, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    data = json.dumps(body).encode() if body is not None else None
    try:
        with urllib.request.urlopen(req, data, timeout=10) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode() or "{}")


def get_token(username, password):
    code, body = http_json("POST", "/api/login", {"username": username, "password": password})
    if body.get("code") != 0:
        http_json("POST", "/api/register", {"username": username, "password": password})
        code, body = http_json("POST", "/api/login", {"username": username, "password": password})
    assert body.get("code") == 0, "login failed: %r" % body
    return body["data"]


def send_frame(sock, obj):
    body = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    sock.sendall(struct.pack("!I", len(body)) + body)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed by server")
        buf += chunk
    return buf


def recv_frame(sock, timeout=6):
    sock.settimeout(timeout)
    length = struct.unpack("!I", recv_exact(sock, 4))[0]
    return json.loads(recv_exact(sock, length).decode("utf-8"))


def tcp_login(sock, token):
    send_frame(sock, {"from": 0, "to": 0, "type": 100, "msg_id": 0, "ts": 0,
                      "conversation_id": 0, "seq": 0, "content": token})
    deadline = time.time() + 6
    while time.time() < deadline:
        m = recv_frame(sock)
        if m["type"] == 101:
            return m
        if m["type"] == 102:
            raise RuntimeError("TCP login failed: %r" % m.get("content"))
    raise RuntimeError("TCP login timeout")


def recv_until(sock, want_types, timeout=6):
    """收帧直到出现 want_types 之一，返回该帧；其余帧打印出来。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = recv_frame(sock, timeout=max(0.1, deadline - time.time()))
        print("    <- recv type=%s msg_id=%s seq=%s content=%r" % (m.get("type"), m.get("msg_id"), m.get("seq"), str(m.get("content"))[:60]))
        if m["type"] in want_types:
            return m
    raise RuntimeError("timeout waiting for types %r" % (want_types,))


def main():
    print("target: %s  tcp=%d  http=%s" % (HOST, TCP_PORT, HTTP_BASE))

    # 1. HTTP 登录
    ua = get_token(*USERS["A"])
    ub = get_token(*USERS["B"])
    print("A userId=%s username=%s" % (ua["userId"], ua["username"]))
    print("B userId=%s username=%s" % (ub["userId"], ub["username"]))
    check("HTTP /api/login 获取 JWT", bool(ua.get("token")) and bool(ub.get("token")))

    # 好友流程（HTTP）
    code, body = http_json("POST", "/api/friends/requests",
                           {"toUserId": ub["userId"], "message": "hi from A"}, token=ua["token"])
    req_id = (body.get("data") or {}).get("request", {}).get("requestId")
    if code == 200 and body.get("code") == 0:
        code2, body2 = http_json("POST", "/api/friends/requests/%s/accept" % req_id, token=ub["token"])
        check("HTTP 好友申请 + 通过 (requestId=%s)" % req_id, body2.get("code") == 0)
    else:
        # 已是好友/重复申请等场景，验证好友列表接口可用即可
        code3, body3 = http_json("GET", "/api/friends", token=ua["token"])
        friends = [f["userId"] for f in (body3.get("data") or {}).get("friends", [])]
        check("HTTP 好友关系已存在/接口可用", body3.get("code") == 0 and ub["userId"] in friends,
              "friends(A)=%s" % friends)

    # 2. TCP 登录（A、B 双连接）
    sa = socket.create_connection((HOST, TCP_PORT), timeout=10)
    sb = socket.create_connection((HOST, TCP_PORT), timeout=10)
    ma = tcp_login(sa, ua["token"])
    mb = tcp_login(sb, ub["token"])
    print("A login ack: %r" % ma.get("content"))
    print("B login ack: %r" % mb.get("content"))
    check("TCP type=100 登录 -> 101", ma["type"] == 101 and mb["type"] == 101)

    # 3. B -> A 发文本
    send_frame(sb, {"from": 0, "to": ua["userId"], "type": 1, "msg_id": 10001,
                    "ts": 0, "conversation_id": 0, "seq": 0, "content": "hello from B"})
    ack_b = recv_until(sb, {113})
    check("发送方收到 SendAck=113 (content=%r)" % ack_b.get("content"), ack_b.get("content") == "sent")

    msg = recv_until(sa, {1})
    ok_push = msg.get("from") == ub["userId"] and msg.get("content") == "hello from B"
    check("接收方收到文本推送 type=1", ok_push, "from=%s conversation_id=%s seq=%s" % (msg.get("from"), msg.get("conversation_id"), msg.get("seq")))

    # A 回投递回执 112
    send_frame(sa, {"from": 0, "to": 0, "type": 112, "msg_id": 0, "ts": 0,
                    "conversation_id": msg["conversation_id"], "seq": msg["seq"], "content": "delivered"})
    check("接收方回 ACK=112", True)

    # A -> B 发文本
    send_frame(sa, {"from": 0, "to": ub["userId"], "type": 1, "msg_id": 10002,
                    "ts": 0, "conversation_id": 0, "seq": 0, "content": "hello from A"})
    ack_a = recv_until(sa, {113})
    check("A 发送收到 SendAck=113", ack_a.get("content") == "sent")
    push_b = recv_until(sb, {1})
    check("B 收到 A 的消息", push_b.get("content") == "hello from A")

    # 4. 历史消息
    send_frame(sa, {"from": 0, "to": ub["userId"], "type": 110, "msg_id": 0, "ts": 0,
                    "conversation_id": 0, "seq": 0, "content": "20"})
    hist_a = recv_until(sa, {111})
    try:
        arr_a = json.loads(hist_a["content"])
    except Exception as e:
        arr_a = []
        print("    history parse error:", e)
    check("A 拉取历史 type=111（二次解析）", len(arr_a) >= 2, "count=%d" % len(arr_a))

    send_frame(sb, {"from": 0, "to": ua["userId"], "type": 110, "msg_id": 0, "ts": 0,
                    "conversation_id": 0, "seq": 0, "content": "20"})
    hist_b = recv_until(sb, {111})
    try:
        arr_b = json.loads(hist_b["content"])
    except Exception:
        arr_b = []
    check("B 拉取历史 type=111", len(arr_b) >= 2, "count=%d" % len(arr_b))

    # 5. 心跳（服务端不回包，能正常发送且连接不断即可）
    send_frame(sa, {"from": 0, "to": 0, "type": 3, "msg_id": 0, "ts": 0,
                    "conversation_id": 0, "seq": 0, "content": ""})
    time.sleep(1)
    send_frame(sa, {"from": 0, "to": ub["userId"], "type": 110, "msg_id": 0, "ts": 0,
                    "conversation_id": 0, "seq": 0, "content": "5"})
    h = recv_until(sa, {111})
    check("心跳 type=3 后连接仍可用", True)

    sa.close()
    sb.close()

    print("\n===== 结果: %d 通过, %d 失败 =====" % (len(passed), len(failed)))
    if failed:
        print("失败项:", failed)
        sys.exit(1)


if __name__ == "__main__":
    main()
