#include "object_store.h"

#include "qiniu_signer.h"

#include <boost/asio/post.hpp>            // net::post：回调统一投回主 io_context
#include <boost/asio/ssl.hpp>             // boost::asio::ssl：管理 API 的 HTTPS 调用
#include <boost/beast/core.hpp>           // Beast：同步 HTTP 客户端（在独立线程里跑）
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>            // beast::ssl_stream：HTTPS 的 TLS 层
#include <boost/beast/version.hpp>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <chrono>
#include <cstdlib>            // std::getenv：QINIU_CONFIG 路径覆盖
#include <ctime>              // gmtime_s/gmtime_r + strftime：X-Qiniu-Date 时间戳头
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>              // std::tuple / std::tie：doRequest 的多返回值

// ============================================================================
// /*************图片文件功能（七牛云直传）：存储抽象实现***************/
// ============================================================================

namespace kc {

namespace {

// Mock 模式的一组"假凭证"。注意：这些不是任何真实账号的密钥，
// 它们的价值是把真实模式要走的**签名代码路径**全部跑一遍——
// 切到 QINIU_ENABLED=1 时，换的只是密钥和域名，算法路径不变。
const char* kMockAk = "MOCKAK";
const char* kMockSk = "MOCKSK-DEV-ONLY";
const char* kMockBucket = "mock-bucket";
// 票据密钥（mock）：泄漏后果上限 = 伪造一次上传凭证申请（还要过登录 JWT），
// 拿不到七牛 SK。真实模式请用 ticket_secret/secret_key。
const char* kMockTicketSecret = "mock-dev-ticket-secret-DO-NOT-USE-IN-PRODUCTION";

// mock 模拟字节的单对象上限：验收只用几百字节~几 KB 的样本，
// 64KB 足够演示"跟随后取回字节"且不撑爆 2GB 内存。
constexpr long long kMockServeBytesCap = 64 * 1024;

// 简易 URL 解码（mock-object 路由把 path 段还原成 object key）。
std::string urlDecode(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) { out.push_back(static_cast<char>(hi * 16 + lo)); i += 2; continue; }
        }
        out.push_back(in[i]);
    }
    return out;
}

// mock 对象的确定性校验和：32 位十六进制，只由 key 决定（FNV-1a）。
// 真实七牛的 qetag 算法不同——这里只求"同 key 恒同值、可核对"的模拟语义。
// ⚠ 排障史（本轮服务器"put 后随机静默死亡"的真凶就在这几行）：
//   32 个 hex 字符 = 4 轮 64 位哈希（每轮 16 hex），循环上界必须是 4。
//   上一版写成 i<8：第二轮起索引 i*8+(7-nib) 达到 32..63，
//   **对 std::string(32) 的堆缓冲区越界写 32 字节**——把 malloc 邻块踩烂，
//   进程在"之后任意一次堆操作"随机死亡（Release 无迭代器检查，现场更远：
//   服务器表现为 mock-put 响应成功后的静默猝死，rc=127、无 WER、无 fatal，
//   最终由 Debug checked-heap + test_file_loopback 进程内复现钉死）。
std::string mockQetag(const std::string& key) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ULL; }
    static const char* hex = "0123456789abcdef";
    std::string s(32, '0');                       // 32 hex = 4 轮 × 8 字节
    for (int i = 0; i < 4; ++i) {                 // ← 曾经误写成 8（越界写！）
        for (int nib = 7; nib >= 0; --nib) {
            s[i * 8 + (7 - nib)] = hex[(h >> (nib * 4)) & 0xF];
        }
        h = h * 6364136223846793005ULL + 1442695040888963407ULL;   // 混匀出下一轮
    }
    return s;
}

} // namespace

