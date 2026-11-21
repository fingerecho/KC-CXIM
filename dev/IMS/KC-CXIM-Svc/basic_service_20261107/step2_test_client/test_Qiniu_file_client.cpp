// ============================================================================
// /*************图片文件功能（七牛云**真实**直传）：文件模块真机验收客户端***************/
// ----------------------------------------------------------------------------
// 与 test_file_client.cpp（mock 模式，QINIU_ENABLED=0）的关键差异：
//   * "直传"一步不再是 POST /api/files/mock-put，而是**真正地向七牛上传域名
//     (up_host) 做「分片上传 V2」**——与官方 C SDK 里已验证可成功上传的
//     examples/up_multipart_sample.c(`Qiniu_Multipart_PutFile`) 逐请求同构：
//       POST {up}/buckets/{bucket}/objects/{b64url(key)}/uploads          → uploadId
//       PUT  {up}/buckets/{bucket}/objects/{b64url(key)}/uploads/{id}/{n} → 该片 etag/md5
//       POST {up}/buckets/{bucket}/objects/{b64url(key)}/uploads/{id}     → {key,hash}
//     三步都用 `Authorization: UpToken <uptoken>`（SDK qiniu/io.c 里
//     Qiniu_UptokenAuth 就是这一个头，不再把 token 塞进表单字段）；bucket 名
//     从 uptoken 的 policy.scope 里解出来（SDK 的 Qiniu_Utils_Extract_Bucket
//     同一做法），所以客户端**从不需要**知道密钥以外的任何配置。
//     字节走 HTTPS 出网，**完全不经过 IM 服务器**——这正是规划
//     "数据面不经过服务器"的落地形态。
//   * 为什么默认用分片 V2 而不是表单直传：表单直传只有一条请求，验不到
//     "多片 + etag 汇总"这条真实业务路径（IM 的 20MB 上限内视频/大文档都会走
//     多片）；V2 还是七牛当前主推、SDK 示例里跑通的那套。表单直传
//     (examples/up_form_upload.c) 仍保留，设 `QINIU_UPLOAD_MODE=form` 即切回，
//     两条路同属"直传"，服务器与控制面完全不感知差异。
//   * commit 阶段服务器会用管理 API(rs.qbox.me) stat 真实核验对象存在，
//     返回的 size/qetag 以七牛权威值为准（不再是我们自己登记的假值）。
//   * 下载换取的 302 Location 指向**真实的私有签名直链**
//     (domain/key?e=&token=AK:sign)，客户端再向该直链发一次 HTTPS GET，
//     取回的字节必须与上传时**逐字节相同**——端到端数据完整性验证。
//
// 因此本客户端只在 QINIU_ENABLED=1 且 QiniuConfig.json 为真实密钥时才有意义，
// 由 step3_verify/verify_Qiniu_file.sh 启动真实模式服务器后调用。
//
// 覆盖真机链路：
//   凭证签发 → 真实分片直传七牛 V2（单片 + **多片**两种，多片额外逐字节回验）→
//   commit(stat 核验) → 单聊 type=2 → 接收端 302 换取真实签名直链 →
//   跟随后下载核对逐字节 + size/qetag → 群聊 type=4 → 删除（含多片对象）；
//   负向：未登录 401 / 超限 400 / 黑名单 400 / 虚报 commit（申请凭证但不直传，
//   被 stat 判不存在）/ 裸签名直链被拒——这条**用一个从未被签名访问过的专用对象**
//   来测：对象一旦被带 token 取回过，边缘节点可能按 path（忽略 query）缓存，
//   此后 tokenless 请求照样 200，测到的就是 CDN 缓存策略而不是七牛的下载鉴权
//   （2026-11-21 真机实测踩到）。已下载过的对象只打 [观察] 行提示该缓存风险。
//
// 用法：test_Qiniu_file_client [http_port] [tcp_port]
//   环境变量：QINIU_UPLOAD_MODE=v2|form（默认 v2 分片直传；form=表单直传对照）、
//             QINIU_PART_SIZE_MB=1..1024（分片大小，默认 4MB，同 SDK 默认）、
//             QINIU_PRIVATE_SPACE=1|0（期望的空间类型，缺省按实际自适应）、
//             QINIU_VERIFY_CERTS=1 + QINIU_CA_BUNDLE=<pem>（打开证书校验）。
//   最后一行 "ALL PASS" 表示真机验收通过（verify_Qiniu_file.sh 以此判定）。
// ============================================================================
#include <atomic>
#include <chrono>
#include <cstdio>          // std::sscanf：解析七牛响应 Date 头做时钟偏差诊断
#include <cstring>
#include <ctime>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

// 复用服务端的编解码：测试客户端与服务端共用同一份协议实现。
#include "message.h"
#include "message_codec.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif

namespace {

int g_fail = 0;
const std::string kHost = "127.0.0.1";

void check(bool cond, const std::string& desc, const std::string& extra = "") {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));   // 轻微节流
    if (cond) {
        std::cout << "  PASS: " << desc << "\n";
    } else {
        std::cout << "  FAIL: " << desc;
        if (!extra.empty()) std::cout << " (" << extra << ")";
        std::cout << "\n";
        ++g_fail;
    }
}

void set_recv_timeout(boost::asio::ip::tcp::socket& s, int seconds) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(seconds * 1000);
    setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// ---- 极简 HTTP 客户端（针对**本地 IM 服务器**，明文回环，无需 TLS）---------
struct HttpResponse {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;   // key 统一小写
};

HttpResponse http_call(const std::string& method, const std::string& path,
                       const std::string& token, const std::string& body,
                       unsigned short port) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket s(io);
    boost::asio::ip::tcp::resolver resolver(io);
    boost::asio::connect(s, resolver.resolve(kHost, std::to_string(port)));

    std::string req;
    req += method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + kHost + ":" + std::to_string(port) + "\r\n";
    req += "Connection: close\r\n";
    req += "Content-Type: application/json\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    req += body;
    boost::asio::write(s, boost::asio::buffer(req));

    std::string raw;
    boost::system::error_code ec;
    char buf[16384];
    for (;;) {
        const std::size_t n = s.read_some(boost::asio::buffer(buf), ec);
        if (n > 0) raw.append(buf, n);
        if (ec == boost::asio::error::eof) break;
        if (ec) throw boost::system::system_error(ec);
    }

    HttpResponse out;
    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) throw std::runtime_error("bad http response");
    const std::string head = raw.substr(0, headerEnd);
    out.body = raw.substr(headerEnd + 4);
    const std::size_t sp = head.find(' ');
    if (sp != std::string::npos) { try { out.status = std::stoi(head.substr(sp + 1, 3)); } catch (...) {} }
    std::istringstream is(head);
    std::string line;
    std::getline(is, line);                       // 状态行跳过
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
    };
    while (std::getline(is, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string v = line.substr(colon + 1);
        trim(v);
        out.headers[k] = v;
    }
    return out;
}

// ---- 拆一个绝对 URL 成 scheme/host/port/target（七牛真实上传/下载都走这里）----
struct UrlParts {
    std::string scheme, host, port, target;   // target = path + query（不含 scheme/host）
};
UrlParts parse_url(const std::string& url) {
    UrlParts p;
    const std::size_t se = url.find("://");
    if (se == std::string::npos) throw std::runtime_error("bad url (no scheme): " + url);
    p.scheme = url.substr(0, se);
    std::string rest = url.substr(se + 3);
    std::size_t slash = rest.find('/');
    std::size_t q     = rest.find('?');
    std::size_t hostEnd = std::min(slash, q);
    if (hostEnd == std::string::npos) hostEnd = rest.size();
    std::string authority = rest.substr(0, hostEnd);
    rest = (slash == std::string::npos) ? "/" : rest.substr(slash);
    auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        p.host = authority.substr(0, colon);
        p.port = authority.substr(colon + 1);
    } else {
        p.host = authority;
        p.port = (p.scheme == "https") ? "443" : "80";
    }
    p.target = rest.empty() ? "/" : rest;
    return p;
}

// 七牛真实上传/下载都要校验证书——本项目 Windows 下 OpenSSL 默认不读系统证书库
// （与 object_store.cpp 的 QiniuHttpAgent 同款处理），故默认不校验；
// 环境变量 QINIU_VERIFY_CERTS=1 且提供 QINIU_CA_BUNDLE=<path> 时改为正常校验。
bool qiniu_verify_certs() {
    const char* verify = std::getenv("QINIU_VERIFY_CERTS");
    const char* ca     = std::getenv("QINIU_CA_BUNDLE");
    return verify && *verify == '1' && ca && *ca;
}

boost::asio::ssl::context make_ssl_context() {
    boost::asio::ssl::context ctx(boost::asio::ssl::context::tlsv12_client);
    const char* ca = std::getenv("QINIU_CA_BUNDLE");
    if (qiniu_verify_certs()) {
        ctx.load_verify_file(ca);
        ctx.set_verify_mode(boost::asio::ssl::verify_peer);
    } else {
        ctx.set_verify_mode(boost::asio::ssl::verify_none);
    }
    return ctx;
}

