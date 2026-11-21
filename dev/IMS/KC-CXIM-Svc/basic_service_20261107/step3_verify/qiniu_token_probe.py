#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# /*************图片文件功能（七牛云真实直传）：uptoken + 直传端到端探针***************/
# ----------------------------------------------------------------------------
# 目的：**绕开 IM 服务器**，用同一份 QiniuConfig.json 手工签 uptoken 并直接向
# up_host POST 一小段字节——把"凭证/AK/SK/bucket/区域是否正确"和"C++ 签名代码
# 是否正确"这两件事解耦。运行结果只有三种，每一种都指向唯一根因：
#
#   ✅ 200 {key,hash}
#      → AK/SK/bucket/up_host/区域/算法 全部正确。
#      本脚本产生的 uptoken 可与 IM 服务器 /api/files/upload-token 返回的 uptoken
#      逐字节 diff（相同 policy 输入下必须完全一致）——若本脚本能传而 IM 服务器
#      签的不能，问题在服务端签发路径（policy 序列化：字段顺序/空格/数字格式），
#      非账号问题。
#
#   ❌ 401 "bad token" / error_code=BadToken
#      → 七牛拿 SK 重算 sign 对不上。**头号原因是签名口径写错（代码问题），
#        第二才是凭证本身**（AK 不存在 / SK 与 AK 不配对 / 凭证首尾粘进空格换行 /
#        AK 对该 bucket 无上传权限）。2026-11-20 本探针曾被解读成"AK/SK 与
#        bucket 区域配错、非代码问题"，其实真因是 sign 多拼了 "\n"+b64("/")
#        （那是管理凭证的规则）——所以这条**别再先怀疑账号**，先按 ①②③ 顺序查。
#      本脚本把 sign 与 encodedPolicy 都打印出来，可与服务端签发的 uptoken
#      逐字节 diff，也可与控制台"密钥管理"里 AK 前/后 4 位、区域、上传域名对照。
#
#   ❌ 401 "expired token"
#      → **先查 policy 里有效期字段名**：必须是 deadline（绝对 Unix 秒）；
#        写成 expires 会被七牛整字段忽略→按 0 处理→永远 expired，而签名是好的。
#        （本探针以前一上来就断言"时钟慢了 3600s"，2026-11-20 实测与时钟无关。）
#        现在它会拿响应 Date 头与本机时间、deadline 三者对比后再下结论；
#        只有它真报"本机比七牛慢"时才 timedatectl set-ntp true 校时。
#
# 用法：
#   python3 step3_verify/qiniu_token_probe.py                # 默认 im/probe/uuid.png
#   python3 step3_verify/qiniu_token_probe.py --key <key>    # 指定 key
#   python3 step3_verify/qiniu_token_probe.py --dry          # 只签、不真上传
#   QINIU_CONFIG=/path/to/QiniuConfig.json 可覆盖配置文件路径
#
# 只用标准库（hmac/hashlib/base64/json/urllib），不 pip install qiniu。
# 算法口径 = 七牛官方「上传凭证」文档：sign = urlsafe_b64(hmac_sha1(SK, encodedPolicy))。
# test_qiniu_signer.cpp 里的 golden sample 就是按这个口径**离线独立**算出来的
# （注意：以前那条"与 C++/Python 两条实现互证"的说法没有证明力——两边共用同一
#  份错误算法时，互证一样全绿，这正是这次 401 没被单测拦住的原因。真正有证明力
#  的只有：官方文档口径 + 本脚本对真实 up_host 的一次成功直传。）
# ============================================================================
import argparse
import base64
import hashlib
import hmac
import json
import os
import ssl
import sys
import time
import urllib.request
import urllib.error
import uuid as uuidlib
from email.utils import parsedate_to_datetime

# Windows 控制台默认 GBK，脚本里有中文与符号会 UnicodeEncodeError——强制 UTF-8。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


# ---------------------------------------------------------------------------
# Qiniu 签名原语（与 qiniu_signer.h 完全一致）
# ---------------------------------------------------------------------------
def urlsafe_b64(data: bytes) -> str:
    # 官方口径：'+'->'-'、'/'->'_'、保留 '=' 填充（对照 test_qiniu_signer.cpp 的黄金样例）。
    return base64.urlsafe_b64encode(data).decode("ascii")