// ============================================================================
// QiniuConfig：加载 QiniuConfig.json（风格与 jwt_config.cpp 完全一致）。
// ============================================================================
QiniuConfig QiniuConfig::load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open qiniu config: " + path);

    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError()) throw std::runtime_error("qiniu config is not valid JSON");
    if (!doc.IsObject())     throw std::runtime_error("qiniu config root must be object");

    QiniuConfig cfg;
    auto getStr = [&](const char* key, std::string& dst, bool required) {
        if (!doc.HasMember(key) || !doc[key].IsString()) {
            if (required) throw std::runtime_error(std::string("qiniu config missing ") + key);
            return;
        }
        dst = doc[key].GetString();
    };
    getStr("access_key",   cfg.access_key,   true);
    getStr("secret_key",   cfg.secret_key,   true);
    getStr("ticket_secret",cfg.ticket_secret,false);
    getStr("bucket",       cfg.bucket,       true);
    getStr("up_host",      cfg.up_host,      true);
    getStr("domain",       cfg.domain,       true);
    getStr("rs_scheme",    cfg.rs_scheme,    false);
    getStr("rs_host",      cfg.rs_host,      false);
    getStr("ca_bundle",    cfg.ca_bundle,    false);

    if (doc.HasMember("zone") && doc["zone"].IsInt()) cfg.zone = doc["zone"].GetInt();
    if (doc.HasMember("private_space") && doc["private_space"].IsBool())
        cfg.private_space = doc["private_space"].GetBool();

    // 结尾 '/' 归一化（domain 拼接时避免出现 "//key"——CDN 对双斜杠路径
    // 经常 404，这类事故的排查成本远高于这两行归一化）。
    while (!cfg.domain.empty() && cfg.domain.back() == '/') cfg.domain.pop_back();
    while (!cfg.up_host.empty() && cfg.up_host.back() == '/') cfg.up_host.pop_back();
    if (cfg.ticket_secret.empty()) cfg.ticket_secret = cfg.secret_key;

    // 这里**绝不打印** access_key/secret_key（保密纪律与 DbConfig 相同）；
    // 只打非敏感字段帮助运维确认"连的是哪个空间"。
    std::cout << "[qiniu] bucket=" << cfg.bucket
              << ", zone=" << cfg.zone
              << ", private=" << (cfg.private_space ? 1 : 0)
              << ", rs=" << cfg.rs_scheme << "://" << cfg.rs_host
              << std::endl;
    return cfg;
}

// ============================================================================
// ObjectStorage：无状态票据 + 下载入口签名（两种实现共用）
// ============================================================================
std::string ObjectStorage::makeTicket(const std::string& uuid, std::uint64_t uploader,
                                      const std::string& key, long long declaredSize,
                                      const std::string& name, const std::string& mime,
                                      long long ttlSec) const {
    // payload 用 '|' 分隔；uuid/key/name/mime 都可能含任意字符（name 是用户
    // 文件名），统一先 urlsafe_base64 再拼接——分隔符永远只有结构里的 '|'，
    // 不存在"文件名里带竖线把字段撑歪"的解析事故。
    std::string payload = "v2|" + uuid + "|" + std::to_string(uploader) +
                          "|" + qsign::urlSafeBase64Encode(key) +
                          "|" + std::to_string(declaredSize) +
                          "|" + std::to_string(nowUnix() + ttlSec) +
                          "|" + qsign::urlSafeBase64Encode(name) +
                          "|" + qsign::urlSafeBase64Encode(mime);
    const std::string sig = qsign::urlSafeBase64Encode(
        qsign::hmacSha256(ticketSecret_, payload));
    return qsign::urlSafeBase64Encode(payload) + "." + sig;
}

bool ObjectStorage::verifyTicket(const std::string& ticket, const std::string& uuid,
                                 std::uint64_t uploader, std::string& keyOut,
                                 long long& sizeOut, std::string& nameOut,
                                 std::string& mimeOut) const {
    const std::size_t dot = ticket.find('.');
    if (dot == std::string::npos || dot + 1 >= ticket.size()) return false;

    std::string payload;
    if (!qsign::urlSafeBase64Decode(ticket.substr(0, dot), payload)) return false;
    const std::string expectSig = qsign::urlSafeBase64Encode(
        qsign::hmacSha256(ticketSecret_, payload));
    if (ticket.substr(dot + 1) != expectSig) return false;   // 验签失败=票据被改

    // payload: v2|uuid|uploader|b64(key)|size|expire|b64(name)|b64(mime)
    std::vector<std::string> parts;
    {
        std::string cur;
        std::istringstream is(payload);
        while (std::getline(is, cur, '|')) parts.push_back(cur);
    }
    if (parts.size() != 8 || parts[0] != "v2") return false;
    if (parts[1] != uuid) return false;
    if (parts[2] != std::to_string(uploader)) return false;
    long long expire = 0;
    try { expire = std::stoll(parts[5]); sizeOut = std::stoll(parts[4]); }
    catch (...) { return false; }
    if (expire < nowUnix()) return false;                    // 过期票据拒绝
    if (!qsign::urlSafeBase64Decode(parts[3], keyOut))   return false;
    if (!qsign::urlSafeBase64Decode(parts[6], nameOut))  return false;
    if (!qsign::urlSafeBase64Decode(parts[7], mimeOut))  return false;
    return true;
}

