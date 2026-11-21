#include "qiniu_signer.h"

#include <openssl/evp.h>       // EVP：Base64 编解码 + HMAC/SHA 的当前推荐入口
#include <openssl/core_names.h>// OSSL_PARAM 的 "digest" 参数名常量（3.x 的 EVP_MAC）
#include <openssl/params.h>    // OSSL_PARAM_construct_utf8_string（3.x）
#include <openssl/hmac.h>      // HMAC() 一行式接口（1.1 路径）
#include <openssl/sha.h>       // SHA1

#include <algorithm>           // std::sort：管理 API 的 query 参数要按 key 字典序排
#include <cctype>
#include <cstring>             // std::strcmp：1.1 兼容路径按摘要名选 EVP_MD
#include <cstdio>              // std::snprintf：十六进制编码
#include <vector>

// ============================================================================
// /*************图片文件功能（七牛云直传）：签名算法实现***************/
// ----------------------------------------------------------------------------
// 实现原则：
//   * 只用 OpenSSL（vcpkg 已装、CMake 已链接 OpenSSL::Crypto），不引 SDK；
//   * 每一步都注释对应官方规范的哪一条，方便协议变更时逐条核对；
//   * 全部纯函数、无状态、无锁，因此可以在 io 线程直接调用（微秒级 CPU 活，
//     与 JWT HS256 同性质——见 http_server.cpp 里对 io 线程纪律的说明）。
// ============================================================================