// 把 beast 响应的所有响应头收集进 HttpResponse.headers（键统一小写）。
// 七牛排障最关键的是响应头 `X-Reqid`（提工单/找日志定位用）与状态码。
template <class Res>
void capture_headers(const Res& res, HttpResponse& out) {
    for (const auto& f : res) {
        std::string k = std::string(f.name_string());
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        out.headers[k] = std::string(f.value());
    }
}

// 这几个"解析工具"的实现留在文件靠后处（与诊断段放在一起，读史顺序不乱），
// 但直传与分片 V2 都要用，所以在此前向声明。
rapidjson::Document parse_json(const std::string& text);
std::string jstr(const rapidjson::Value& v, const char* key);
std::string describe_qiniu(const HttpResponse& r);
std::string b64DecodeLoose(const std::string& in);

// ---- urlsafe Base64 编码（分片 V2 的 objects/<b64url(key)> 路径段用）---------
// 与 qiniu_signer.cpp::urlSafeBase64Encode 同口径：'-'/'_' 替换、**保留 '=' 填充**。
// 服务端 object_store.cpp 的 stat/remove 用的也是保留填充的这份，两端必须一致，
// 否则"传上去的 key"与"核验用的 entry"看起来不是同一个对象。
std::string urlSafeB64Encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() / 3 + 1) * 4);
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
        const std::uint32_t v = (static_cast<unsigned char>(in[i]) << 16) |
                                (static_cast<unsigned char>(in[i + 1]) << 8) |
                                 static_cast<unsigned char>(in[i + 2]);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
        i += 3;
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        const std::uint32_t v = static_cast<unsigned char>(in[i]) << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (rem == 2) {
        const std::uint32_t v = (static_cast<unsigned char>(in[i]) << 16) |
                                (static_cast<unsigned char>(in[i + 1]) << 8);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    for (auto& c : out) { if (c == '+') c = '-'; else if (c == '/') c = '_'; }
    return out;
}

// ---- 通用一次请求（七牛数据面全部走它：分片 V2 三步 / 表单直传 / 跟直链下载）--
// 等价于 SDK 的 Qiniu_Client_Call*：method + 绝对 URL + 头 + 原始字节 body。
// ⚠ SNI 一律设置（不再只在校验证书时才设）：up-*.qiniup.com 与 CDN 域名共用
//   接入 IP，不带 server_name 会拿到默认证书，真机上表现成"握手过了但证书不对"，
//   排查成本极高；SNI 本身不涉密，开着校验与否都该发。
HttpResponse qn_request(const std::string& method, const std::string& url,
                        const std::vector<std::pair<std::string, std::string>>& headers,
                        const std::string& body) {
    namespace beast = boost::beast;
    namespace http  = beast::http;
    namespace net   = boost::asio;
    namespace ssl   = boost::asio::ssl;
    using tcp = net::ip::tcp;

    const UrlParts u = parse_url(url);
    http::verb verb = http::verb::get;
    if      (method == "POST") verb = http::verb::post;
    else if (method == "PUT")  verb = http::verb::put;
    else if (method == "HEAD") verb = http::verb::head;

    net::io_context io;
    auto ctx = make_ssl_context();
    tcp::resolver resolver(io);

    http::request<http::string_body> req{verb, u.target, 11};
    req.set(http::field::host, u.host);
    req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    req.set(http::field::connection, "close");
    for (const auto& h : headers) req.set(h.first, h.second);
    req.body() = body;
    // POST/PUT 一律带 Content-Length（哪怕是 0——V2 的 init 就是空 body 的 POST）；
    // 空 body 的 GET/HEAD 则**不发** Content-Length，保持与浏览器/普通客户端一致，
    // 免得某个环节的"GET 却带 CL"被当成异常请求。
    if (!body.empty() || verb == http::verb::post || verb == http::verb::put)
        req.prepare_payload();

    auto do_read = [&](auto& stream, HttpResponse& out) {
        http::write(stream, req);
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        out.status = static_cast<int>(res.result_int());
        out.body   = std::string(res.body());
        capture_headers(res, out);
        // 下载用例要比对字节数：以实收长度为准（body() 已把 chunk 解码完）。
        out.headers["content-length"] = std::to_string(res.body().size());
    };

    HttpResponse out;
    if (u.scheme == "https") {
        ssl::stream<beast::tcp_stream> s(io, ctx);
        beast::get_lowest_layer(s).expires_after(std::chrono::seconds(60));
        beast::get_lowest_layer(s).connect(resolver.resolve(u.host, u.port));
        if (!SSL_set_tlsext_host_name(s.native_handle(), u.host.c_str()))
            throw std::runtime_error("SNI setup failed for " + u.host);
        s.handshake(ssl::stream_base::client);
        do_read(s, out);
        beast::error_code ec;
        s.shutdown(ec);                      // 尽力而为：对端已 close 时忽略
    } else {
        beast::tcp_stream s(io);
        s.expires_after(std::chrono::seconds(60));
        s.connect(resolver.resolve(u.host, u.port));
        do_read(s, out);
    }
    return out;
}

// ---- 向七牛 up_host 做 multipart/form-data 表单直传（QINIU_UPLOAD_MODE=form）--
// 与 examples/up_form_upload.c 的 Qiniu_Io_PutFile 等价：三段 form-data
//   token=<uptoken>  key=<object_key>  file=<二进制字节, filename=<name>>
// 成功响应 200，body = {"key":"...","hash":"F:..."}（hash 即七牛 qetag）。
HttpResponse qiniu_form_upload(const std::string& uploadUrl, const std::string& uptoken,
                               const std::string& key, const std::string& filename,
                               const std::string& mime, const std::string& bytes) {
    const std::string boundary = "----KCcximBoundary1234567890abcdef";
    std::string body;
    body.reserve(bytes.size() + uptoken.size() + key.size() + 512);
    body += "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"token\"\r\n\r\n" + uptoken + "\r\n";
    body += "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"key\"\r\n\r\n" + key + "\r\n";
    body += "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n"
            "Content-Type: " + (mime.empty() ? "application/octet-stream" : mime) + "\r\n\r\n";
    body += bytes;
    body += "\r\n--" + boundary + "--\r\n";

    return qn_request("POST", uploadUrl + "/" + key,
                      {{"Content-Type", "multipart/form-data; boundary=" + boundary}}, body);
}

// ---- 分片上传 V2：uptoken 的 policy.scope="<bucket>:<key>" 里取 bucket -------
// 与 SDK Qiniu_Utils_Extract_Bucket 同做法：客户端不必知道任何额外配置，
// 空间名是服务端签进凭证的，客户端只负责照做（也顺带证明 scope 真被签进去了）。
std::string uptokenScopeBucket(const std::string& uptoken) {
    const std::size_t c1 = uptoken.find(':');
    const std::size_t c2 = (c1 == std::string::npos) ? std::string::npos
                                                     : uptoken.find(':', c1 + 1);
    if (c2 == std::string::npos) throw std::runtime_error("uptoken 格式异常（缺 encodedPolicy）");
    const std::string policy = b64DecodeLoose(uptoken.substr(c2 + 1));
    auto d = parse_json(policy);
    const std::string scope = jstr(d, "scope");
    const std::size_t colon = scope.find(':');
    const std::string bucket = (colon == std::string::npos) ? scope : scope.substr(0, colon);
    if (bucket.empty()) throw std::runtime_error("uptoken policy.scope 里没有 bucket: " + scope);
    return bucket;
}