std::string ObjectStorage::makeEntryQuery(const std::string& path, long long ttlSec) const {
    // 与七牛私有下载签名同构，但密钥是 ticketSecret_（分域：入口密钥泄露
    // 不影响 bucket 管理权限——最小权限）。
    const long long e = nowUnix() + ttlSec;
    const std::string data = path + "?e=" + std::to_string(e);
    const std::string sign = qsign::urlSafeBase64Encode(qsign::hmacSha1(ticketSecret_, data));
    return "?e=" + std::to_string(e) + "&vtoken=" + sign;
}

bool ObjectStorage::verifyEntryQuery(const std::string& path, const std::string& eStr,
                                     const std::string& vtoken) const {
    long long e = 0;
    try { e = std::stoll(eStr); } catch (...) { return false; }
    if (e < nowUnix()) return false;                         // 过期
    const std::string data = path + "?e=" + eStr;            // 用调用方原样的 e 字符串，避免重序列化差异
    const std::string expect = qsign::urlSafeBase64Encode(qsign::hmacSha1(ticketSecret_, data));
    return expect == vtoken;
}

// ============================================================================
// MockObjectStore
// ============================================================================
namespace {

// Mock 也要遵守"回调不在调用线程同步触发"的契约：统一 post 回主 io_context，
// 这样调用方代码（http 层/session 层）对两种实现完全一致，切真实七牛时
// 不会因为回调时机变化炸出竞态。
boost::asio::io_context* g_mockIo = nullptr;

} // namespace

MockObjectStore::MockObjectStore(boost::asio::io_context& mainIo)
    : ObjectStorage(kMockTicketSecret) {
    g_mockIo = &mainIo;   // 见下：进程内单例式的 mock，main.cpp 只创建一次
    std::cerr << "[file] QINIU_ENABLED=0：文件存储运行在 MockObjectStore 模式"
                 "（对象只登记元数据、不落盘、不走外网）。生产必须配置 QiniuConfig.json 并置 1。"
              << std::endl;
}

std::string MockObjectStore::signUploadToken(const std::string& key, long long fsizeLimit,
                                             long long expiresUnix) {
    // 与真实模式同一份 policy 生成逻辑（scope 锁 key、fsizeLimit 限大小），
    // 只是 AK/SK 是假值——保证"凭证格式"这条代码路径始终被覆盖。
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("scope");      w.String((std::string(kMockBucket) + ":" + key).c_str());
    // ⚠ 字段名必须是 **deadline**（七牛官方上传策略口径，绝对 Unix 秒）。
    // 写成 "expires" 会被七牛直接忽略 → 截止时间按 0 处理 → 恒定 401
    // {"error":"expired token"}，而签名本身是**对的**（2026-11-20 实测：
    // 同一对 AK/SK、同一 up_host，expires 写法 401、deadline 写法 200）。
    w.Key("deadline");   w.Int64(expiresUnix);
    w.Key("fsizeLimit"); w.Int64(fsizeLimit);
    w.Key("insertOnly"); w.Int(1);
    w.EndObject();
    const std::string policy(sb.GetString(), sb.GetSize());
    return qsign::makeUploadToken(kMockAk, kMockSk, policy);
}

std::string MockObjectStore::uploadUrlForClient() const {
    // mock 的"直传目标"就是本服务器的模拟端点（http_server 里仅 mock 模式开放）。
    return "/api/files/mock-put";
}

std::string MockObjectStore::makeDownloadUrl(const FileRecord& rec, long long ttlSec) {
    const std::string path = "/api/files/mock-object/" + qsign::urlEncode(rec.object_key, true);
    const long long e = nowUnix() + ttlSec;
    return path + "?e=" + std::to_string(e) +
           "&token=" + qsign::makeDownloadToken(kMockAk, kMockSk, path, e);
}

