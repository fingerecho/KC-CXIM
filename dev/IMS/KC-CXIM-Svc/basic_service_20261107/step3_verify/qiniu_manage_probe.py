#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# ============================================================================
# /*************图片文件功能（七牛云管理 API）：stat/delete 鉴权探针***************/
# ----------------------------------------------------------------------------
# 用途：直传已经 200、但 commit 回 `{"code":1,"message":"qiniu stat http=401"}`
# 时，用**一把只读请求**把根因钉死到下面三者之一：
#   ① 签名口径（"Qiniu AK:sign" 才对，q-sign v2 那套 rs.* 不认）；
#   ② rs 端点选错（rs.qbox.me 全局 / rs-z0.qbox.me 区域 / rs.qiniuapi.com 新域名）；
#   ③ 凭证本身（子账号 AK 无该 bucket 管理权限）。
# 做法：对同一 entry 试 多个 host × 多种前缀 的组合，把 status/body 打成矩阵。
#
# 默认**只发 GET /stat**（读，不改任何东西）。要打 delete 必须显式
# --action delete --yes-i-mean-delete，且只应指向你自己造的探针 key。
#
# 用法：
#   python3 step3_verify/qiniu_manage_probe.py --key im/probe/xxxx.png
#   python3 step3_verify/qiniu_manage_probe.py --dry          # 只算签名头，不发请求
#   python3 step3_verify/qiniu_manage_probe.py --key ... --action delete --yes-i-mean-delete
#   QINIU_CONFIG=/path/QiniuConfig.json 覆盖配置路径
#
# 口径与 step1_tcp_echo/qiniu_signer.cpp::makeMacAuthorization 逐字节一致：
#   signStr = METHOD "\n" Host "\n" Path [ "?" Query ] [ "\n" Body | Content-Type ]
#   Authorization: Qiniu <AK>:urlsafe_b64(hmac_sha1(SK, signStr))
# 只用标准库。
# ============================================================================
import argparse
import base64
import hashlib
import hmac
import json
import os
import sys
import time
import urllib.error
import urllib.request
# 注意：**不在模块顶层 import ssl**。本机可能装的是没有 _ssl 的精简 Python
# （Qt/MinGW 自带那套就是这样），那样连 --dry 都跑不起来；ssl 只在真要发请求时
# 在 request() 里就地 import。

try:  # Windows 控制台默认 GBK，中文/符号会 UnicodeEncodeError
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def urlsafe_b64(data):
    if isinstance(data, str):
        data = data.encode("utf-8")
    return base64.urlsafe_b64encode(data).decode("ascii")


def entry_b64(key, bucket):
    """七牛 entry 的官方顺序是 <key>:<bucket>（反了就是 6121/401）。"""
    return urlsafe_b64(key + ":" + bucket)


def make_authorization(sk, ak, method, host, path, query="", body="", content_type=""):
    sign_str = method + "\n" + host + "\n" + path
    if query:
        sign_str += "?" + query
    if body:
        sign_str += "\n" + body
    elif content_type:
        sign_str += "\n" + content_type
    sign = urlsafe_b64(hmac.new(sk.encode("utf-8"), sign_str.encode("utf-8"),
                                hashlib.sha1).digest())
    return sign_str, "Qiniu " + ak + ":" + sign


def make_authorization_qbox(sk, ak, method, host, path):
    """老写法：QBox 前缀 + 待签串不含 METHOD 行（与 Qiniu 方案不同）。"""
    sign = urlsafe_b64(hmac.new(sk.encode("utf-8"), (path + "\n" + host).encode("utf-8"),
                                hashlib.sha1).digest())
    return path + "\n" + host, "QBox " + ak + ":" + sign


def make_authorization_qsign_v2(sk, ak, method, host, path, when):
    """仓库**改之前**的错误实现，保留在这里只为作对照（预期 401）。"""
    sign_time = "%d;%d" % (when - 300, when + 3600)
    signing_key = hashlib.sha1(sk.encode("utf-8")).hexdigest()
    signing_str = "&".join([method, host, path, ""])
    signature = hmac.new(signing_key.encode("utf-8"), signing_str.encode("utf-8"),
                         hashlib.sha1).hexdigest()
    header = ("QINIU q-sign-algorithm=sha1&q-ak=%s&q-sign-time=%s&q-key-time=%s"
              "&q-header-list=host&q-url-param-list=&q-signature=%s"
              % (ak, sign_time, sign_time, signature))
    return signing_str, header