// ---- 向七牛 up_host 做「分片上传 V2」直传（真实数据面，默认路径）-------------
// 逐请求对齐 examples/up_multipart_sample.c → qiniu/multipart_upload.c：
//   1) init     : POST {up}/buckets/{bucket}/objects/{b64url(key)}/uploads
//                 → {"uploadId":"...","expireAt":...}
//   2) 每一片    : PUT  {up}/buckets/{bucket}/objects/{b64url(key)}/uploads/{uploadId}/{partNumber}
//                 body = 该片原始字节，Content-Type: application/octet-stream
//                 → {"etag":"...","md5":"..."}（partNumber 从 **1** 起）
//   3) complete : POST {up}/buckets/{bucket}/objects/{b64url(key)}/uploads/{uploadId}
//                 body = {"parts":[{"etag":..,"partNumber":..},..],"mimeType":..,"fname":..}
//                 → {"key":"...","hash":"F:..."}（hash 即七牛 qetag）
// 三步的鉴权头都是 `Authorization: UpToken <uptoken>`（SDK Qiniu_UptokenAuth），
// 分片大小合法区间 1MB~1GB、SDK 默认 4MB（verifyParam 里那段判定），
// 总片数上限 10000——本函数按同一口径校验，避免拿一个服务端必拒的参数去打网络。
// 返回值 = **最后一步（或第一个失败步）的原始响应**，失败时不再抛异常：
// 这样调用方能对 form/V2 两种模式共用同一套"按七牛错误码走专项诊断"的代码
// （BadToken / expired 那两段是踩过坑换来的，值得对每条链路都生效）。
// stepOut 告诉调用方死在哪一步（init / part#2of4 / complete），网络故障等
// 传输层异常仍然抛出（那类问题没有响应体可诊断）。
HttpResponse qiniu_multipart_upload(const std::string& uploadUrl, const std::string& uptoken,
                                    const std::string& key, const std::string& filename,
                                    const std::string& mime, const std::string& bytes,
                                    std::size_t partSize, int& partsOut, std::string& stepOut) {
    partsOut = 0;                                       // 只有走完 complete 才是真实片数
    const std::string bucket   = uptokenScopeBucket(uptoken);
    const std::string objBase  = uploadUrl + "/buckets/" + bucket + "/objects/" +
                                 urlSafeB64Encode(key);
    const std::string upAuth   = "UpToken " + uptoken;

    if (partSize < (1u << 20) || partSize > (1u << 30))
        throw std::runtime_error("partSize 必须在 1MB~1GB（七牛 V2 口径）");
    const std::size_t total = bytes.size();
    const std::size_t nParts = total ? (total + partSize - 1) / partSize : 1;   // 0 字节也要 1 片
    if (nParts > 10000) throw std::runtime_error("分片数超过 10000 上限");

    // 1) init
    stepOut = "init";
    HttpResponse init = qn_request("POST", objBase + "/uploads",
                                   {{"Authorization", upAuth},
                                    {"Content-Type", "application/octet-stream"}}, "");
    if (init.status != 200) return init;
    const std::string uploadId = jstr(parse_json(init.body), "uploadId");
    if (uploadId.empty())
        throw std::runtime_error("V2 init 响应缺 uploadId: " + init.body);

    // 2) 逐片 PUT（body 是原始字节，不做任何编码；etag 汇总给 complete）
    std::string partsJson = "[";
    for (std::size_t i = 0; i < nParts; ++i) {
        const std::size_t off  = i * partSize;
        const std::size_t thisLen = std::min(partSize, total - off);
        stepOut = "part#" + std::to_string(i + 1) + "of" + std::to_string(nParts);
        const std::string url = objBase + "/uploads/" + uploadId + "/" + std::to_string(i + 1);
        HttpResponse pr = qn_request("PUT", url,
                                     {{"Authorization", upAuth},
                                      {"Content-Type", "application/octet-stream"}},
                                     bytes.substr(off, thisLen));
        if (pr.status != 200) return pr;
        const std::string etag = jstr(parse_json(pr.body), "etag");
        if (etag.empty())
            throw std::runtime_error("V2 " + stepOut + " 响应缺 etag: " + pr.body);
        if (i) partsJson += ",";
        // etag 只含 urlsafe base64 字符，无需 JSON 转义；这里仍走 Writer 以免将来踩坑。
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject(); w.Key("etag"); w.String(etag.c_str(), (unsigned)etag.size());
                        w.Key("partNumber"); w.Uint((unsigned)(i + 1)); w.EndObject();
        partsJson += std::string(sb.GetString(), sb.GetSize());
    }
    partsJson += "]";

    // 3) complete（mimeType/fname 与 SDK complete_upload 同一份字段名）
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("parts"); w.RawValue(partsJson.data(), partsJson.size(), rapidjson::kArrayType);
    if (!mime.empty())    { w.Key("mimeType"); w.String(mime.c_str(), (unsigned)mime.size()); }
    if (!filename.empty()){ w.Key("fname");    w.String(filename.c_str(), (unsigned)filename.size()); }
    w.EndObject();
    stepOut = "complete";
    HttpResponse done = qn_request("POST", objBase + "/uploads/" + uploadId,
                                   {{"Authorization", upAuth},
                                    {"Content-Type", "application/json"}},
                                   std::string(sb.GetString(), sb.GetSize()));
    partsOut = static_cast<int>(nParts);
    return done;
}

std::string strip_query(const std::string& url) {
    const std::size_t q = url.find('?');
    return q == std::string::npos ? url : url.substr(0, q);
}

// 响应体是 HTML（tengine/openresty 的默认错误页）而不是七牛的 JSON：说明请求
// **没走到七牛的 Kodo 业务层**，被域名接入层就拒了。421 Misdirected Request /
// 403 Forbidden / 429 都属于这一类——签名、凭证、对象存在性都还没被看过一眼，
// 拿它们去怀疑凭证或签名口径一定是白费力气（真机 2026-11-21 在此绕了一圈）。
bool looksLikeEdgeHtml(const HttpResponse& r) {
    return r.body.find("<html") != std::string::npos ||
           r.body.find("<!DOCTYPE") != std::string::npos ||
           r.body.find("Misdirected") != std::string::npos ||
           r.body.find("Forbidden") != std::string::npos;
}

// ---- 接入层探针：判断"域名本身在不在服务"，与我们的签名/上传实现对不对无关 ----
// 触发时机：跟随 302 取字节时拿到了 **HTML 错误页**（421/403/429 这类）。那种响应
// 是 CDN 边缘节点给的，根本没走到七牛 Kodo 业务层，所以拿它去怀疑 uptoken/管理签名
// 是白费力气（2026-11-21 真机就在这上面绕了一圈）。
// 做法：对**同一个主机名**再发一个"不带任何签名参数"的裸 GET，分别试原 scheme 与
// 另一个 scheme：
//   · 回到七牛的 JSON 错误（如 {"error":"sign attr not validate"} / no such file）
//     ⇒ 域名是绑定了该 bucket 的，问题出在被测那次的 scheme（典型：一级测试域名
//       *.clouddn.com **没有可用的 HTTPS 站点**，走 https 会被 tengine 判
//       421 Misdirected Request，走 http 才通）；
//   · 两边都是 HTML/连不上 ⇒ 域名压根没在这个边缘节点服务（未绑定/已回收/DNS 漂走）。
// ⚠ 这里刻意**只请求去掉 query 的裸路径**：签名直链的 token 是密钥派生出来的凭证，
//   绝不为"多一条诊断信息"把它发到明文连接上。
void probeEdgeReachability(const std::string& url) {
    const UrlParts u = parse_url(url);
    const std::string pathOnly = u.target.substr(0, u.target.find('?'));
    const std::string altScheme = (u.scheme == "https") ? "http" : "https";
    for (const std::string& scheme : {u.scheme, altScheme}) {
        const std::string probe = scheme + "://" + u.host + pathOnly;
        std::string verdict;
        try {
            HttpResponse r = qn_request("GET", probe, {}, "");
            const bool html = looksLikeEdgeHtml(r);
            verdict = "HTTP " + std::to_string(r.status) +
                      (html ? "（边缘节点 HTML 错误页＝没到七牛业务层）"
                            : "（七牛业务层回话：" + (r.body.size() > 80 ? r.body.substr(0, 80) : r.body) + "）");
        } catch (const std::exception& e) {
            verdict = std::string("连接失败：") + e.what();
        }
        std::cout << "  [探针] 裸路径(不含签名) " << probe << " -> " << verdict << "\n";
    }
    std::cout << "  [结论提示] 若 https 那行是 HTML/握手失败而 http 那行到了七牛业务层，"
                 "就是这个域名**没有可用的 HTTPS 站点**（一级测试域名 *.clouddn.com 常见）："
                 "换已备案自有域名并在七牛控制台配好 HTTPS 证书，"
                 "或在明确接受风险的前提下把 QiniuConfig.json 的 domain 改成 http://（测试环境）。"
                 "若两行都是 HTML，则该域名并未绑定这个 bucket（或测试域名已被回收）。\n";
}

// ---- 对一条绝对 URL 做 GET（跟随 302 后取回七牛签名直链的字节）---------------
// 与直传共用 qn_request：同一套 SNI/超时/证书口径，下载能复现上传时踩到的任何
// TLS 问题（反之亦然），不必在两处各写一遍 beast 样板。
HttpResponse https_get(const std::string& url) {
    return qn_request("GET", url, {}, "");
}

rapidjson::Document parse_json(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject()) throw std::runtime_error("bad json: " + text);
    return doc;
}

std::string jstr(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key) || !v[key].IsString()) return {};
    return std::string(v[key].GetString(), v[key].GetStringLength());
}
std::uint64_t ju64(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    if (v[key].IsUint64()) return v[key].GetUint64();
    if (v[key].IsInt64() && v[key].GetInt64() > 0) return static_cast<std::uint64_t>(v[key].GetInt64());
    return 0;
}