void MockObjectStore::mockPut(const std::string& key, long long size) {
    fileCrumb("mock.put");
    std::lock_guard<std::mutex> lk(mu_);
    objects_[key] = Obj{size, mockQetag(key)};
}

bool MockObjectStore::mockGet(const std::string& key, long long& size, std::string& qetag) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = objects_.find(key);
    if (it == objects_.end()) return false;
    size = it->second.size;
    qetag = it->second.qetag;
    return true;
}

bool MockObjectStore::verifyObjectToken(const std::string& pathWithE,
                                        const std::string& tokenParam) const {
    // 与真实七牛私有下载同一套算法/同一实现函数，只是密钥换成 mock 假 SK。
    // 覆盖的是"token 参数格式、e 校验、URL 规范化"这些写法层面的东西。
    // ⚠ 但它**盖不住"待签串的口径"**：mock 自己签自己验，relative path 也自洽；
    //   真实七牛要求签整条绝对 URL。2026-11-21 就是这个差异让 mock 全绿、真机
    //   401 download token auth failed。凡涉及"与外部系统对齐口径"的规矩，
    //   只有真机能定案——所以 verify_Qiniu_file.sh 不能被 mock 验收替代。
    const std::size_t colon = tokenParam.find(':');
    if (colon == std::string::npos) return false;
    const std::string expect = qsign::urlSafeBase64Encode(qsign::hmacSha1(kMockSk, pathWithE));
    return tokenParam.substr(colon + 1) == expect;
}

void MockObjectStore::stat(const std::string& key, StatCb cb) {
    fileCrumb("mock.stat");
    if (down_.load()) {
        // 断连模拟（验收 6）：传输层失败，上层映射 503。
        boost::asio::post(*g_mockIo, [cb] {
            cb(false, "mock storage unreachable (simulate down)", false, 0, {});
        });
        return;
    }
    Obj obj;
    bool found;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = objects_.find(key);
        found = it != objects_.end();
        if (found) obj = it->second;
    }
    boost::asio::post(*g_mockIo, [cb, found, obj] {
        cb(true, {}, found, obj.size, obj.qetag);
    });
}

void MockObjectStore::remove(const std::string& key, OpCb cb) {
    if (down_.load()) {
        boost::asio::post(*g_mockIo, [cb] {
            cb(false, "mock storage unreachable (simulate down)");
        });
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        objects_.erase(key);
    }
    boost::asio::post(*g_mockIo, [cb] { cb(true, {}); });
}

// ============================================================================
// QiniuHttpAgent：管理 API（stat/rm）的外网调用执行器
// ----------------------------------------------------------------------------
// 为什么不复用 Db 的 worker：sqlQueue_ 的任务都要借 DB 连接（池上限 = worker 数）。
// 七牛调用最长阻塞 3s×2 次重试，混进 SQL 池等于"七牛抖一下，SQL 吞吐塌一半"。
// 独立小池（1 线程足够：stat/rm 都是毫秒级控制面调用，并发上限由 HTTP 入口闸门
// 兜住）+ 队列上限 64，满了立即回 busy——过载语义与 P0-4 一致。
//
// TLS 说明（如实）：https 走 boost::asio::ssl。OpenSSL 在 Windows 上
// set_default_verify_paths() **不读系统证书库**，因此提供 ca_bundle 配置：
// 指定 CA 文件（如 cacert.pem）做校验；未指定时跳过证书校验并在日志限频警告
// ——这是"可用性优先、风险如实记录"的取舍，生产强烈建议配 ca_bundle。
// 本模块在 QINIU_ENABLED=1 时才会被真正调用（本轮交付默认 mock，未做真实联调）。
// ============================================================================
// 注意：本类定义在 namespace kc 内（与 object_store.h 里的前向声明
// `std::unique_ptr<class QiniuHttpAgent>` 所指一致），刻意**不放**匿名命名空间——
// 否则同一 TU 里会出现两个同名类型（匿名 ns 的那个永远用不到、前向声明的
// 那个永远不完整），MSVC 报"不明确的符号/使用未定义类型"。
class QiniuHttpAgent {
public:
    // reqid = 七牛响应头 X-Reqid：任何非 200 都要把它带进日志，
    // 否则"401 到底是七牛给的、还是链路上某个网关给的"永远只能靠猜
    // （2026-11-21 commit 卡 stat 就吃了这个亏：只看到 qiniu stat http=401）。
    using RespCb = std::function<void(bool transportOk, const std::string& err,
                                      int status, const std::string& body,
                                      const std::string& reqid)>;