namespace kc {
namespace qsign {

namespace {

// 原始字节 -> 小写十六进制串。管理 API 的 signing_key / signature 都用 hex。
std::string toHex(const unsigned char* data, unsigned int len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out[i * 2]     = digits[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
    return out;
}

// HMAC(key, data)：1.1 与 3.x 双兼容。
//   * OpenSSL 3.0：HMAC() 单函数被弃用，走 EVP_MAC_fetch("HMAC") + digest 参数；
//   * OpenSSL 1.1：HMAC() 一行式接口仍是推荐路径。
// 摘要算法用**名字**传递（"SHA1"/"SHA256"），3.x 里它是 OSSL_PARAM 的取值、
// 1.1 里映射回 EVP_sha1()/EVP_sha256()——避免调用 EVP_MD_get0_name() 这类
// 只在某一个大版本里存在的函数（版本兼容代码里最容易踩的坑）。
std::string hmac(const char* mdName,
                 const std::string& key,
                 const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];      // SHA1=20、SHA256=32 字节，按最大给

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    size_t outLen = 0;                       // 注意：3.x 的 outl 是 size_t
    EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (!mac) return {};
    EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
    if (!ctx) { EVP_MAC_free(mac); return {}; }
    // OSSL_PARAM_construct_utf8_string 要非 const char*：拷一份到栈缓冲。
    char digestName[8];
    std::snprintf(digestName, sizeof(digestName), "%s", mdName);
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_utf8_string("digest", digestName, 0);
    params[1] = OSSL_PARAM_construct_end();
    std::string result;
    const bool ok =
        EVP_MAC_init(ctx, reinterpret_cast<const unsigned char*>(key.data()),
                     key.size(), params) &&
        EVP_MAC_update(ctx, reinterpret_cast<const unsigned char*>(data.data()),
                       data.size()) &&
        EVP_MAC_final(ctx, out, &outLen, sizeof(out));
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);
    if (!ok) return {};
    return std::string(reinterpret_cast<char*>(out), outLen);
#else
    const EVP_MD* md = (std::strcmp(mdName, "SHA1") == 0) ? EVP_sha1() : EVP_sha256();
    unsigned int outLen = 0;                 // 1.x 的出参是 unsigned int
    HMAC(md,
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         out, &outLen);
    return std::string(reinterpret_cast<char*>(out), outLen);
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Base64（标准）
// ---------------------------------------------------------------------------
std::string base64Encode(const std::string& data) {
    if (data.empty()) return {};
    // EVP_EncodeBlock 输出长度 = 4 * ceil(n/3)，含 '=' 填充，无换行。
    std::vector<char> buf((data.size() / 3 + 1) * 4 + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(buf.data()),
                                  reinterpret_cast<const unsigned char*>(data.data()),
                                  static_cast<int>(data.size()));
    if (n <= 0) return {};
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

bool base64Decode(const std::string& text, std::string& out) {
    if (text.empty()) { out.clear(); return true; }
    // 长度分组检查必须在**解码之前**：EVP_DecodeBlock 对非 4 倍数输入会按
    // 满组写出 3*ceil(L/4) 字节——先写后查等于给越界写开了门（历史教训：
    // 本轮 mockQetag 越界把堆踩烂后，这里差点成为第二个随机崩溃源）。
    // 七牛体系内所有 base64（token/policy/entry）都是标准 4 字节分组，
    // 非 4 倍数一律视为非法输入。
    if (text.size() % 4 != 0) return false;
    // 预校验字符合法性：EVP_DecodeBlock 会"尽力解码"，先挡掉明显非法输入。
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) continue;
        if (c == '+' || c == '/' || c == '=' || c == '-' || c == '_') continue;
        return false;
    }
    // 缓冲按 EVP_DecodeBlock 的最坏写入量分配（3 字节/4 字符组，向上取整）。
    std::vector<char> buf(text.size(), '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(buf.data()),
                                  reinterpret_cast<const unsigned char*>(text.data()),
                                  static_cast<int>(text.size()));
    if (n < 0) return false;
    // EVP_DecodeBlock 不裁填充：按 '=' 个数把多出的字节削掉（0~2 个）。
    std::size_t len = static_cast<std::size_t>(n);
    const std::size_t pads = static_cast<std::size_t>(
        std::count(text.begin(), text.end(), '='));
    if (pads > 2 || pads > len) return false;
    len -= pads;
    out.assign(buf.data(), len);
    return true;
}

// 七牛 URL 安全 Base64：字符表 '-'/'_' 替换 '+'/'/'，**保留 '=' 填充**
// （对照官方 Go SDK encode 实现：base64.RawURLEncoding? 不——官方是
//  urlsafe = strings.NewReplacer("+/","-_") 后再原样保留 padding）。
std::string urlSafeBase64Encode(const std::string& data) {
    std::string s = base64Encode(data);
    for (auto& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return s;
}

bool urlSafeBase64Decode(const std::string& text, std::string& out) {
    std::string s = text;
    for (auto& c : s) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    return base64Decode(s, out);
}

// ---------------------------------------------------------------------------
// RFC3986 百分号编码。未保留字符：A-Z a-z 0-9 - _ . ~
// keepSlash=false：object key 放进 **URL path 段** 时必须整体转义（七牛私有
// 下载签名文档对 '/' 的口径是 **不转义**，因为 key 在 URL 中按原样参与签名，
// 我们这里对 key 用 keepSlash=true，与官方 demo 一致；签名原文也用同一形态，
// 两边不一致是最常见的"签名正确但七牛说 invalid sign"根因）。
// ---------------------------------------------------------------------------
std::string urlEncode(const std::string& s, bool keepSlash) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool safe = std::isalnum(c) || c == '-' || c == '_' ||
                          c == '.' || c == '~' || (keepSlash && c == '/');
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 哈希原语
// ---------------------------------------------------------------------------
std::string hmacSha1(const std::string& key, const std::string& data) {
    return hmac("SHA1", key, data);
}

std::string hmacSha256(const std::string& key, const std::string& data) {
    return hmac("SHA256", key, data);
}

std::string sha1Hex(const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data.data(), data.size(), out, &len, EVP_sha1(), nullptr);
    return toHex(out, len);
}

// ---------------------------------------------------------------------------
// 1) 上传凭证 uploadToken
// ---------------------------------------------------------------------------
std::string makeUploadToken(const std::string& accessKey,
                            const std::string& secretKey,
                            const std::string& policyJson) {
    // encodedPolicy：策略 JSON 的 urlsafe base64。
    const std::string encodedPolicy = urlSafeBase64Encode(policyJson);

    // 待签串**就是 encodedPolicy 本身**（官方 upload-token 文档口径）。
    // 历史教训（2026-11-20）：这里曾按"管理凭证"的规矩多拼了
    //   + "\n" + urlSafeBase64Encode(uploadPath)
    // 七牛重算 sign 时不含 path，于是每个 uptoken 都验签失败→
    // 表单直传恒定 401 bad token。若将来接入的是**带路径的自建网关**
    // 或官方某个新签名版本，请以官方样例 token 逐字节比对，别改回拼 path。
    const std::string sign = urlSafeBase64Encode(hmacSha1(secretKey, encodedPolicy));

    return accessKey + ":" + sign + ":" + encodedPolicy;
}

// ---------------------------------------------------------------------------
// 2) 私有空间下载签名（token 方案）
// ---------------------------------------------------------------------------
std::string makeDownloadToken(const std::string& accessKey,
                              const std::string& secretKey,
                              const std::string& urlWithoutEQuery,
                              long long deadlineUnix) {
    // 官方规则（C SDK Qiniu_RS_GetPolicy_MakeRequest 同形）：
    //   待签串 = 那一整条 URL（**含 scheme://host**，含除 e/token 以外的已有 query）
    //            + "?e=" + deadline
    // 真实模式的调用方传绝对 URL；mock 传相对路径由自家校验（见 qiniu_signer.h 2)）。
    const std::string data = urlWithoutEQuery + "?e=" + std::to_string(deadlineUnix);
    const std::string sign = urlSafeBase64Encode(hmacSha1(secretKey, data));
    return accessKey + ":" + sign;
}

bool verifyEntryToken(const std::string& entrySecret,
                      const std::string& pathWithQuery,
                      const std::string& token) {
    // token 形如 "<ak>:<urlsafe_b64(hmac_sha1(secret, pathWithQuery))>"。
    const std::size_t colon = token.find(':');
    if (colon == std::string::npos) return false;
    std::string expected;
    expected = urlSafeBase64Encode(hmacSha1(entrySecret, pathWithQuery));
    // 这里用普通比较：入口 token 不是高价值长期凭证（分钟级时效），
    // 恒定时间比较的缺失只意味着极小的计时侧信道，如实记录。
    return expected == token.substr(colon + 1);
}

// ---------------------------------------------------------------------------
// 3) 管理 API 鉴权（官方 "Qiniu 签名算法" v7）
// ---------------------------------------------------------------------------
// 出处与逐字节依据：本仓库 sdk/qiniu-c-sdk-master 的 qiniu/auth_mac.c
//   Qiniu_Mac_AuthV2 → Qiniu_Mac_HmacV2 的待签项序列是
//     {method, " ", path, "\nHost: ", host, "\nContent-Type: ", ct, "\n"}
//     + 每个 X-Qiniu-* 头 {name, ": ", value, "\n"}（name 经
//       canonical_mime_header_key 规范化成 "X-Qiniu-Date" 这种**每段首字母大写**）
//     再由 Qiniu_Mac_Hmac_inner 统一补一个 "\n"，最后才接 body。
//   展开即：
//     "<METHOD> <PATH>[?<QUERY>]\nHost: <HOST>\n[Content-Type: <CT>\n]"
//     ["X-Qiniu-Date: <TS>\n"] "\n" [<BODY>]
// 历史教训（两轮都记在这里，免得第三次再猜）：
//   * 2026-11-20 之前实现的是 q-sign v2（signing_key=hex_sha1(SK)、'&' 拼接）
//     ——七牛根本不认；
//   * 2026-11-20~21 实现的是 "<METHOD>\n<Host>\n<Path>[\n<Body>]"（把 Host 单独
//     占一行、method 与 path 之间是 '\n' 而不是 ' '、也没有尾随空行）——真机
//     实测 rs.stat 仍回 401 {"error":"bad token","error_code":"BadToken"}，
//     X-Reqid 可查。差别就在**这一处口径**：v7 的 method 与 path 用空格连接、
//     Host/Content-Type 是 "Key: value" 头形式、且签名串与 body 之间必有一个
//     空行。自己跟自己都对得上，所以单元测试全绿也照样 401（自洽 ≠ 与官方一致）。
std::string makeMacAuthorization(const std::string& accessKey,
                                 const std::string& secretKey,
                                 const std::string& method,
                                 const std::string& host,
                                 const std::string& path,
                                 const std::string& query,
                                 const std::string& body,
                                 const std::string& contentType,
                                 const std::string& xQiniuDate) {
    std::string signStr = method + " " + path;
    // 空 query 时**不能**留那个 '?'（多一个字符就 401）。
    if (!query.empty()) signStr += "?" + query;      // 调用方保证已按 key 排序 + 编码
    signStr += "\nHost: " + host + "\n";
    // 下面两行是"发了这个头就要签这个头"：object_store.cpp 里 stat/remove
    // 都会把 CT 与 X-Qiniu-Date 实际发出去，签名字段与请求头必须**成对**出现，
    // 少签一个或多签一个都是 BadToken。
    if (!contentType.empty()) signStr += "Content-Type: " + contentType + "\n";
    if (!xQiniuDate.empty())  signStr += "X-Qiniu-Date: "  + xQiniuDate  + "\n";
    signStr += "\n";                                  // 头与 body 之间的空行（必有）
    if (!body.empty()) signStr += body;               // body 必须与实发字节逐字节一致
    const std::string sign = urlSafeBase64Encode(hmacSha1(secretKey, signStr));
    return "Qiniu " + accessKey + ":" + sign;
}

std::string entryBase64(const std::string& key, const std::string& bucket) {
    // ⚠ 官方 entry 的顺序是 **bucket:key**（官方 C SDK qiniu/rs.c：
    //   Qiniu_String_Concat3(bucket, ":", key) 再 urlsafe_base64；Go/Python SDK
    //   同）。参数按 (key, bucket) 传是历史调用点决定的，拼接顺序以官方为准。
    //   写反不会报鉴权错——entry 整段都在签名里，自洽——七牛只会当它是一个
    //   "查无此对象"的 entry，症状是 612/631，极易被误判成"客户端没传上去"。
    return urlSafeBase64Encode(bucket + ":" + key);
}

} // namespace qsign
} // namespace kc