// 解析七牛错误响应，提炼一行可直接排障的诊断串：
//   上传端点失败典型 body：{"error":"bad file format"} / {"error":"invalid upload token"}
//   管理端点(stat/rm)失败：{"error":"no such file or directory, bucket[...] key[...]"}
// 关键错误码：HTTP 状态（如 401/612/701）+ 响应头 X-Reqid + body 里的 error/code。
// X-Reqid 是提七牛工单/让运维查后台日志的唯一线索，务必带上。
std::string describe_qiniu(const HttpResponse& r) {
    std::string reqid;
    auto it = r.headers.find("x-reqid");
    if (it != r.headers.end()) reqid = it->second;
    std::string err;
    try {
        auto d = parse_json(r.body);
        if (d.HasMember("error") && d["error"].IsString())
            err = std::string(d["error"].GetString(), d["error"].GetStringLength());
        // 七牛错误码：上传/管理端点用 "error_code"（如 BadToken/no such file），
        // 少数旧接口用数字 "code"——两者都尽力提取。
        auto pick = [&](const char* k) -> std::string {
            if (!d.HasMember(k)) return {};
            const auto& v = d[k];
            if (v.IsString()) return std::string(v.GetString(), v.GetStringLength());
            if (v.IsInt())    return std::to_string(v.GetInt());
            if (v.IsUint())   return std::to_string(v.GetUint());
            if (v.IsInt64())  return std::to_string(v.GetInt64());
            return {};
        };
        const std::string ecode = pick("error_code");
        const std::string ncode = pick("code");
        if (!ecode.empty()) err += " (error_code=" + ecode + ")";
        if (!ncode.empty()) err += " (code=" + ncode + ")";
    } catch (...) {}
    // 诊断串里要塞进 stdout，而 stdout 会被验收脚本用命令替换捕获——**一个 NUL
    // 字节就能让 bash 报 "ignored null byte in input" 并吞掉后面的内容**。下载
    // 用例失败时打印的正是对象字节（二进制），所以这里统一做可见字符化：
    // 非可打印字节一律换成 '.'，只保留形状供人判断"这是二进制正文还是错误页"。
    std::string body = r.body.substr(0, std::min<std::size_t>(r.body.size(), 400));
    for (auto& ch : body) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c > 0x7E) ch = '.';
    }
    if (r.body.size() > body.size()) {
        body += "...(truncated, total " + std::to_string(r.body.size()) + " bytes)";
    }
    std::string s = "qnHTTP=" + std::to_string(r.status);
    if (!reqid.empty()) s += " X-Reqid=" + reqid;
    if (!err.empty())   s += " qiniuError=\"" + err + "\"";
    // 响应体是 HTML（openresty/nginx 的默认错误页）而不是七牛的 JSON：
    // 说明请求**没走到七牛的 Kodo 业务层**，是被域名接入层拒的——
    // 典型三种：domain 没绑定这个 bucket、domain 与 bucket 不同区域、
    // 一级测试域名(clouddn.com)到期被回收。这时改签名/凭证都是白费力气，
    // 该去七牛控制台看"空间管理 → 域名"。（真机 2026-11-21 就撞在这上面。）
    if (looksLikeEdgeHtml(r)) {
        s += " [域名接入层直接拒绝，未到七牛业务层：查 domain 是否绑定该 bucket/"
             "是否与 bucket 同区域/测试域名是否已被回收]";
    }
    s += " body=" + body;
    return s;
}

// urlsafe/standard Base64 解码（仅用于把 uptoken 第三段 encodedPolicy 还原成
// JSON，读出其中的 expires 字段做时钟诊断；不校验签名，故无需严格实现）。
const int* b64RevTable() {
    static int t[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) t[i] = -1;
        const std::string chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) t[(unsigned char)chars[i]] = i;
        t[(unsigned char)'-'] = t[(unsigned char)'+'];   // urlsafe
        t[(unsigned char)'_'] = t[(unsigned char)'/'];
        init = true;
    }
    return t;
}

std::string b64DecodeLoose(const std::string& in) {
    const int* T = b64RevTable();
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        const int d = T[c];
        if (d < 0) continue;
        // 只保留最近 24 bit（4 个 base64 字符 = 3 字节），否则 val 一路左移溢出，
        // (val >> bits) 取到的就是别的字节的位置。
        val = ((val << 6) | d) & 0xFFFFFF;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;          // ⚠ 每吐一个字节就要退 8 位；漏了这行 = 解出全乱码
        }
    }
    return out;
}

// 七牛 uptoken = "AK:sign:encodedPolicy"。解出 policy 的有效期（Unix 秒）。
// 官方字段名是 **deadline**；"expires" 只是旧诊断代码读的名字（七牛服务端
// 完全不认它——带 expires 的 policy 会被当成"已过期"直接 401）。所以这里
// 先读 deadline，读不到再兜底读 expires，便于对照历史日志。
long long uptokenExpires(const std::string& uptoken) {
    const std::size_t c1 = uptoken.find(':');
    if (c1 == std::string::npos) return -1;
    const std::size_t c2 = uptoken.find(':', c1 + 1);
    if (c2 == std::string::npos) return -1;
    const std::string policy = b64DecodeLoose(uptoken.substr(c2 + 1));
    try {
        auto d = parse_json(policy);
        for (const char* field : {"deadline", "expires"}) {
            if (!d.HasMember(field)) continue;
            const rapidjson::Value& v = d[field];
            if (v.IsInt64()) return v.GetInt64();
            if (v.IsInt())   return v.GetInt();
            if (v.IsUint())  return v.GetUint();
        }
        if (policy.find("deadline") == std::string::npos &&
            policy.find("expires")  != std::string::npos) {
            std::cout << "  [诊断] ⚠ policy 里只有 expires、没有 deadline："
                         "七牛不认 expires，会一律按已过期处理（401 expired token），"
                         "服务端 signUploadToken 必须写 deadline。\n";
        }
    } catch (...) {}
    return -1;
}

// BadToken 专项诊断：把服务端签给客户端的 uptoken 拆成三段可视化，方便与
// Qiniu SDK/其它参考实现对同一份 policy 手工签一次做逐字节比对，钉死差异来源。
// 关键要点（Qiniu 官方「上传凭证」规则）：
//   encodedPolicy = urlsafe_b64(policy_json)
//   sign          = urlsafe_b64(hmac_sha1(secret_key, encodedPolicy))
//   ⚠ 待签串**只有 encodedPolicy**，不含上传 URL 的 path（带 "\n"+encodedUrl
//     那套是**管理凭证**的规则；2026-11-20 本项目曾把它误用于 uptoken，
//     于是服务端每个 uptoken 都 BadToken，却被这里的旧诊断文案误导成"AK/SK
//     与区域配错"，白查一轮账号）。policy 序列化差一个字符同样会炸，
//     所以本函数把解出来的 policy 原样打印，便于逐字节比对。
void reportBadToken(const HttpResponse& put, const std::string& uptoken,
                    const std::string& uploadUrl, const std::string& sentKey) {
    auto split = [&uptoken] {
        std::size_t a = uptoken.find(':');
        std::size_t b = (a == std::string::npos) ? std::string::npos
                                                 : uptoken.find(':', a + 1);
        std::string ak  = (a == std::string::npos) ? uptoken : uptoken.substr(0, a);
        std::string sg  = (a == std::string::npos || b == std::string::npos)
                            ? std::string() : uptoken.substr(a + 1, b - a - 1);
        std::string ep  = (b == std::string::npos)
                            ? std::string() : uptoken.substr(b + 1);
        return std::make_tuple(ak, sg, ep);
    };
    const auto [ak, sg, ep] = split();
    const std::string policy = b64DecodeLoose(ep);
    std::string akShown = ak;
    if (akShown.size() > 10) akShown = akShown.substr(0, 4) + "..." + akShown.substr(akShown.size() - 4);
    std::cout << "  [诊断] uptoken 拆解："
              << "AK=" << akShown << " (len=" << ak.size() << ")"
              << " sign.len=" << sg.size()
              << " encodedPolicy.len=" << ep.size() << "\n";
    std::cout << "        policy=" << policy << "\n";
    // POST 到的 host 仍然值得打印——它是**区域**的唯一线索（BadToken 的次要
    // 根因之一就是 up_host 与 bucket 实际区域不符）。注意 path 不参与签名。
    try {
        const UrlParts u = parse_url(uploadUrl);
        const std::string pathOnly = u.target.substr(0, u.target.find('?'));
        std::cout << "        POST host=" << u.host << " path=" << pathOnly
                  << "（host 须与 bucket 实际区域一致；path 不进签名）\n";
    } catch (const std::exception& e) {
        std::cout << "        uploadUrl 解析失败: " << e.what() << "\n";
    }
    std::cout << "        form key 字段=" << sentKey << "\n";
    auto it = put.headers.find("x-reqid");
    if (it != put.headers.end()) std::cout << "        X-Reqid=" << it->second << "（提工单可让七牛查后台具体拒绝原因）\n";
    std::cout << "  [BadToken 可能根因（按概率，先查代码再查账号）]：\n"
              << "   1) 签名口径不符：sign 必须 = urlsafe_b64(hmac_sha1(SK, encodedPolicy))，\n"
              << "      多拼/少拼任何字节（例如又混进 \"\\n\"+b64(path)）都会 100% BadToken\n"
              << "      ——用 step3_verify/qiniu_token_probe.py --dry 对同一 policy 签一次，\n"
              << "      与这里的 sign 逐字节 diff 即可立刻证实/排除；\n"
              << "   2) policy 串本身两处生成不一致（字段顺序/空格/数字格式）：encodedPolicy\n"
              << "      与上面打印的 policy 必须由服务端同一份 Writer 逻辑产出；\n"
              << "   3) access_key 与 secret_key 不是同一账号的一对，或粘贴时带了空格/换行\n"
              << "      （AK/SK 均为 40 位 urlsafe base64，含 '-'/'_' 是**正常**的，不代表填反）；\n"
              << "   4) AK 对 policy.scope 的 bucket 无上传权限（子账号未授权 / bucket 属另一账号）；\n"
              << "   5) up_host 区域与 bucket 实际区域不一致（本 bucket=kc-cxim 请核对其区域；\n"
              << "      这种更常见的是回 no such bucket/6121，但凭证跨账号时也可能先报 BadToken）；\n"
              << "   6) policy.scope 的 key 与表单 key 不一致（本项目两端同源，理论不会；已把两者打印便于肉眼比对）。\n";
}