    QiniuHttpAgent(boost::asio::io_context& mainIo, std::string scheme,
                   std::string host, std::string caBundle)
        : mainIo_(mainIo), scheme_(std::move(scheme)), host_(std::move(host)),
          caBundle_(std::move(caBundle)) {
        if (scheme_ == "https") {
            // 整个 agent 共用一个 ssl::context：TLS 上下文构造不便宜。
            ctx_ = boost::asio::ssl::context(boost::asio::ssl::context::tlsv12_client);
            if (!caBundle_.empty()) {
                ctx_.load_verify_file(caBundle_);             // 指定 CA 文件时正常校验
                ctx_.set_verify_mode(boost::asio::ssl::verify_peer);
                verifyCerts_ = true;
            } else {
                ctx_.set_verify_mode(boost::asio::ssl::verify_none);
                std::cerr << "[qiniu] WARNING: ca_bundle not set, TLS peer verification "
                             "DISABLED for management API calls (set ca_bundle in QiniuConfig.json)"
                          << std::endl;
            }
        }
        threads_.emplace_back([this] { workerLoop(); });
    }

    ~QiniuHttpAgent() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (threads_[0].joinable()) threads_[0].join();
    }

    // 队列满直接失败（快速失败优于无限排队，语义同 DB_QUEUE_LIMIT）。
    void request(std::string method, std::string path,
                 std::vector<std::pair<std::string, std::string>> headers,
                 RespCb cb, long long timeoutSec = 3) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopping_ || jobs_.size() >= 64) {
                boost::asio::post(mainIo_, [cb] {
                    cb(false, "qiniu agent busy", 0, {}, {});
                });
                return;
            }
            jobs_.push_back(Job{std::move(method), std::move(path), std::move(headers),
                                std::move(cb), timeoutSec});
        }
        cv_.notify_one();
    }