def request(url, headers, method, timeout, insecure):
    import ssl  # 见文件头：延迟到这里，--dry 才不需要 _ssl
    ctx = ssl.create_default_context()
    if insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(url, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=ctx) as resp:
            return resp.status, resp.read().decode("utf-8", "replace"), ""
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace"), (e.headers.get("X-Reqid") or "")
    except Exception as e:
        return -1, "", "%s: %s" % (type(e).__name__, e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--key", required=True,
                    help="要 stat/delete 的 object key（必填：stat 一个没真实上传过的 key "
                         "只会拿到 6121 no such file，分不清是鉴权还是对象不存在，白跑一轮）")
    ap.add_argument("--action", choices=["stat", "delete"], default="stat",
                    help="stat=只读检查（默认）；delete=真删，需 --yes-i-mean-delete")
    ap.add_argument("--yes-i-mean-delete", action="store_true",
                    help="显式确认才允许 delete")
    ap.add_argument("--hosts", default="rs.qbox.me,rs-z0.qbox.me,rs.qiniuapi.com",
                    help="逗号分隔的 rs 端点候选")
    ap.add_argument("--scheme", default="https", help="https（默认）或 http")
    ap.add_argument("--dry", action="store_true", help="只打印签名，不发请求（本机无 _ssl 也能跑）")
    ap.add_argument("--insecure", action="store_true", default=True,
                    help="不校验服务端证书（与 object_store.cpp 的 ca_bundle 留空行为一致）")
    ap.add_argument("--verify-certs", dest="insecure", action="store_false",
                    help="严格校验证书（配了 ca_bundle 时用）")
    args = ap.parse_args()

    cfg_path = os.environ.get("QINIU_CONFIG", "QiniuConfig.json")
    if not os.path.exists(cfg_path):
        raise SystemExit("[mgmt] 未找到配置：%s（可用 QINIU_CONFIG 覆盖）" % cfg_path)
    cfg = json.load(open(cfg_path, encoding="utf-8"))
    ak, sk, bucket = cfg["access_key"], cfg["secret_key"], cfg["bucket"]

    if args.action == "delete" and not args.yes_i_mean_delete:
        raise SystemExit("[mgmt] delete 会真删对象：加 --yes-i-mean-delete 再来")

    key = args.key
    entry = entry_b64(key, bucket)
    path = ("/stat/" if args.action == "stat" else "/delete/") + entry
    method = "GET" if args.action == "stat" else "POST"

    print("[mgmt] config=%s" % cfg_path)
    print("[mgmt] action=%s method=%s bucket=%s key=%s" % (args.action, method, bucket, key))
    print("[mgmt] entry=%s" % entry)
    print("[mgmt] path=%s" % path)
    print("[mgmt] AK=%s...%s(len=%d)" % (ak[:4], ak[-4:], len(ak)))
    print("[mgmt] 注意：signStr 里的 Host 必须等于实际发出的 Host 头，所以每个候选端点单独签。")

    rows = []
    for host in [h.strip() for h in args.hosts.split(",") if h.strip()]:
        variants = []
        s, h1 = make_authorization(sk, ak, method, host, path)
        variants.append(("Qiniu", s, h1))
        s2, h2 = make_authorization_qbox(sk, ak, method, host, path)
        variants.append(("QBox", s2, h2))
        s3, h3 = make_authorization_qsign_v2(sk, ak, method, host, path, int(time.time()))
        variants.append(("q-sign-v2(旧实现,预期失败)", s3, h3))

        for name, sign_str, header in variants:
            url = "%s://%s%s" % (args.scheme, host, path)
            if args.dry:
                print("[dry] %-28s host=%-18s signStr=%r" % (name, host, sign_str))
                print("      Authorization: %s" % (header[:64] + "..."))
                continue
            status, body, reqid = request(url, {"Authorization": header,
                                                "User-Agent": "kc-cxim-mgmt-probe/1.0"},
                                          method, 25, args.insecure)
            print("[%s] host=%-18s http=%s X-Reqid=%s resp=%s"
                  % (name, host, status, reqid, body[:140].replace("\n", " ")))
            rows.append((name, host, status, body))

    if args.dry:
        print("[dry] 未发请求。要联网判定请去掉 --dry。")
        return

    ok = [r for r in rows if r[2] == 200]
    print("-" * 72)
    if ok:
        name, host, _, body = ok[0]
        print("[结论] ✅ 可用组合：口径=%s 端点=%s  响应=%s" % (name, host, body[:120]))
        if name == "Qiniu":
            print("       Qiniu AK:sign + 该端点即正确口径。")
            if host != cfg.get("rs_host", "rs.qbox.me"):
                print("       ⚠ 但你的 QiniuConfig.json 里 rs_host=%r ——把它改成 %s 后重启服务即可。"
                      % (cfg.get("rs_host"), host))
            else:
                print("       rs_host 与配置一致：若服务端仍 401，多半是它把 signStr 拼错"
                      "（对照 makeMacAuthorization 的每一行分隔符），或 AK 无管理权限。")
    else:
        codes = sorted({r[2] for r in rows})
        print("[结论] ❌ 没有任何组合成功（出现的状态码：%s）" % codes)
        if any(r[2] == 401 for r in rows):
            print("       三种口径全 401 ⇒ 基本排除'签名拼法'问题，指向凭证权限："
                  "AK 是子账号且未授 object:Stat/Delete 权限，或 bucket 不属该账号。")
            print("       另外确认 key 真存在（直传成功过没？stat 一个 200 直传留下的 key）。")
        if any(r[2] == -1 for r in rows):
            print("       还有连接失败项：本机到 rs.* 的 443 是否可达、证书校验是否被中间盒干扰。")


if __name__ == "__main__":
    main()