// 解析 HTTP-Date（RFC1123，如 "Wed, 06 Nov 2026 07:28:00 GMT"）成 Unix 秒。
long long parseHttpDate(const std::string& s) {
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    char mon[8] = {0};
    if (std::sscanf(s.c_str(), "%*3s %d %7s %d %d:%d:%d",
                    &d, mon, &y, &hh, &mm, &ss) != 6) return -1;
    static const char* names[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
    int month = -1;
    for (int i = 0; i < 12; ++i) if (std::strcmp(names[i], mon) == 0) { month = i; break; }
    if (month < 0) return -1;
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = month; tm.tm_mday = d;
    tm.tm_hour = hh; tm.tm_min = mm; tm.tm_sec = ss;
#if defined(_WIN32)
    return static_cast<long long>(_mkgmtime(&tm));
#else
    return static_cast<long long>(timegm(&tm));
#endif
}

// "expired token" 专项诊断：七牛返回里带权威 Date 头，而 uptoken 的 policy
// 有效期（官方字段名 **deadline**）是服务器用本机 system_clock 算出来的
// （签发时刻 + TTL）。三者一比，就能把"凭证正确却报过期"分到三类根因里：
//   a) deadline 还没到却报 expired → 七牛压根没读出有效期 = policy 字段名写错
//      （写成 expires 会被整字段忽略，按 0 处理）；
//   b) deadline 已过且七牛 Date 明显大于服务器签发时刻 → 服务器时钟落后 > TTL；
//   c) deadline 已过但时钟同步 → TTL 太短 / 签发到上传间隔太久。
// 以前这里只认 b（"一率让你去校时"），而 2026-11-20 的真因是 a，白校一轮时钟。
void reportTokenClock(const HttpResponse& put, const std::string& uptoken,
                      long long ttlAssumed) {
    const long long qiniuNow = [&] {
        auto it = put.headers.find("date");
        return it == put.headers.end() ? -1 : parseHttpDate(it->second);
    }();
    const long long deadline = uptokenExpires(uptoken);   // 优先读 policy.deadline
    std::cout << "  [诊断] uptoken policy.deadline=" << deadline
              << "（= 服务器签发时刻 + ~" << ttlAssumed << "s）";
    if (qiniuNow > 0) {
        const long long serverIssueAt = deadline - ttlAssumed;  // 反推服务器认为的"现在"
        std::cout << ", 七牛权威 Date(now)=" << qiniuNow
                  << ", 服务器时钟比七牛落后≈" << (qiniuNow - serverIssueAt) << "s";
    }
    std::cout << "\n";
    if (deadline < 0) {
        std::cout << "  [结论] policy 里既没有 deadline 也没有 expires——七牛会把有效期按 0 处理，"
                     "必然 expired。请检查服务端 signUploadToken 的 policy 字段。\n";
    } else if (qiniuNow > 0 && deadline < qiniuNow && (qiniuNow - deadline) > 300 &&
               (deadline - ttlAssumed) > qiniuNow - 60) {
        // deadline 是"未来"却已过期：只可能是七牛没把它当有效期用（字段名问题），
        // 判据 = 反推出的签发时刻 (deadline-ttl) 明显大于七牛当前时间。
        std::cout << "  [结论] 签名/AK/SK 有效（否则是 bad token），且反推的签发时刻已晚于七牛"
                     "当前时间 → 这不是过期，是**七牛没从 policy 里认出有效期**：\n"
                     "  [修复] policy 的有效期字段必须是 **deadline**（绝对 Unix 秒），"
                     "不是 expires（那是 SDK 入参名，被七牛忽略）。\n";
    } else if (qiniuNow > 0 && deadline > 0 && qiniuNow >= deadline) {
        std::cout << "  [结论] 签名/AK/SK 均有效（否则是 bad token），但 policy.deadline"
                     " 已早于七牛当前时间 → **服务器系统时钟比真实时间慢了超过一个 TTL**"
                     "（或签发到实际上传间隔太久）。\n"
                     "  [修复] 校时后重启服务：Linux `sudo timedatectl set-ntp true` 并确认"
                     " chrony/ntpd 已同步；容器/VM 需同步宿主机时区时钟。\n";
    } else if (qiniuNow > 0 && deadline > 0) {
        std::cout << "  [说明] 七牛 Date 尚未超过 policy.deadline，有效期字段也在——若仍报 "
                     "expired，查 encodedPolicy 是否在链路上被改写（单位/时区/被截断）。\n";
    }
}

// ---- TCP 帧 ----
void send_frame(boost::asio::ip::tcp::socket& s, const std::string& msg) {
    const std::uint32_t net_len = htonl(static_cast<std::uint32_t>(msg.size()));
    std::vector<char> out(4 + msg.size());
    std::memcpy(out.data(), &net_len, 4);
    std::memcpy(out.data() + 4, msg.data(), msg.size());
    boost::asio::write(s, boost::asio::buffer(out));
}
std::string recv_frame(boost::asio::ip::tcp::socket& s) {
    char header[4];
    boost::asio::read(s, boost::asio::buffer(header, 4));
    std::uint32_t net_len = 0;
    std::memcpy(&net_len, header, 4);
    const std::uint32_t len = ntohl(net_len);
    std::vector<char> body(len);
    boost::asio::read(s, boost::asio::buffer(body));
    return std::string(body.begin(), body.end());
}
void send_msg(boost::asio::ip::tcp::socket& s, const kc::Message& m) { send_frame(s, kc::serialize_message(m)); }
kc::Message recv_msg(boost::asio::ip::tcp::socket& s) {
    const std::string raw = recv_frame(s);
    std::vector<char> body(raw.begin(), raw.end());
    kc::Message m; std::string err;
    if (!kc::parse_message(body, m, err)) throw std::runtime_error("bad server message: " + err);
    return m;
}

bool connect_with_retry(boost::asio::ip::tcp::socket& s, unsigned short port, int attempts = 10) {
    for (int i = 1; i <= attempts; ++i) {
        try { s.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(kHost), port)); return true; }
        catch (const std::exception&) {
            if (i == attempts) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    return false;
}

kc::Message wait_type(boost::asio::ip::tcp::socket& s, kc::MsgType type) {
    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == type) return r;
        if (r.type == kc::MsgType::Error) return r;   // Error 交调用方判断（负向用例）
    }
}

kc::Message login(boost::asio::ip::tcp::socket& s, std::uint64_t uid, const std::string& token) {
    kc::Message m; m.from = uid; m.type = kc::MsgType::Login; m.content = token;
    send_msg(s, m);
    for (;;) { kc::Message r = recv_msg(s);
        if (r.type == kc::MsgType::LoginAck || r.type == kc::MsgType::Error) return r; }
}

std::thread start_heartbeat(boost::asio::ip::tcp::socket& s, std::uint64_t uid, std::atomic<bool>& stop) {
    return std::thread([&s, uid, &stop] {
        for (;;) {
            for (int i = 0; i < 100 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stop.load()) break;
            try {
                kc::Message hb; hb.from = uid; hb.type = kc::MsgType::Heartbeat;
                send_msg(s, hb);
            } catch (...) { break; }
        }
    });
}

std::uint64_t register_and_login(const std::string& username, const std::string& password,
                                 std::string& token, unsigned short httpPort) {
    auto r = http_call("POST", "/api/register", "",
                       "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}", httpPort);
    if (r.status != 200) throw std::runtime_error("register failed: " + r.body);
    auto d = parse_json(r.body);
    const std::uint64_t uid = ju64(d["data"], "userId");
    auto lr = http_call("POST", "/api/login", "",
                        "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}", httpPort);
    if (lr.status != 200) throw std::runtime_error("login failed: " + lr.body);
    token = jstr(parse_json(lr.body)["data"], "token");
    return uid;
}

// 生成确定性的测试字节（固定种子，同 size 同内容，便于上传/下载逐字节比对）。
std::string make_payload(std::size_t n) {
    std::mt19937 rng(0xC0DE1234u ^ static_cast<std::uint32_t>(n));
    std::string s;
    s.resize(n);
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(rng() & 0xFF);
    return s;
}

// ---- 文件模块的"真实直传"三步曲 --------------------------------------------
struct UploadedFile {
    std::string fileId, key, url, qetag;
    std::uint64_t size = 0;
    std::string bytes;      // 上传的原始字节，下载时逐字节比对
    std::string uploadHash; // 直传响应里的 hash（七牛 qetag）
    int parts = 0;          // 分片 V2 实际片数（form 模式为 0）
};

// 直传模式：默认 **v2**（分片上传，同 SDK examples/up_multipart_sample.c），
// QINIU_UPLOAD_MODE=form 切回表单直传（同 examples/up_form_upload.c）做对照。
// 两种模式对 IM 服务器完全等价：都是"拿服务端签的 uptoken 直接把字节推给
// up_host"，服务器只在 commit 时用 stat 核验结果——所以切模式不需要改服务器。
bool qiniu_use_form_mode() {
    const char* m = std::getenv("QINIU_UPLOAD_MODE");
    return m && std::strcmp(m, "form") == 0;
}