def make_upload_token(ak: str, sk: str, policy_json: str) -> str:
    """七牛官方 upload-token 口径：
         encodedPolicy = urlsafe_b64(policy_json)
         sign          = urlsafe_b64(hmac_sha1(SK, encodedPolicy))
         uptoken       = AK + ":" + sign + ":" + encodedPolicy
    ⚠ 待签串**只有** encodedPolicy。旧版这里多拼了 "\\n"+urlsafe_b64("/")
      （那是「管理凭证」的规则），导致探针与服务端签出的每个 token 都
      401 bad token —— 当时还被误判成 AK/SK/区域配错。别改回去。
    """
    encoded_policy = urlsafe_b64(policy_json.encode("utf-8"))
    sign = urlsafe_b64(hmac.new(sk.encode("utf-8"),
                                encoded_policy.encode("utf-8"),
                                hashlib.sha1).digest())
    return f"{ak}:{sign}:{encoded_policy}"


# ---------------------------------------------------------------------------
# 与 object_store.cpp 的 QiniuObjectStore::signUploadToken **逐字节同构**的
# policy 生成（rapidjson Writer 输出是紧凑无空格的，Python json.dumps 用
# separators=(',',':') 与之等价；字段顺序与 Writer 的 Key 顺序保持一致，纯粹
# 是为了能把本脚本的 encodedPolicy 与服务端签出的逐字节 diff——七牛自己**不看
# 顺序**（实测 scope 放最后一个照样 200），它只看 sign 是否与它重算的一致）。
#
# ⚠ 有效期字段名 = **deadline**（绝对 Unix 秒）。这里曾经是 "expires"，而
#   七牛服务端**不认** expires（那是官方 SDK 的入参名，SDK 会换算成 deadline），
#   于是 policy 被当成"没有有效期"→ 一律 401 {"error":"expired token"}，
#   签名却完全正确——这就是"AK/SK 明明是对的却怎么都传不上去"的第二只靴子。
# ---------------------------------------------------------------------------
def build_policy(bucket: str, key: str, fsize_limit: int, deadline: int) -> str:
    obj = {
        "scope":      f"{bucket}:{key}",
        "deadline":   int(deadline),
        "fsizeLimit": int(fsize_limit),
        "insertOnly": 1,
    }
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


# 注意：**没有** "upload_url_path_of()/encodedUrl 参与签名" 这种东西了。
# 七牛 uptoken 的待签串只有 encodedPolicy（见 make_upload_token 文档串），
# 上传 URL 的 path 不进签名；七牛各区域上传域名本身也不带路径段。
# 旧实现把「管理凭证」的 policy+"\n"+b64(path) 抄到这里，是本次 BadToken 的真因。


# ---------------------------------------------------------------------------
# 手工 multipart/form-data 表单直传（对应 examples/up_form_upload.c 的 curl 三段）。
# ---------------------------------------------------------------------------
def form_upload(up_host: str, uptoken: str, key: str, filename: str,
                mime: str, data: bytes, timeout: int = 30) -> tuple:
    boundary = "----KCcximProbeBoundary0123456789"
    parts = []
    parts.append(f"--{boundary}\r\n"
                 f'Content-Disposition: form-data; name="token"\r\n\r\n'
                 f"{uptoken}\r\n")
    parts.append(f"--{boundary}\r\n"
                 f'Content-Disposition: form-data; name="key"\r\n\r\n'
                 f"{key}\r\n")
    parts.append(f"--{boundary}\r\n"
                 f'Content-Disposition: form-data; name="file"; filename="{filename}"\r\n'
                 f"Content-Type: {mime}\r\n\r\n")
    body  = "".join(parts).encode("utf-8")
    body += data
    body += f"\r\n--{boundary}--\r\n".encode("utf-8")

    req = urllib.request.Request(
        up_host if up_host.endswith("/") else up_host + "/",
        data=body,
        method="POST",
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
            "User-Agent": "kc-cxim-qiniu-probe/1.0",
            "Connection": "close",
        },
    )
    # 与 object_store.cpp 的 QiniuHttpAgent 一致：默认不校验证书（Windows/Linux
    # 下 OpenSSL 系统证书库读取差异太大）；如需严格校验设 QINIU_CA_BUNDLE=<path>。
    ctx = ssl.create_default_context()
    ca = os.environ.get("QINIU_CA_BUNDLE")
    if os.environ.get("QINIU_VERIFY_CERTS") == "1" and ca:
        ctx.load_verify_locations(cafile=ca)
    else:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


# ---------------------------------------------------------------------------
# HTTP Date（RFC1123, GMT）→ Unix 秒。七牛响应的 Date 头就是**权威时间**：
# "expired token" 到底是真过期，还是有效期字段压根没被认出来，靠它一分为二。
# ---------------------------------------------------------------------------
def parse_http_date(value: str):
    if not value:
        return None
    try:
        dt = parsedate_to_datetime(value)
        return int(dt.timestamp()) if dt.tzinfo else None
    except Exception:
        return None