private:
    struct Job {
        std::string method, path;
        std::vector<std::pair<std::string, std::string>> headers;
        RespCb cb;
        long long timeoutSec;
    };

    void workerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stopping_ || !jobs_.empty(); });
                if (stopping_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            // 3s 超时 + 1 次重试（规划口径）。只对传输层错误重试；
            // 4xx/5xx 响应本身**不算**传输失败（612 等业务码由上层解释）。
            bool transportOk = false;
            std::string err, body, reqid;
            int status = 0;
            for (int attempt = 0; attempt < 2 && !transportOk; ++attempt) {
                if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
                std::tie(transportOk, err, status, body, reqid) = doRequest(job);
            }
            const bool ok = transportOk;
            boost::asio::post(mainIo_, [cb = std::move(job.cb), ok, err, status, body, reqid] {
                cb(ok, err, status, body, reqid);
            });
        }
    }

    std::tuple<bool, std::string, int, std::string, std::string> doRequest(const Job& job) {
        namespace beast = boost::beast;
        namespace http  = beast::http;
        namespace net   = boost::asio;
        namespace ssl   = boost::asio::ssl;
        using tcp = net::ip::tcp;

        // 每次调用一个临时 io_context：本函数只在 agent 自有线程里跑（同步），
        // 同步写法的可读性远高于把一次三秒超时摊成一条异步链，
        // 而"绝不阻塞 io 线程"的纪律靠线程归属来保证，与同步/异步无关。
        net::io_context io;
        tcp::resolver resolver(io);
        const std::string port = (scheme_ == "https") ? "443" : "80";

        http::request<http::string_body> req;
        req.method(job.method == "POST" ? http::verb::post : http::verb::get);
        req.target(job.path);
        req.version(11);
        req.set(http::field::host, host_);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::connection, "close");
        for (const auto& h : job.headers) req.set(h.first, h.second);
        req.prepare_payload();

        http::response<http::string_body> res;
        beast::flat_buffer buffer;

        try {
            if (scheme_ == "https") {
                // Boost 1.83 起 beast::ssl_stream 已退役；官方替代就是
                // asio::ssl::stream<beast::tcp_stream>：tcp_stream 提供
                // expires_after 超时，ssl::stream 包在外面做 TLS。
                ssl::stream<beast::tcp_stream> s(io, ctx_);
                beast::get_lowest_layer(s).expires_after(std::chrono::seconds(job.timeoutSec));
                beast::get_lowest_layer(s).connect(resolver.resolve(host_, port));   // 先连 TCP
                // SNI **无条件**发送（旧代码只在 verifyCerts_ 时才设）：
                // rs.qbox.me 这类端点与一堆租户域名共用接入 IP，ClientHello 里
                // 没有 server_name，接入层只能按默认证书/默认站点受理——那时你
                // 收到的 401 根本不是七牛给的，而是链路上某个默认 vhost 给的，
                // 而签名/entry/权限会被一路误查。带 SNI 不需要开证书校验，
                // 它只是把"我要访问哪个主机名"说清楚（本来就在 URL 里）。
                if (!SSL_set_tlsext_host_name(s.native_handle(), host_.c_str())) {
                    return {false, "SNI setup failed", 0, {}, {}};
                }
                s.handshake(ssl::stream_base::client);          // 证书校验由 ctx_ 的 verify mode 决定
                http::write(s, req);
                http::read(s, buffer, res);
            } else {
                beast::tcp_stream s(io);
                s.expires_after(std::chrono::seconds(job.timeoutSec));
                s.connect(resolver.resolve(host_, port));
                http::write(s, req);
                http::read(s, buffer, res);
            }
            std::string reqid;
            if (auto it = res.find("X-Reqid"); it != res.end())
                reqid = std::string(it->value());
            return {true, {}, static_cast<int>(res.result_int()), res.body(), reqid};
        } catch (const std::exception& e) {
            return {false, e.what(), 0, {}, {}};
        }
    }

    boost::asio::io_context& mainIo_;
    std::string scheme_, host_, caBundle_;
    boost::asio::ssl::context ctx_{boost::asio::ssl::context::tlsv12_client};
    bool verifyCerts_ = false;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

// ============================================================================
// QiniuObjectStore
// ============================================================================
QiniuObjectStore::QiniuObjectStore(boost::asio::io_context& mainIo, QiniuConfig cfg)
    : ObjectStorage(cfg.ticket_secret.empty() ? cfg.secret_key : cfg.ticket_secret),
      cfg_(std::move(cfg)),
      agent_(std::make_unique<QiniuHttpAgent>(mainIo, cfg_.rs_scheme, cfg_.rs_host, cfg_.ca_bundle)) {}

QiniuObjectStore::~QiniuObjectStore() = default;

std::string QiniuObjectStore::signUploadToken(const std::string& key, long long fsizeLimit,
                                              long long expiresUnix) {
    // policy 用 Writer 生成：key 里若混入特殊字符也能被正确转义。
    // scope 带 ":key" 精确锁定到单对象（Qiniu 凭证的粒度控制点），
    // insertOnly=1 禁止覆盖已有对象（防"传错文件覆盖别人"类事故）。
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("scope");      w.String((cfg_.bucket + ":" + key).c_str());
    // ⚠ 字段名必须是 **deadline**（七牛官方上传策略口径，绝对 Unix 秒）。
    // 写成 "expires" 会被七牛直接忽略 → 截止时间按 0 处理 → 恒定 401
    // {"error":"expired token"}，而签名本身是**对的**（2026-11-20 实测：
    // 同一对 AK/SK、同一 up_host，expires 写法 401、deadline 写法 200）。
    w.Key("deadline");   w.Int64(expiresUnix);
    w.Key("fsizeLimit"); w.Int64(fsizeLimit);
    w.Key("insertOnly"); w.Int(1);
    w.EndObject();
    const std::string policy(sb.GetString(), sb.GetSize());

    // 官方 upload-token 规范：sign 只覆盖 encodedPolicy，与 POST 到哪个 path 无关。
    return qsign::makeUploadToken(cfg_.access_key, cfg_.secret_key, policy);
}