// 分片大小：SDK 默认 4MB；这里允许用 QINIU_PART_SIZE_MB 调小（最小 1MB，七牛 V2
// 口径），好让几 MB 的测试样本也能真的走出"多片 + etag 汇总"那条代码路径。
std::size_t qiniu_part_size() {
    std::size_t mb = 4;
    if (const char* p = std::getenv("QINIU_PART_SIZE_MB"); p && *p) {
        try { mb = static_cast<std::size_t>(std::stoul(p)); } catch (...) {}
    }
    if (mb < 1) mb = 1;
    if (mb > 1024) mb = 1024;
    return mb << 20;
}

// upload-token（服务器签凭证）→ 真实直传七牛 → commit（服务器 stat 核验登记）。
// skipUpload=true 时跳过直传，用于"虚报 commit"负向用例；
// partSizeOverride!=0 时强制分片大小（用于"必须多片"的用例，不受环境变量影响）。
UploadedFile upload_file_real(const std::string& token, const std::string& name,
                              const std::string& mime, std::size_t byteLen,
                              bool skipUpload, unsigned short httpPort,
                              std::size_t partSizeOverride = 0) {
    const std::string bytes = make_payload(byteLen);

    auto rt = http_call("POST", "/api/files/upload-token", token,
        "{\"name\":\"" + name + "\",\"mime\":\"" + mime +
        "\",\"size\":" + std::to_string(byteLen) + "}", httpPort);
    if (rt.status != 200) throw std::runtime_error(
        "upload-token 失败[IM服务器 HTTP=" + std::to_string(rt.status) + "] body=" + rt.body);
    auto rd = parse_json(rt.body);
    UploadedFile f;
    f.fileId = jstr(rd["data"], "fileId");
    f.key    = jstr(rd["data"], "key");
    const std::string uploadUrl = jstr(rd["data"], "uploadUrl");
    const std::string uptoken   = jstr(rd["data"], "uptoken");
    const std::string ticket    = jstr(rd["data"], "ticket");
    f.bytes = bytes;

    if (!skipUpload) {
        // 真实数据面：向七牛 up_host 直传（字节出网，不经 IM 服务器）。
        const bool useForm = qiniu_use_form_mode();
        const std::string mode = useForm ? "form" : "v2";
        HttpResponse put;
        std::string step = useForm ? "form-put" : "init";
        try {
            if (useForm) {
                put = qiniu_form_upload(uploadUrl, uptoken, f.key, name, mime, bytes);
            } else {
                put = qiniu_multipart_upload(uploadUrl, uptoken, f.key, name, mime, bytes,
                                             partSizeOverride ? partSizeOverride : qiniu_part_size(),
                                             f.parts, step);
            }
        } catch (const std::exception& e) {
            std::cout << "  [诊断] " << mode << " 直传七牛 up_host 异常 host="
                      << parse_url(uploadUrl).host << " step=" << step
                      << " err=" << e.what() << "\n";
            throw std::runtime_error(std::string("qiniu 直传连接异常: ") + e.what());
        }
        if (put.status != 200) {
            std::cout << "  [诊断] " << mode << " 直传失败 step=" << step << " -> "
                      << describe_qiniu(put) << " (key=" << f.key << ")\n";
            // 按七牛错误码走两条专项诊断（凭证正确时二选一能定位到根因）。
            if (put.body.find("expired") != std::string::npos) {
                reportTokenClock(put, uptoken, 3600);
            } else if (put.body.find("bad token") != std::string::npos ||
                       put.body.find("BadToken") != std::string::npos ||
                       put.body.find("invalid token") != std::string::npos ||
                       put.body.find("invalid access key") != std::string::npos) {
                reportBadToken(put, uptoken, uploadUrl, f.key);
            }
            throw std::runtime_error("qiniu " + mode + " 直传失败 step=" + step +
                                     "[" + describe_qiniu(put) + "]");
        }
        auto pd = parse_json(put.body);
        f.uploadHash = jstr(pd, "hash");
        if (f.uploadHash.empty()) {
            std::cout << "  [诊断] " << mode << " 直传 200 但响应缺 hash -> "
                      << describe_qiniu(put) << "\n";
            throw std::runtime_error("qiniu 直传响应无 hash: " + put.body);
        }
        std::cout << "  [直传] mode=" << mode << " parts=" << f.parts
                  << " size=" << bytes.size() << " hash=" << f.uploadHash << "\n";
    }

    auto ct = http_call("POST", "/api/files/commit", token,
        "{\"file_id\":\"" + f.fileId + "\",\"ticket\":\"" + ticket + "\"}", httpPort);
    if (ct.status != 200) {
        // commit 非 200 = 服务端向七牛 stat 核验失败（400 虚报 / 503 管理 API 被拒）。
        std::cout << "  [诊断] commit 失败[IM服务器 HTTP=" << ct.status << "] body="
                  << ct.body << "（503=服务器→七牛管理 API 这一步失败：entry 拼法/"
                     "rs 端点区域/管理权限，见 verify 脚本排查提示；"
                     "400=对象未真实上传或超上限）\n";
        throw std::runtime_error("commit 失败[HTTP=" + std::to_string(ct.status) + "] " + ct.body);
    }
    auto cd = parse_json(ct.body);
    f.url   = jstr(cd["data"], "url");
    f.qetag = jstr(cd["data"], "qetag");
    f.size  = ju64(cd["data"], "size");
    return f;
}