# ---------------------------------------------------------------------------
def load_config(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    need = ["access_key", "secret_key", "bucket", "up_host", "domain"]
    missing = [k for k in need if not cfg.get(k)]
    if missing:
        raise SystemExit(f"[probe] QiniuConfig.json 缺字段：{missing}")
    return cfg


def mask_ak(ak: str) -> str:
    if len(ak) <= 8:
        return ak + "(len=" + str(len(ak)) + ")"
    return f"{ak[:4]}...{ak[-4:]}(len={len(ak)})"


# 七牛 AK/SK 形态常识：
#   两者都是 **40 位**，字符表 = URL-safe Base64（A-Z a-z 0-9 '-' '_'）。
#   ⚠ 因此 **AK 里出现 '_' 或 '-' 完全正常**，绝不能据此判"AK/SK 抄反了"——
#     本函数早期版本就是这么断言的，把一个合法 AK 报成填反，纯属误导
#     （而且 AK/SK 同字符表，形态上根本无法区分谁是谁，不该做这种猜测）。
#   真正值得机器检查的：长度、字符表、首尾空白（粘贴漏字符/带换行 = sign 恒定错）。
_B64URL_ALPHABET = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")


def credential_shape_check(ak: str, sk: str) -> list:
    warns = []
    for name, val in (("access_key", ak), ("secret_key", sk)):
        if len(val) != 40:
            warns.append(f"{name} 长度={len(val)}，七牛固定 40 位"
                         "（多半是粘贴时漏/多字符，或被行尾注释截断）")
        bad = sorted({c for c in val if c not in _B64URL_ALPHABET})
        if bad:
            warns.append(f"{name} 含非 URL-safe-Base64 字符 {bad!r}，请从『密钥管理』整段重贴")
        if val != val.strip():
            warns.append(f"{name} 首尾含空白字符（空格/换行/\\r）——这是粘贴事故的头号形态，"
                         "它会让每个 sign 都验不过（表现为 bad token）")
    return warns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", default=None,
                    help="object key；默认 im/probe/<uuid>.png（每次不同，insertOnly=1 才不会撞）")
    ap.add_argument("--size", type=int, default=1024, help="上传字节数，默认 1024")
    ap.add_argument("--ttl",  type=int, default=3600,
                    help="有效期相对秒，换算成 policy.deadline 绝对时间戳；默认 3600（与服务端 kTokenTtl 一致）")
    ap.add_argument("--dry",  action="store_true",    help="只签 uptoken，不真上传")
    args = ap.parse_args()

    cfg_path = os.environ.get("QINIU_CONFIG", "QiniuConfig.json")
    if not os.path.exists(cfg_path):
        raise SystemExit(f"[probe] 未找到 QiniuConfig.json：{cfg_path}（可用 QINIU_CONFIG 环境变量覆盖）")
    cfg = load_config(cfg_path)

    ak = cfg["access_key"]
    sk = cfg["secret_key"]
    bucket = cfg["bucket"]
    up_host = cfg["up_host"].rstrip("/")

    # 形态自检只查长度/字符表/首尾空白（粘贴事故）。它**查不出**"AK 与 SK
    # 不配对"——那只能靠真发一次请求；也不代表 BadToken 的头号原因（头号是
    # 签名口径错，即本脚本自己的算法，见 make_upload_token）。
    for w in credential_shape_check(ak, sk):
        print(f"[probe] ⚠ 凭证形态告警：{w}")

    key = args.key or f"im/probe/{uuidlib.uuid4().hex}.png"
    deadline_abs = int(time.time()) + args.ttl      # 绝对 Unix 秒 → policy.deadline
    policy = build_policy(bucket, key, args.size, deadline_abs)

    print(f"[probe] config={cfg_path}")
    print(f"[probe] AK={mask_ak(ak)}  bucket={bucket}  zone={cfg.get('zone','?')}  "
          f"up_host={up_host}")
    print(f"[probe] key={key}")
    print(f"[probe] policy={policy}")
    # 只走 make_upload_token 这**唯一一条**签名路径。以前 main 里把签名逻辑
    # 又内联抄了一遍，于是改算法时两处不同步——这正是本次事故的放大器。
    uptoken = make_upload_token(ak, sk, policy)
    _, sign, encoded_policy = uptoken.split(":", 2)
    print(f"[probe] encodedPolicy={encoded_policy[:40]}...(len={len(encoded_policy)})")
    print(f"[probe] sign         ={sign}")
    print(f"[probe] uptoken      = {ak[:4]}...:{sign}:{encoded_policy[:20]}...")

    if args.dry:
        print("[probe] --dry 已跳过实际上传")
        return

    data = (b"\x00\x11\x22\x33\x44\x55\x66\x77" * 4096)[:args.size]
    print(f"[probe] POST {cfg['up_host']}  size={len(data)} ...")
    status, headers, body = form_upload(cfg["up_host"], uptoken, key,
                                        os.path.basename(key), "image/png", data)
    reqid = headers.get("X-Reqid") or headers.get("x-reqid") or ""
    date  = headers.get("Date") or headers.get("date") or ""
    print(f"[probe] HTTP {status}  X-Reqid={reqid}  Date={date}")
    try:
        j = json.loads(body.decode("utf-8", "replace"))
        print(f"[probe] resp={json.dumps(j, ensure_ascii=False)}")
    except Exception:
        print(f"[probe] resp(body 非 JSON)={body[:200]!r}")

    if status == 200:
        print("[probe] ✅ AK/SK/bucket/up_host/区域/算法 全部 OK —— 若 IM 服务器仍 BadToken，"
              "问题在服务端签发路径（policy 序列化差异：字段顺序/空格/数字格式），非账号问题；"
              "把本脚本打印的 uptoken 与 IM 客户端 [诊断] 里 policy/AK 拆解逐字段对照即可定位。")
    elif status == 401:
        msg = ""
        try: msg = json.loads(body.decode("utf-8", "replace")).get("error", "")
        except Exception: pass
        if "expired" in msg:
            # 旧文案在这里直接断言"系统时钟慢 >3600s，去校时"——2026-11-20 那次
            # 实测本机与七牛 Date **零偏差**、deadline 还有整 3600s，照样报
            # expired，白校一轮时钟。真因是 policy 字段名（expires 不被识别）。
            srv = parse_http_date(date)
            print("[probe] ❌ expired token：先用七牛的 Date 头把两种情况分开，别急着校时钟：")
            if srv is None:
                print("        （响应没带可解析的 Date 头，无法核对权威时间，跳过判定）")
            else:
                local = int(time.time())
                left = deadline_abs - srv
                print(f"        七牛权威时间={srv}  本机={local}  时钟差={local - srv:+d}s")
                print(f"        policy.deadline={deadline_abs}  按七牛时间还剩 {left}s")
                if left > 0:
                    print("        → deadline **明明还没到**却报 expired ⇒ 七牛没能从 policy 里")
                    print("          读出有效期。头号原因：有效期字段写成了 **expires**")
                    print("          （那是官方 SDK 的入参名，policy JSON 里必须是 **deadline**，")
                    print("          写成 expires 会被整字段忽略→按 0→永远 expired）。")
                    print("          实测钉死：同一 AK/SK、同一 up-z0：expires→401，deadline→200。")
                    print("          本脚本已用 deadline；若你看到的是服务端签的 token，")
                    print("          请解 encodedPolicy 确认里面有 deadline。")
                elif local - srv > 60:
                    print("        → 本机确实比七牛慢且超过了 TTL：timedatectl set-ntp true 后重启服务。")
                else:
                    print(f"        → 时钟基本同步但凭证确实已到期：TTL={args.ttl}s 太短，"
                          "或请求在队列/重试里躺太久。")
        elif "bad token" in msg or "invalid token" in msg:
            print("[probe] ❌ bad token：七牛用 SK 重算 sign 后对不上，**只可能**是下面 3 类，按序排查：")
            print("        ① 签名口径/代码错（**最常见**，本项目 2026-11-20 就是这条）：")
            print("           uptoken 的 sign 必须 = urlsafe_b64(hmac_sha1(SK, encodedPolicy))，")
            print("           待签串里**不能**混进 URL path、也不能混进原始 policy JSON；")
            print("           同时 encodedPolicy 必须由**逐字节一致**的 policy 串编出")
            print("           （字段顺序/空格/数字格式变一个字符，sign 就全变）。")
            print("           排查办法：--dry 打印 encodedPolicy+sign，与服务端")
            print("           /api/files/upload-token 返回的 uptoken 逐字节 diff。")
            print("        ② 凭证本身：AK 长度须 40；SK 与该 AK 成对（去『密钥管理』整段重贴，")
            print("           别手抄）；bucket 属于该 AK 所在账号；子账号 AK 需有上传权限策略。")
            print("        ③ bucket 实际区域 ≠ up_host（这种通常回 no such bucket/6121，")
            print("           但区域与凭证跨账号混用时也可能先报 bad token）。")
            print("        注：AK/SK 都是 urlsafe base64 字符表（**可以**含 '-' 和 '_'），")
            print("        所以 AK 里有下划线不代表你填反了。")
        else:
            print(f"[probe] ❌ 401 其它：{msg}")
    else:
        print(f"[probe] ❌ 非预期 {status}（用 X-Reqid 找七牛工单最快）。")


if __name__ == "__main__":
    main()
