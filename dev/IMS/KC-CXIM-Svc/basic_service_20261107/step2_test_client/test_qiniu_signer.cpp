// ============================================================================
// /*************图片文件功能（七牛云直传）：qiniu_signer 独立单测***************/
// ----------------------------------------------------------------------------
// 验收标准 1 要求"三类签名有独立单测"。这里用**两条腿**保证正确性：
//   1) RFC 2202 / RFC 4231 的 HMAC 官方向量（行业标准值，写死在源码里），
//      证明底层 HMAC-SHA1 / HMAC-SHA256 原语正确；
//   2) 七牛三类签名的"黄金样例"：由**独立的 Python 参考实现**
//      （hmac/hashlib/base64 标准库，与本 C++ 实现零共享代码）按官方
//      算法规范逐条算出后写死在这里——C++ 输出与之**逐字节**比对。
//      两条实现路径互相印证，任何一边算错都会在这里暴露。
//
// 运行方式（不需要服务器、不需要数据库、不需要网络）：
//   ./build/bin/Release/test_qiniu_signer        → 全部输出 PASS 并返回 0
// 任何一条 FAIL 都返回非 0，可直接接入 CI。
// ============================================================================
#include "qiniu_signer.h"

#include <iostream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

int g_fail = 0;

void expectEq(const std::string& got, const std::string& want, const char* what) {
    if (got == want) {
        std::cout << "  PASS: " << what << "\n";
    } else {
        std::cout << "  FAIL: " << what << "\n    got  = " << got
                  << "\n    want = " << want << "\n";
        ++g_fail;
    }
}

void expectTrue(bool cond, const char* what) {
    if (cond) std::cout << "  PASS: " << what << "\n";
    else { std::cout << "  FAIL: " << what << "\n"; ++g_fail; }
}

std::string toHex(const std::string& raw) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (unsigned char c : raw) { s.push_back(d[c >> 4]); s.push_back(d[c & 0xF]); }
    return s;
}

} // namespace