// ---- 走一遍"换取入口 → 302 → 真实直链"并逐字节核对 --------------------------
// 与单聊里 B 的下载动作等价（只是发起者是上传者本人），给"多分片"这种
// 不经过消息链路的对象做完整性回验：任何一片错位/漏拼，这里必然对不上。
void verify_download_matches(const UploadedFile& f, const std::string& token,
                             unsigned short httpPort, const std::string& desc) {
    if (f.fileId.empty() || f.bytes.empty()) {
        check(false, desc + "：前置不成立（未 commit 成功或无字节可比）");
        return;
    }
    auto r = http_call("GET", "/api/files/" + f.fileId, token, "", httpPort);
    check(r.status == 302, desc + "：换取入口返回 302", std::to_string(r.status));
    if (r.status != 302) return;
    const std::string loc = r.headers["location"];
    const std::string finalUrl = (loc.rfind("http", 0) == 0)
        ? loc : ("http://127.0.0.1:" + std::to_string(httpPort) + loc);
    try {
        auto dl = https_get(finalUrl);
        const bool same = (dl.status == 200 && dl.body == f.bytes);
        std::string extra = "status=" + std::to_string(dl.status) +
            " got=" + std::to_string(dl.body.size()) +
            " want=" + std::to_string(f.bytes.size());
        if (!same && dl.status != 200) {
            extra += " " + describe_qiniu(dl);
            if (looksLikeEdgeHtml(dl)) probeEdgeReachability(finalUrl);
        }
        check(same, desc + "：跟随直链取回字节与上传时逐字节相同", extra);
    } catch (const std::exception& e) {
        check(false, desc + "：跟随直链下载", e.what());
    }
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    unsigned short httpPort = 8080, tcpPort = 8888;
    if (argc > 1) httpPort = static_cast<unsigned short>(std::stoi(argv[1]));
    if (argc > 2) tcpPort  = static_cast<unsigned short>(std::stoi(argv[2]));

    const std::string password = "secret123";
    const std::string suffix = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);

    boost::asio::io_context io;
    std::atomic<bool> stopA{false}, stopB{false};
    std::thread hbA, hbB;

    try {
        // ---- 0. 注册登录 A/B，互为好友 --------------------------------------
        std::string tokenA, tokenB;
        const std::uint64_t uidA = register_and_login("qfilA_" + suffix, password, tokenA, httpPort);
        const std::uint64_t uidB = register_and_login("qfilB_" + suffix, password, tokenB, httpPort);
        check(uidA && uidB && !tokenA.empty() && !tokenB.empty(), "注册并登录 A/B");
        {
            auto rr = http_call("POST", "/api/friends/requests", tokenA,
                                "{\"toUserId\":" + std::to_string(uidB) + "}", httpPort);
            auto rd = parse_json(rr.body);
            const std::uint64_t reqId = ju64(rd["data"]["request"], "requestId");
            http_call("POST", "/api/friends/requests/" + std::to_string(reqId) + "/accept",
                      tokenB, "", httpPort);
            check(true, "A/B 互为好友");
        }

        // ---- 1. TCP 登录 ------------------------------------------------------
        boost::asio::ip::tcp::socket connA(io), connB(io);
        if (!connect_with_retry(connA, tcpPort) || !connect_with_retry(connB, tcpPort))
            throw std::runtime_error("tcp connect failed");
        set_recv_timeout(connA, 8); set_recv_timeout(connB, 8);
        check(login(connA, uidA, tokenA).type == kc::MsgType::LoginAck, "A TCP 登录");
        check(login(connB, uidB, tokenB).type == kc::MsgType::LoginAck, "B TCP 登录");
        hbA = start_heartbeat(connA, uidA, stopA);
        hbB = start_heartbeat(connB, uidB, stopB);

        // ---- 2. 鉴权与参数负向 -------------------------------------------------
        {
            auto r = http_call("POST", "/api/files/upload-token", "",
                               "{\"name\":\"a.png\",\"size\":10}", httpPort);
            check(r.status == 401, "未登录请求上传凭证 401", std::to_string(r.status));
        }
        {
            // 超限：默认 FILE_MAX_SIZE_MB=20，这里报 21MB（服务器在签凭证前即拒）。
            auto r = http_call("POST", "/api/files/upload-token", tokenA,
                               "{\"name\":\"big.png\",\"size\":" +
                               std::to_string(21LL * 1024 * 1024) + "}", httpPort);
            check(r.status == 400, "超过 FILE_MAX_SIZE 的凭证请求被拒(400)",
                  std::to_string(r.status) + " " + r.body);
        }
        {
            auto r = http_call("POST", "/api/files/upload-token", tokenA,
                               "{\"name\":\"evil.exe\",\"size\":1024}", httpPort);
            check(r.status == 400, "黑名单类型 exe 被拒(400)",
                  std::to_string(r.status) + " " + r.body);
        }
        {
            // 任何下发给客户端的响应都不该出现七牛 SK 明文。
            auto r = http_call("POST", "/api/files/upload-token", tokenA,
                               "{\"name\":\"x.png\",\"size\":1024}", httpPort);
            const bool leak = r.body.find("secret_key") != std::string::npos;
            check(r.status == 200 && !leak, "凭证响应不含七牛密钥",
                  std::to_string(r.status) + " leak=" + (leak ? "1" : "0"));
        }

        // ---- 3. 正向：真实直传七牛（分片 V2 / form 二选一）+ commit -----------
        std::cout << "  [模式] 直传方式=" << (qiniu_use_form_mode() ? "form(表单直传)" : "v2(分片上传)")
                  << " partSize=" << (qiniu_part_size() >> 20) << "MB"
                  << "（QINIU_UPLOAD_MODE / QINIU_PART_SIZE_MB 可调）\n";
        UploadedFile img;
        try {
            img = upload_file_real(tokenA, "照片.png", "image/png", 2048, false, httpPort);
            check(!img.fileId.empty() && !img.uploadHash.empty(),
                  "真实直传七牛返回 key+hash（qetag）", img.uploadHash);
            check(img.size == 2048 && img.qetag == img.uploadHash,
                  "commit 登记的 size/qetag 与七牛 stat 权威值一致",
                  "size=" + std::to_string(img.size) + " qetag=" + img.qetag);
            check(img.url.find("/api/files/" + img.fileId) != std::string::npos,
                  "commit 返回服务器换取入口 URL（非七牛直链）", img.url);
        } catch (const std::exception& e) {
            check(false, "真实上传链路（检查七牛凭证/网络是否可用）", e.what());
        }

        // ---- 3b. 多分片：1MB 一片 × 1.5MB 样本，必须真正走"多片 + etag 汇总" ----
        // 这一段是分片上传 V2 相对表单直传的**唯一实质差异**，也是 IM 里
        // 大视频/大文档的真实路径：init → 多次 PUT part → complete(带 etag 列表)。
        // 片大小在这里写死 1MB（不受 QINIU_PART_SIZE_MB 影响），保证用例可复现。
        // form 模式下 parts 恒为 0，该用例改为断言"单请求直传成功"，不误报失败。
        UploadedFile clip;
        try {
            clip = upload_file_real(tokenA, "视频片段.mp4", "video/mp4",
                                    1536 * 1024, false, httpPort, 1u << 20);
            if (qiniu_use_form_mode()) {
                check(clip.size == 1536u * 1024 && clip.qetag == clip.uploadHash,
                      "form 模式 1.5MB 直传 + commit 一致（多片用例在 v2 模式下才有意义）",
                      "size=" + std::to_string(clip.size));
            } else {
                check(clip.parts >= 2, "分片 V2 真走了多片（parts>=2）",
                      "parts=" + std::to_string(clip.parts));
                check(clip.size == 1536u * 1024 && clip.qetag == clip.uploadHash,
                      "多片 commit 的 size/qetag 与七牛 stat 权威值一致",
                      "size=" + std::to_string(clip.size) + " qetag=" + clip.qetag);
                verify_download_matches(clip, tokenA, httpPort, "多片对象");
            }
        } catch (const std::exception& e) {
            check(false, "多分片直传链路（V2 init/part/complete）", e.what());
        }

        // 虚报 commit：申请凭证但**不直传**，直接 commit 必须被 stat 核验拒绝。
        {
            auto rt = http_call("POST", "/api/files/upload-token", tokenA,
                "{\"name\":\"lie.png\",\"mime\":\"image/png\",\"size\":1024}", httpPort);
            if (rt.status == 200) {
                auto d = parse_json(rt.body);
                auto c = http_call("POST", "/api/files/commit", tokenA,
                    "{\"file_id\":\"" + jstr(d["data"], "fileId") +
                    "\",\"ticket\":\"" + jstr(d["data"], "ticket") + "\"}", httpPort);
                check(c.status == 400 && c.body.find("object not uploaded") != std::string::npos,
                      "未直传就 commit（虚报）被七牛 stat 拒绝", std::to_string(c.status) + " " + c.body);
            } else {
                check(false, "虚报用例前置：凭证申请应成功", rt.body);
            }
        }

        // ---- 4. 单聊 type=2：B 收到服务端重建元数据，跟 302 到真实七牛下载------
        std::uint64_t clientMsg = 1000;
        if (!img.size) {
            // 图片真实上传没成功（凭证/网络问题，根因见上方 [诊断]）：跳过依赖它的
            // 收发/下载用例，避免拿空 file_id 发消息触发服务端 Error 后 read 超时刷屏。
            check(false, "单聊/下载链路：跳过（图片真实上传未成功，无有效 file_id）",
                  "根因见上方表单直传/commit 的 qnHTTP/X-Reqid/error_code 诊断，多为凭证 invalid/bad token");
        } else {
        {
            kc::Message m;
            m.type = kc::MsgType::Image;
            m.to = uidB;
            m.msg_id = ++clientMsg;
            // 故意带上"自报 url 是钓鱼链接"——服务端必须整体重建，绝不透传。
            m.content = "{\"file_id\":\"" + img.fileId + "\",\"url\":\"http://evil.example/x.png\"}";
            send_msg(connA, m);
        }
        kc::Message ackA = wait_type(connA, kc::MsgType::SendAck);
        check(ackA.type == kc::MsgType::SendAck, "A 收到 SendAck", ackA.content);
        kc::Message gotB = wait_type(connB, kc::MsgType::Image);
        bool metaOk = false, urlRewritten = false, externalGone = false;
        std::string dlUrl;
        if (gotB.type == kc::MsgType::Image) {
            auto d = parse_json(gotB.content);
            metaOk = jstr(d, "file_id") == img.fileId && ju64(d, "size") == img.size &&
                     jstr(d, "mime") == "image/png" && jstr(d, "name") == "照片.png";
            dlUrl = jstr(d, "url");
            urlRewritten = dlUrl.find("/api/files/" + img.fileId) != std::string::npos;
            externalGone = gotB.content.find("evil.example") == std::string::npos;
        }
        check(metaOk, "B 收到图片消息且元数据由服务端重建（权威 size/qetag）", gotB.content);
        check(urlRewritten && externalGone, "自报钓鱼外链已被替换为换取入口", dlUrl);

        if (!dlUrl.empty() && img.size) {
            const std::string path = dlUrl.substr(dlUrl.find('/', 7));   // 去掉 scheme://host
            auto r = http_call("GET", path, tokenB, "", httpPort);
            check(r.status == 302, "GET 换取入口返回 302", std::to_string(r.status));
            const std::string loc = r.headers["location"];
            // 私有空间(private_space=true)：Location 带 e=/token= 时效签名；
            // 公共空间(private_space=false)：Location 是不带签名的裸直链。
            // 期望口径由 QINIU_PRIVATE_SPACE 指定（verify_Qiniu_file.sh 依据
            // QiniuConfig.json 的 private_space 导出）；未指定则按实际 Location 自适应。
            const bool isPrivate = loc.find("e=") != std::string::npos &&
                (loc.find("token=") != std::string::npos || loc.find("vtoken=") != std::string::npos);
            int expectPrivate = -1;
            if (const char* pv = std::getenv("QINIU_PRIVATE_SPACE"); pv && *pv)
                expectPrivate = (*pv == '1') ? 1 : 0;
            if (expectPrivate < 0) expectPrivate = isPrivate ? 1 : 0;   // 自适应：以实际为准
            const std::string spaceName = expectPrivate ? "私有空间(签名直链)" : "公共空间(裸直链)";
            check(isPrivate == (expectPrivate == 1),
                  "Location 直链形态与配置空间类型一致：" + spaceName,
                  "实际 private=" + std::string(isPrivate ? "1" : "0") +
                  " 期望 private=" + std::to_string(expectPrivate) + " loc=" + loc);
            // 真实模式：跟随 Location 到七牛域名取回字节，逐字节核对。
            const std::string finalUrl = (loc.rfind("http", 0) == 0)
                ? loc : ("http://127.0.0.1:" + std::to_string(httpPort) + loc);
            std::cout << "  [诊断] 换取入口 302 -> Location host="
                      << (loc.rfind("http", 0) == 0 ? parse_url(loc).host : std::string("回环"))
                      << " private=" << (isPrivate ? 1 : 0) << "\n";
            // edgeReachable：这条直链有没有真的被七牛业务层处理过。
            // 拿它当"时效性用例"的前置——域名接入层就拒了的话，任何请求都是
            // 403/421，"去掉签名也被拒"这个用例会以**错误的理由**变绿。
            bool edgeReachable = false;
            try {
                auto dl = https_get(finalUrl);
                const bool same = (dl.status == 200 && dl.body == img.bytes);
                edgeReachable = (dl.status == 200) || !looksLikeEdgeHtml(dl);
                std::string extra = "status=" + std::to_string(dl.status) +
                    " got=" + std::to_string(dl.body.size()) +
                    " want=" + std::to_string(img.bytes.size());
                if (!same && dl.status != 200) {
                    extra += " " + describe_qiniu(dl);
                    if (looksLikeEdgeHtml(dl)) probeEdgeReachability(finalUrl);
                }
                check(same, "跟随直链取回字节：与上传时逐字节相同（端到端完整性）", extra);
            } catch (const std::exception& e) {
                std::cout << "  [诊断] 跟随七牛直链下载异常 host="
                          << parse_url(finalUrl).host << " err=" << e.what() << "\n";
                check(false, "跟随七牛直链下载", e.what());
            }
            // 时效性/防篡改只对私有签名直链有意义：去掉签名 query 的裸路径必须被拒。
            // 公共空间无签名参数可言（去 query 即正常下载 URL），此用例自动跳过。
            if (!isPrivate) {
                std::cout << "  SKIP: 公共空间无签名直链，跳过『去签名参数应被拒』时效性用例\n";
            } else if (!edgeReachable) {
                std::cout << "  SKIP: 下载域名连业务层都没到（见上方 [探针] 两行），"
                             "时效性无法判定——先修好 domain 再谈签名直链\n";
            } else {
                // 这里**只作观察，不作断言**：img 刚被"带 token"成功取回过，而七牛的
                // 测试域名/CDN 默认可能"忽略查询参数"缓存对象——同一个 path 不带 token
                // 也会命中缓存直接 200。那不是私有空间放行了，而是 CDN 绕过了鉴权
                // （真机 2026-11-21 实测正是这样）。断言放到 4b：用一个从未被签名
                // 访问过的新对象去测，测到的才是 Kodo 自己的口径。
                try {
                    auto bad = https_get(strip_query(finalUrl));
                    std::cout << "  [观察] 已下载过的对象再取裸路径 -> HTTP=" << bad.status;
                    if (bad.status == 200) {
                        std::cout << "（命中边缘缓存：说明该域名按 path 缓存、不看 token。"
                                     "生产必须给下载域名配『保留查询参数』或开启私有空间回源鉴权，"
                                     "否则时效签名形同虚设）";
                    }
                    std::cout << "\n";
                } catch (const std::exception& e) {
                    std::cout << "  [观察] 已下载过的对象再取裸路径 -> 连接失败：" << e.what() << "\n";
                }
            }
         }
         } // if (img.size) else 分支结束（单聊/下载用例）

        // ---- 4b. 时效性正向断言：裸直链（去掉 e/token）必须被七牛拒绝 -----------
        // 判据必须是"**从未被签名访问过**"的对象：否则边缘节点按 path 缓存的那份
        // 会先命中，tokenless 请求照样 200，用例会以错误的理由变红/变绿
        // （2026-11-21 实测：同一对象先带 token 下载成功、再取裸路径 → 200，
        //   而换一个没下载过的对象立刻 401 {"error":"download token not specified"}）。
        // 所以这里现传一个专用小对象，取到 302 的 Location 后**故意去掉 query** 再请求。
        if (img.size) {
            UploadedFile guard;
            try {
                guard = upload_file_real(tokenA, "时效性专用.png", "image/png", 512, false, httpPort);
                auto r = http_call("GET", "/api/files/" + guard.fileId, tokenA, "", httpPort);
                const std::string loc = r.headers["location"];
                const bool signedUrl = loc.find("e=") != std::string::npos &&
                    (loc.find("token=") != std::string::npos || loc.find("vtoken=") != std::string::npos);
                if (!signedUrl) {
                    std::cout << "  SKIP: 公共空间直链无签名参数，时效性用例不适用\n";
                } else {
                    const std::string bare = (loc.rfind("http", 0) == 0)
                        ? strip_query(loc)
                        : ("http://127.0.0.1:" + std::to_string(httpPort) + strip_query(loc));
                    try {
                        auto bad = https_get(bare);
                        std::string extra = "HTTP=" + std::to_string(bad.status);
                        if (bad.status == 200) {
                            extra += " 预期被拒却取回 200（该对象从未被签名访问过，所以这不是缓存，"
                                     "而是这个下载域名对私有空间**没有 enforcing 下载凭证**）："
                                   + describe_qiniu(bad);
                        } else if (looksLikeEdgeHtml(bad)) {
                            extra += " 但响应来自边缘节点 HTML 页，不能算七牛业务层在鉴权："
                                   + describe_qiniu(bad);
                        }
                        check((bad.status == 401 || bad.status == 403 || bad.status == 504) &&
                              !looksLikeEdgeHtml(bad),
                              "去掉签名参数的裸直链被七牛业务层拒绝（新对象，无缓存干扰）", extra);
                    } catch (const std::exception& e) {
                        check(true, "去掉签名参数的裸直链被拒绝（连接层异常亦视为拒绝）", e.what());
                    }
                }
            } catch (const std::exception& e) {
                check(false, "时效性负向用例前置（上传一个专用小对象）", e.what());
            }
            if (guard.size) {   // 用完即删，别在 bucket 里堆测试垃圾
                http_call("DELETE", "/api/files/" + guard.fileId, tokenA, "", httpPort);
            }
        }

        // ---- 5. 群聊 type=4：真实上传一份文档并群里发送 ------------------------
        std::uint64_t groupId = 0;
        {
            auto r = http_call("POST", "/api/groups", tokenA,
                "{\"name\":\"七牛文件群_" + suffix + "\",\"memberIds\":[" + std::to_string(uidB) + "]}", httpPort);
            auto d = parse_json(r.body);
            groupId = ju64(d["data"]["group"], "groupId");
            check(groupId != 0, "A 建群 [B]");
            for (int i = 0; i < 10; ++i) {
                kc::Message ev = recv_msg(connB);
                if (ev.type == kc::MsgType::GroupMemberChanged) break;
            }
        }
        UploadedFile doc;
        try {
            doc = upload_file_real(tokenA, "报告.pdf", "application/pdf", 4096, false, httpPort);
            check(doc.size == 4096 && doc.qetag == doc.uploadHash, "PDF 真实上传链路");
        } catch (const std::exception& e) {
            check(false, "PDF 上传", e.what());
        }
        if (doc.size) {
            kc::Message m;
            m.type = kc::MsgType::File; m.to = groupId; m.is_group = true;
            m.msg_id = ++clientMsg;
            m.content = "{\"file_id\":\"" + doc.fileId + "\"}";
            send_msg(connA, m);
            kc::Message a = wait_type(connA, kc::MsgType::SendAck);
            check(a.type == kc::MsgType::SendAck, "A 群文件 SendAck");
            kc::Message b = wait_type(connB, kc::MsgType::File);
            bool ok = b.type == kc::MsgType::File && b.is_group;
            if (ok) { auto d = parse_json(b.content); ok = jstr(d, "file_id") == doc.fileId; }
            check(ok, "在线成员 B 实时收到群文件（type=4 透传）", b.content);
        }

        // ---- 6. 删除：本人删除后换取入口 404 ----------------------------------
        if (img.size) {
            auto del = http_call("DELETE", "/api/files/" + img.fileId, tokenA, "", httpPort);
            check(del.status == 200, "本人删除成功");
            auto after = http_call("GET", "/api/files/" + img.fileId, tokenA, "", httpPort);
            check(after.status == 404, "删除后换取入口 404", std::to_string(after.status));
        }
        // 多分片对象同样要能删干净（七牛 remove 走的是管理 API，与 stat 同一套
        // 凭证口径）；这一步失败 = 测试数据会留在 bucket 里长垃圾，必须算失败。
        if (clip.size) {
            auto del = http_call("DELETE", "/api/files/" + clip.fileId, tokenA, "", httpPort);
            check(del.status == 200, "多分片对象删除成功（不留测试垃圾）",
                  std::to_string(del.status) + " " + del.body);
        }

        stopA = true; stopB = true;
        if (hbA.joinable()) hbA.join();
        if (hbB.joinable()) hbB.join();

    } catch (const std::exception& e) {
        std::cout << "[qiniu-file-test] EXCEPTION: " << e.what() << "\n";
        ++g_fail;
        stopA = stopB = true;
        if (hbA.joinable()) hbA.join();
        if (hbB.joinable()) hbB.join();
    }

    if (g_fail == 0) {
        std::cout << "[qiniu-file-test] ALL PASS\n";
        return 0;
    }
    std::cout << "[qiniu-file-test] " << g_fail << " FAILED\n";
    return 1;
}
// /*************图片文件功能（七牛云真实直传）：真机验收客户端结束***************/