std::string QiniuObjectStore::uploadUrlForClient() const {
    return cfg_.up_host;
}

std::string QiniuObjectStore::makeDownloadUrl(const FileRecord& rec, long long ttlSec) {
    // key 在 URL path 里保留 '/' 不转义（七牛对 key 的编码口径，
    // 见 qiniu_signer.h 的注释——两边编码不一致是 401 invalid sign 的头号原因）。
    const std::string keyPath = qsign::urlEncode(rec.object_key, true);
    std::string url = cfg_.domain + "/" + keyPath;
    if (!cfg_.private_space) return url;                     // 公共空间：直链即可

    // ⚠ 待签串是**完整的下载 URL**（scheme://host/path）+ "?e=" + deadline，
    //   不是"只有 path"。依据：官方 C SDK qiniu/rs.c 的
    //   Qiniu_RS_GetPolicy_MakeRequest —— 它先拼 authstr = baseUrl + "?e=" + e
    //   （baseUrl 含 scheme://host），再 Qiniu_Mac_Sign(mac, authstr)，最后
    //   把 "&token=" 接在同一个 authstr 后面；Go SDK 的 GetPolicy.MakeURL 同构。
    //   真机 2026-11-21 实测：只签 "/<key>?e=..." 时 Kodo 直接回
    //   401 {"error":"download token auth failed"}（X-Reqid 可查），
    //   按整条 URL 签才 200。
    //   副作用要注意：**scheme 也在签名里**——domain 写 https 而实际用 http 去请求
    //   （或反之）必然验签失败，所以 QiniuConfig.json 的 domain 必须与客户端真正
    //   使用的协议一致（一级测试域名没有 HTTPS 站点，只能写 http://）。
    // makeDownloadToken 的第三参是"**不含 e/token 的那半条 URL**"（这里是含
    // scheme://host 的绝对 URL），函数内部自己补 "?e=" + deadline，
    // 拼出来的待签串与官方 SDK 的 authstr 逐字节同形。
    const long long e = nowUnix() + ttlSec;
    const std::string token = qsign::makeDownloadToken(cfg_.access_key, cfg_.secret_key, url, e);
    return url + "?e=" + std::to_string(e) + "&token=" + token;
}

// 官方 v7 签名要求的时间戳头：UTC 的 "%Y%m%dT%H%M%SZ"（与 SDK auth_mac.c 里
// strftime 的格式串逐字符一致）。它**既要发出去、又要签进串里**——两头必须成对，
// 所以由调用处一次算好、分别传给 makeMacAuthorization 与请求头列表。
static std::string qiniuXQiniuDateNow() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &t);
#else
    gmtime_r(&t, &gm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &gm);
    return buf;
}

// 管理 API 的固定请求头：SDK Qiniu_Mac_AuthV2 在没有 CT 时会**强制补**
// "application/x-www-form-urlencoded" 并把它签进串里，这里与它保持一致
// （GET /stat 带 CT 头是合法且无害的，关键是"发的=签的"）。
static const char* kRsContentType = "application/x-www-form-urlencoded";