int main() {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    using namespace kc::qsign;
    std::cout << "[signer] ---- 1. HMAC 官方向量（RFC 2202 / RFC 4231）----\n";

    // key="Jefe", data="what do ya want for nothing?" 是两份 RFC 共同的 case 2。
    expectEq(toHex(hmacSha1("Jefe", "what do ya want for nothing?")),
             "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79",
             "HMAC-SHA1 RFC2202 case2");
    expectEq(toHex(hmacSha256("Jefe", "what do ya want for nothing?")),
             "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
             "HMAC-SHA256 RFC4231 case2");
    expectEq(sha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d",
             "SHA1(\"abc\") 经典测试串");

    std::cout << "[signer] ---- 2. Base64 / URL 安全 Base64 ----\n";
    expectEq(base64Encode("hello world"), "aGVsbG8gd29ybGQ=", "base64 hello world");
    expectEq(urlSafeBase64Encode(std::string("\xfb\xff\xff", 3)), "-___",
             "urlsafe base64 '+' '/' 替换");
    {
        std::string back;
        expectTrue(urlSafeBase64Decode("-___", back) && back == std::string("\xfb\xff\xff", 3),
                   "urlsafe base64 往返");
        expectTrue(urlSafeBase64Decode("not base64!!", back) == false,
                   "非法 base64 必须被拒（不能拿垃圾当解码结果）");
    }

    std::cout << "[signer] ---- 3. 上传凭证 uploadToken（Python 参考实现黄金样例）----\n";
    // 过期字段名用的是官方口径 **deadline**（不是 expires——后者会被七牛忽略，
    // 真实直传会回 401 expired token；见 object_store.cpp::signUploadToken 注释）。
    const std::string policy =
        "{\"scope\":\"bkt:im/2026-11/abc.png\",\"deadline\":1700000000,"
        "\"fsizeLimit\":1048576,\"insertOnly\":1}";
    // 期望值口径：sign = urlsafe_b64(hmac_sha1("TestSecretKey", encodedPolicy))，
    // 待签串**不含**任何 URL path。该值已用独立实现（Python 标准库 hmac）现算，
    // 并在 step3_verify/qiniu_token_probe.py 里用真实 AK/SK 走通到 up-z0 的直传
    // （2026-11-20 实测 up-z0.qiniup.com 回 200 {key,hash}）。
    expectEq(makeUploadToken("TestAccessKey", "TestSecretKey", policy),
             "TestAccessKey:oWLxopycX-uWf7QUfYG64GNZV44=:"
             "eyJzY29wZSI6ImJrdDppbS8yMDI2LTExL2FiYy5wbmciLCJkZWFkbGluZSI6MTcwMDAwMDAw"
             "MCwiZnNpemVMaW1pdCI6MTA0ODU3NiwiaW5zZXJ0T25seSI6MX0=",
             "makeUploadToken 与参考实现逐字节一致");

    std::cout << "[signer] ---- 4. 私有空间下载签名（含中文 key 的 URL 编码）----\n";
    // key="/dir/照.png" 已由调用方 urlEncode 成 "/dir/%E7%85%A7.png"；
    // 签名的待签串就是这个编码后的 path+"?e="+deadline（两边编码口径
    // 不一致 = 真实对接时 401 invalid sign 的头号原因，样例把它钉死）。
    expectEq(makeDownloadToken("DownloadAK", "DownloadSecret", "/dir/%E7%85%A7.png", 1700000000),
             "DownloadAK:Awrb7sd-yrXKc-TohfJky7dnLUg=",
             "makeDownloadToken 与参考实现逐字节一致");

    std::cout << "[signer] ---- 5. 管理 API 鉴权（官方 Qiniu 签名算法 v7）----\n";
    // 期望值口径（逐字节照抄 sdk/qiniu-c-sdk-master/qiniu/auth_mac.c 的
    // Qiniu_Mac_HmacV2 拼接序列）：
    //   signStr = "<METHOD> <Path>[?<Query>]\n"
    //             "Host: <Host>\n"
    //             ["Content-Type: <CT>\n"]["X-Qiniu-Date: <TS>\n"]
    //             "\n" [<Body>]
    //   sign = urlsafe_b64(hmac_sha1("MacSK", signStr))，头 = "Qiniu MacAK:" + sign。
    // 这组样例把三个易错点各钉一个：method/path 之间是**空格**、头是
    // "Key: value" 形式、头与 body 之间**必有一个空行**。以前钉的是
    // "METHOD\nHost\nPath" 那套（自洽但与官方不一致），单测全绿真机照样 401。
    // entry 的**官方顺序是 bucket:key**（官方 C SDK qiniu/rs.c：
    // Concat3(bucket, ":", key) 再 urlsafe_base64）。以前这里钉的是 key:bucket，
    // 单元测试同样绿——entry 整段都在签名里，拼反了两边依然自洽，七牛只会把它
    // 当成另一个"查无此对象"的 entry（612/631），"自洽的单测"挡不住这个 bug。
    const std::string entry = entryBase64("k1", "bucket1");
    const std::string ct   = "application/x-www-form-urlencoded";
    const std::string date = "20261121T000000Z";   // 写死，保证黄金值可复现
    expectEq(entry, "YnVja2V0MTprMQ==", "entry = urlsafe_b64(bucket:key) 官方顺序");
    // 与 object_store.cpp::stat 完全同参（GET + CT + X-Qiniu-Date、空 body）。
    expectEq(makeMacAuthorization("MacAK", "MacSK", "GET", "rs.qbox.me",
                                  "/stat/" + entry, "", "", ct, date),
             "Qiniu MacAK:ixsrLOosvwE2-Lveq1vd0S7gksQ=",
             "stat：GET 空 body，签 Host+CT+X-Qiniu-Date 三行后跟一个空行");
    expectEq(makeMacAuthorization("MacAK", "MacSK", "POST", "rs.qbox.me",
                                  "/delete/" + entry, "", "", ct, date),
             "Qiniu MacAK:eQ_K-nuY6c-RqmRouGwfZisV4L0=",
             "delete：与 object_store.cpp::remove 同参（POST 空 body 也带 CT/日期头）");
    expectEq(makeMacAuthorization("MacAK", "MacSK", "POST", "rs.qbox.me",
                                  "/batch", "", "op=/stat/" + entry, ct, date),
             "Qiniu MacAK:iMWW-5OORyp7ZBdmJBKC_Zxr8qA=",
             "body 非空 → 接在那个空行之后");
    expectEq(makeMacAuthorization("MacAK", "MacSK", "GET", "rs.qbox.me",
                                  "/stat/" + entry),
             "Qiniu MacAK:4s_WNAqlp0dADNO5ETFLfQPhUV8=",
             "无 CT/无日期头 → 只有 Host 行 + 空行（不发也不签）");
    expectEq(makeMacAuthorization("MacAK", "MacSK", "GET", "rs.qbox.me",
                                  "/list", "limit=2&marker=abc", "", ct, date),
             "Qiniu MacAK:I5WnkKNyE4yEKEAEarmJzCgpxnU=",
             "query 非空 → 用 '?' 接到 path 后（空 query 绝不能留 '?'）");
    {
        const std::string auth = makeMacAuthorization("MacAK", "MacSK", "GET",
                                                      "rs.qbox.me", "/stat/x", "");
        expectTrue(auth.rfind("Qiniu MacAK:", 0) == 0,
                   "Authorization 前缀是 \"Qiniu \"（不是 q-sign v2 那套 QINIU ...）");
    }

    std::cout << "[signer] ---- 6. urlEncode 白名单 ----\n";
    expectEq(urlEncode("a b/c.png", true), "a%20b/c.png", "keepSlash=true 保留 '/'");
    expectEq(urlEncode("a b/c.png", false), "a%20b%2Fc.png", "keepSlash=false 转义 '/'");

    if (g_fail == 0) {
        std::cout << "[signer] ALL PASS\n";
        return 0;
    }
    std::cout << "[signer] " << g_fail << " FAILED\n";
    return 1;
}