void QiniuObjectStore::stat(const std::string& key, StatCb cb) {
    // 管理 API：GET /stat/<urlsafe_b64("bucket:key")>
    // ⚠ entry 的官方顺序是 **bucket:key**（官方 C SDK qiniu/rs.c 里就是
    //   Qiniu_String_Concat3(bucket, ":", key) 再 urlsafe_base64）。本项目此前
    //   写成 key:bucket：签名与请求自洽（entry 整段都参与签名），所以七牛不会
    //   报鉴权错，而是照着一个"并不存在的 entry"回话——症状是 612/631 之类的
    //   "查无此对象"，很容易被误读成"客户端根本没传上去"。
    const std::string path = "/stat/" + qsign::entryBase64(key, cfg_.bucket);
    const std::string date = qiniuXQiniuDateNow();
    const std::string auth = qsign::makeMacAuthorization(
        cfg_.access_key, cfg_.secret_key, "GET", cfg_.rs_host, path, "", "",
        kRsContentType, date);
    agent_->request("GET", path, {{"Authorization", auth},
                                  {"Content-Type", kRsContentType},
                                  {"X-Qiniu-Date", date}},
        [cb, host = cfg_.rs_host](bool transportOk, const std::string& err, int status,
                                  const std::string& body, const std::string& reqid) {
            if (!transportOk) { cb(false, err.empty() ? "qiniu unreachable" : err, false, 0, {}); return; }
            // 612 = no such file or directory（七牛自定义状态码）：对象不存在，
            // 这是**业务事实**不是传输错误，cb(true, ..., exists=false)。
            if (status == 612) { cb(true, {}, false, 0, {}); return; }
            if (status != 200) {
                // 把端点、X-Reqid 与七牛原话一起回：401 到底是"签名不对"、
                // "entry 查不到"、还是"AK 无管理权限"，七牛这句 error 一目了然；
                // 只有 http 状态码时这三种完全分不开（2026-11-21 的教训）。
                std::string snippet = body.size() > 200 ? body.substr(0, 200) + "..." : body;
                cb(false, "qiniu stat http=" + std::to_string(status) +
                          " @" + host + " X-Reqid=" + (reqid.empty() ? "-" : reqid) +
                          " resp=" + (snippet.empty() ? "<空响应体>" : snippet),
                   false, 0, {});
                return;
            }
            // 响应体：{"hash":"<qetag>","fsize":1234,...}
            rapidjson::Document doc;
            doc.Parse(body.data(), body.size());
            if (doc.HasParseError() || !doc.IsObject()) {
                cb(false, "qiniu stat bad json", false, 0, {});
                return;
            }
            long long size = 0;
            std::string qetag;
            if (doc.HasMember("fsize") && doc["fsize"].IsInt64()) size = doc["fsize"].GetInt64();
            if (doc.HasMember("hash") && doc["hash"].IsString()) qetag = doc["hash"].GetString();
            cb(true, {}, true, size, qetag);
        });
}

void QiniuObjectStore::remove(const std::string& key, OpCb cb) {
    const std::string path = "/delete/" + qsign::entryBase64(key, cfg_.bucket);
    const std::string date = qiniuXQiniuDateNow();
    const std::string auth = qsign::makeMacAuthorization(
        cfg_.access_key, cfg_.secret_key, "POST", cfg_.rs_host, path, "", "",
        kRsContentType, date);
    // 空 body 的 POST：Content-Length: 0 照发，CT 与 X-Qiniu-Date 与签名一一对应。
    agent_->request("POST", path, {{"Authorization", auth},
                                   {"Content-Type", kRsContentType},
                                   {"X-Qiniu-Date", date},
                                   {"Content-Length", "0"}},
        [cb, host = cfg_.rs_host](bool transportOk, const std::string& err, int status,
                                 const std::string& body, const std::string& reqid) {
            if (!transportOk) { cb(false, err.empty() ? "qiniu unreachable" : err); return; }
            // 612 = 对象已不存在：幂等删除语义下按成功处理（调用方可能重试过）。
            if (status == 200 || status == 612) { cb(true, {}); return; }
            std::string snippet = body.size() > 200 ? body.substr(0, 200) + "..." : body;
            cb(false, "qiniu rm http=" + std::to_string(status) + " @" + host +
                      " X-Reqid=" + (reqid.empty() ? "-" : reqid) +
                      " resp=" + (snippet.empty() ? "<空响应体>" : snippet));
        });
}

// ============================================================================
// 工厂与配置路径
// ============================================================================
std::string qiniuConfigPath() {
    if (const char* v = std::getenv("QINIU_CONFIG"); v && *v) return v;
    return "QiniuConfig.json";
}

std::shared_ptr<ObjectStorage> makeObjectStorage(const FileRuntime& rt,
                                                 boost::asio::io_context& mainIo) {
    if (!rt.qiniuEnabled) {
        return std::make_shared<MockObjectStore>(mainIo);
    }
    // 真实模式：配置缺失/非法直接抛异常让服务启动失败（main 会打 [server] fatal）。
    // 刻意不"缺配置就悄悄退回 mock"：生产上 mock 等于文件功能没接，
    // 静默降级比启动失败危险得多。
    QiniuConfig cfg = QiniuConfig::load(qiniuConfigPath());
    return std::make_shared<QiniuObjectStore>(mainIo, std::move(cfg));
}

} // namespace kc
