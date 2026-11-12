// ============================================================================
// test_stress_03.cpp：好友全生命周期分阶段压测客户端
// ----------------------------------------------------------------------------
// 场景（1 个主用户 + N 个好友，默认 N=100）：
//   注册主用户/好友
//     -> 主用户与所有好友 TCP 登录
//     -> 主用户向 100 个好友发好友申请
//     -> 100 个好友并发通过申请
//     -> 100 个好友与主用户互相发消息（好友发文本 + 主用户回复 + 双方 ACK）
//     -> 100 个好友并发查询好友列表（主用户也查一次）
//     -> 100 个好友并发删除主用户（主用户列表最终应为空）
//
// 用法：
//   test_stress_03 --host H [--friends 100]
//                  [--http-port 8080] [--tcp-port 8888]
//                  [--hb-interval 5] [--timeout 120] [--help]
//
// 输出：
//   - 每个阶段的成功数量；
//   - 注册/登录/建申请/通过/发消息/收回复/查列表/删好友的延迟分位数。
// ============================================================================

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>
#include <cmath>

#ifndef _WIN32
#include <sys/time.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#include "message.h"
#include "message_codec.h"
#include "rapidjson/document.h"

using boost::asio::ip::tcp;
using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

struct Config {
    std::string host = "127.0.0.1";
    unsigned short httpPort = 8080;
    unsigned short tcpPort = 8888;
    int friends = 100;
    int hbIntervalSec = 5;
    int timeoutSec = 120;
};

using LatencySamples = std::vector<std::int64_t>;  // 微秒

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch()).count();
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------
// 与 test_stress_02 相同的基础设施：帧队列、HTTP、TCP 会话
// ---------------------------------------------------------------------------
class FrameQueue {
public:
    void push(kc::Message m) {
        { std::lock_guard<std::mutex> lk(mu_); q_.push_back(std::move(m)); }
        cv_.notify_one();
    }
    std::optional<kc::Message> wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, timeout, [&] { return closed_ || !q_.empty(); }))
            return std::nullopt;
        if (q_.empty()) return std::nullopt;
        kc::Message m = std::move(q_.front());
        q_.pop_front();
        return m;
    }
    void close() {
        { std::lock_guard<std::mutex> lk(mu_); closed_ = true; }
        cv_.notify_all();
    }
private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<kc::Message> q_;
    bool closed_ = false;
};

struct HttpResponse { int status = 0; std::string body; };

HttpResponse http_call(const std::string& host, unsigned short port,
                       const std::string& method, const std::string& path,
                       const std::string& token, const std::string& body) {
    boost::asio::io_context io;
    tcp::socket s(io);
    tcp::resolver resolver(io);
    boost::asio::connect(s, resolver.resolve(host, std::to_string(port)));
    std::string req;
    req += method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    req += "Connection: close\r\n";
    req += "Content-Type: application/json\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    req += body;
    boost::asio::write(s, boost::asio::buffer(req));
    std::string raw;
    boost::system::error_code ec;
    char buf[4096];
    for (;;) {
        const std::size_t n = s.read_some(boost::asio::buffer(buf), ec);
        if (n > 0) raw.append(buf, n);
        if (ec == boost::asio::error::eof) break;
        if (ec) throw boost::system::system_error(ec);
    }
    HttpResponse out;
    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        throw std::runtime_error("bad http response: no header end");
    out.body = raw.substr(headerEnd + 4);
    const std::size_t firstSpace = raw.find(' ');
    if (firstSpace != std::string::npos) {
        try { out.status = std::stoi(raw.substr(firstSpace + 1, 3)); } catch (...) {}
    }
    return out;
}

rapidjson::Document parse_json(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject())
        throw std::runtime_error("bad json: " + text);
    return doc;
}
std::string json_string(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key) || !v[key].IsString()) return {};
    return std::string(v[key].GetString(), v[key].GetStringLength());
}
std::uint64_t json_uint(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    if (v[key].IsUint64()) return v[key].GetUint64();
    if (v[key].IsInt64() && v[key].GetInt64() > 0)
        return static_cast<std::uint64_t>(v[key].GetInt64());
    return 0;
}
void expect_http_ok(const HttpResponse& r, const std::string& what) {
    if (r.status != 200)
        throw std::runtime_error(what + " failed: HTTP " + std::to_string(r.status) + " " + r.body);
}

void send_frame(tcp::socket& s, const std::string& msg) {
    const std::uint32_t net_len = htonl(static_cast<std::uint32_t>(msg.size()));
    std::vector<char> out(4 + msg.size());
    std::memcpy(out.data(), &net_len, 4);
    std::memcpy(out.data() + 4, msg.data(), msg.size());
    boost::asio::write(s, boost::asio::buffer(out));
}
std::string recv_frame(tcp::socket& s) {
    boost::system::error_code ec;
    char header[4];
    boost::asio::read(s, boost::asio::buffer(header, 4), ec);
    if (ec) throw boost::system::system_error(ec, "recv header");
    std::uint32_t net_len = 0;
    std::memcpy(&net_len, header, 4);
    const std::uint32_t len = ntohl(net_len);
    std::vector<char> body(len);
    boost::asio::read(s, boost::asio::buffer(body), ec);
    if (ec) throw boost::system::system_error(ec, "recv body");
    return std::string(body.begin(), body.end());
}
void send_msg(tcp::socket& s, const kc::Message& m) {
    send_frame(s, kc::serialize_message(m));
}
kc::Message recv_msg(tcp::socket& s) {
    const std::string raw = recv_frame(s);
    std::vector<char> body(raw.begin(), raw.end());
    kc::Message m; std::string err;
    if (!kc::parse_message(body, m, err))
        throw std::runtime_error("bad server message: " + err);
    return m;
}
bool connect_with_retry(tcp::socket& s, const std::string& host,
                        unsigned short port, int attempts = 10) {
    for (int i = 1; i <= attempts; ++i) {
        try { s.connect(tcp::endpoint(boost::asio::ip::make_address(host), port)); return true; }
        catch (const std::exception& e) {
            if (i == attempts) { std::cerr << "[stress03] tcp connect failed: " << e.what() << "\n"; return false; }
            sleep_ms(300);
        }
    }
    return false;
}

class Session {
public:
    Session(boost::asio::io_context& io, std::uint64_t uid,
            const std::string& token, int hbIntervalSec)
        : socket_(io), uid_(uid), token_(token), hbInterval_(std::max(1, hbIntervalSec)) {}
    ~Session() { stop(); }
    bool connect(const std::string& host, unsigned short port) {
        if (!connect_with_retry(socket_, host, port)) return false;
        start_recv_thread();
        kc::Message m; m.from = uid_; m.type = kc::MsgType::Login; m.content = token_;
        send_msg(socket_, m);
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        for (;;) {
            auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            if (remain.count() <= 0) return false;
            auto r = queue_.wait(remain);
            if (!r) return false;
            if (r->type == kc::MsgType::LoginAck) break;
            if (r->type == kc::MsgType::Error) { std::cerr << "[stress03] login error: " << r->content << "\n"; return false; }
        }
        start_heartbeat_thread();
        return true;
    }
    FrameQueue& queue() { return queue_; }
    void send(const kc::Message& m) {
        std::lock_guard<std::mutex> lk(writeMu_);
        send_msg(socket_, m);
    }
    void stop() {
        if (stopped_.exchange(true)) return;
        boost::system::error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        socket_.close(ec);
        queue_.close();
        if (recvThread_.joinable()) recvThread_.join();
        if (hbThread_.joinable()) hbThread_.join();
    }
private:
    void start_recv_thread() {
        recvThread_ = std::thread([this] {
            try { for (;;) { kc::Message m = recv_msg(socket_); if (stopped_.load()) break; queue_.push(std::move(m)); } }
            catch (const std::exception& e) { if (!stopped_.load()) std::cerr << "[stress03] recv exit: " << e.what() << "\n"; }
            queue_.close();
        });
    }
    void start_heartbeat_thread() {
        hbThread_ = std::thread([this] {
            while (!stopped_.load()) {
                for (int i = 0; i < hbInterval_ * 10 && !stopped_.load(); ++i)
                    sleep_ms(100);
                if (stopped_.load()) break;
                try {
                    kc::Message hb; hb.from = uid_; hb.type = kc::MsgType::Heartbeat; hb.content = "";
                    send(hb);
                } catch (const std::exception& e) {
                    if (!stopped_.load()) std::cerr << "[stress03] hb failed: " << e.what() << "\n";
                    break;
                }
            }
        });
    }
    tcp::socket socket_;
    std::uint64_t uid_;
    std::string token_;
    int hbInterval_;
    FrameQueue queue_;
    std::mutex writeMu_;
    std::atomic<bool> stopped_{false};
    std::thread recvThread_, hbThread_;
};

// 在 deadline 前等待指定类型消息；期间丢弃其他事件（好友事件、心跳 ACK 等）。
kc::Message wait_event_until(Session& s, kc::MsgType type, Clock::time_point deadline) {
    for (;;) {
        if (g_stop.load()) throw std::runtime_error("aborted");
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remain.count() <= 0) throw std::runtime_error("wait_event timeout");
        if (remain > std::chrono::milliseconds(500))
            remain = std::chrono::milliseconds(500);   // 分片等待，便于及时响应 abort
        auto m = s.queue().wait(remain);
        if (!m) continue;
        if (m->type == type) return *m;
        if (m->type == kc::MsgType::Error)
            throw std::runtime_error("server error: " + m->content);
    }
}

// ---------------------------------------------------------------------------
// 阶段同步与统计
// ---------------------------------------------------------------------------
std::atomic<int> g_phase{0};
std::atomic<int> g_ready{0};       // 已完成注册/HTTP 登录/TCP 登录
std::atomic<int> g_accepted{0};
std::atomic<int> g_messaged{0};
std::atomic<int> g_listed{0};
std::atomic<int> g_deleted{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_sendRetries{0};

bool wait_phase(int target, int timeoutSec) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeoutSec);
    while (Clock::now() < deadline) {
        if (g_stop.load()) return false;
        if (g_phase.load() >= target) return true;
        sleep_ms(20);
    }
    return false;
}

bool wait_counter(std::atomic<int>& counter, int target, int timeoutSec) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeoutSec);
    while (Clock::now() < deadline) {
        if (g_stop.load()) return counter.load() >= target;
        if (counter.load() >= target) return true;
        sleep_ms(20);
    }
    return counter.load() >= target;
}

// 发送 Text 并等待 SendAck；遇到服务端瞬态错误（如 1020/1213/1205）
// 或超时则用同一个 msg_id 重试，服务端会按 client_msg_id 去重。
void send_text_with_retry(Session& s, const kc::Message& msg, int timeoutSec,
                          int attempts = 3) {
    const int ackTimeout = std::min(timeoutSec, 30);
    for (int a = 1; a <= attempts; ++a) {
        try {
            s.send(msg);
            (void)wait_event_until(s, kc::MsgType::SendAck,
                                   Clock::now() + std::chrono::seconds(ackTimeout));
            return;
        } catch (const std::exception& e) {
            if (g_stop.load()) throw;          // 整体已中止，不再重试
            if (a == attempts) throw;
            g_sendRetries.fetch_add(1);
            std::cerr << "[stress03] retry send msg_id=" << msg.msg_id
                      << " (" << a << "/" << attempts << "): " << e.what() << "\n";
            sleep_ms(100);
        }
    }
}

struct LatGroup {
    LatencySamples reg, login, accept, send, recv, list, del;
};

struct FriendCtx {
    int idx = 0;
    std::string user;
    std::uint64_t uid = 0;
    std::string token;
    std::uint64_t requestId = 0;
};

struct FriendResult {
    LatGroup lat;
    bool ok = false;
    std::string err;
};

// ---------------------------------------------------------------------------
// HTTP 小封装（带延迟采样）
// ---------------------------------------------------------------------------
void http_register(const Config& cfg, const std::string& user, LatencySamples& lat) {
    const std::string body =
        "{\"username\":\"" + user + "\",\"password\":\"secret123\"}";
    auto t0 = now_us();
    HttpResponse r = http_call(cfg.host, cfg.httpPort, "POST", "/api/register", "", body);
    lat.push_back(now_us() - t0);
    expect_http_ok(r, "register " + user);
}

std::pair<std::uint64_t, std::string>
http_login(const Config& cfg, const std::string& user, LatencySamples& lat) {
    const std::string body =
        "{\"username\":\"" + user + "\",\"password\":\"secret123\"}";
    auto t0 = now_us();
    HttpResponse r = http_call(cfg.host, cfg.httpPort, "POST", "/api/login", "", body);
    lat.push_back(now_us() - t0);
    expect_http_ok(r, "login " + user);
    rapidjson::Document d = parse_json(r.body);
    if (!d.HasMember("data") || !d["data"].IsObject())
        throw std::runtime_error("login missing data: " + r.body);
    const std::uint64_t uid = json_uint(d["data"], "userId");
    const std::string token = json_string(d["data"], "token");
    if (!uid || token.empty())
        throw std::runtime_error("login missing userId/token: " + r.body);
    return {uid, token};
}

std::uint64_t http_create_request(const Config& cfg, const std::string& token,
                                  std::uint64_t toUid, LatencySamples& lat) {
    const std::string body =
        "{\"toUserId\":" + std::to_string(toUid) + ",\"message\":\"stress03\"}";
    auto t0 = now_us();
    HttpResponse r = http_call(cfg.host, cfg.httpPort, "POST",
                               "/api/friends/requests", token, body);
    lat.push_back(now_us() - t0);
    expect_http_ok(r, "create friend request");
    rapidjson::Document d = parse_json(r.body);
    if (!d.HasMember("data") || !d["data"].IsObject() || !d["data"].HasMember("request"))
        throw std::runtime_error("missing request: " + r.body);
    const std::uint64_t rid = json_uint(d["data"]["request"], "requestId");
    if (!rid) throw std::runtime_error("missing requestId: " + r.body);
    return rid;
}

std::size_t http_count_friends(const Config& cfg, const std::string& token) {
    HttpResponse r = http_call(cfg.host, cfg.httpPort, "GET", "/api/friends", token, "");
    expect_http_ok(r, "list friends");
    rapidjson::Document d = parse_json(r.body);
    if (!d.HasMember("data") || !d["data"].IsObject()) return 0;
    const auto& data = d["data"];
    if (!data.HasMember("friends") || !data["friends"].IsArray()) return 0;
    return data["friends"].Size();
}

// ---------------------------------------------------------------------------
// 单个好友线程：注册 -> HTTP 登录 -> TCP 登录 -> 按阶段推进
// ---------------------------------------------------------------------------
void friend_worker(const Config& cfg, int idx, std::uint64_t runId,
                   std::uint64_t mainUid,
                   std::vector<FriendCtx>& ctxs,
                   std::vector<FriendResult>& results) {
    FriendCtx& ctx = ctxs[idx];
    FriendResult& res = results[idx];
    // io_context 必须先于 Session 构造、晚于 Session 析构：
    // Session 内部的 socket 引用了 io_context。
    boost::asio::io_context io;
    std::unique_ptr<Session> session;
    try {
        ctx.idx = idx;
        ctx.user = "s3F_" + std::to_string(runId) + "_" + std::to_string(idx);

        http_register(cfg, ctx.user, res.lat.reg);
        auto [uid, token] = http_login(cfg, ctx.user, res.lat.login);
        ctx.uid = uid;
        ctx.token = token;

        session = std::make_unique<Session>(io, uid, token, cfg.hbIntervalSec);
        if (!session->connect(cfg.host, cfg.tcpPort))
            throw std::runtime_error("tcp login failed");
        g_ready.fetch_add(1);
        if (cfg.friends <= 10) std::cout << "[stress03] friend " << idx << " TCP ready\n";

        // ---- 阶段 1：等待主用户创建完好友申请，然后并发通过 ----
        if (!wait_phase(1, cfg.timeoutSec))
            throw std::runtime_error("aborted before accept");
        {
            auto t0 = now_us();
            HttpResponse r = http_call(cfg.host, cfg.httpPort, "POST",
                "/api/friends/requests/" + std::to_string(ctx.requestId) + "/accept",
                token, "");
            res.lat.accept.push_back(now_us() - t0);
            expect_http_ok(r, "accept friend request");
        }
        g_accepted.fetch_add(1);
        if (cfg.friends <= 10) std::cout << "[stress03] friend " << idx << " accepted\n";

        // ---- 阶段 2：好友发消息给主用户，主用户回复，好友 ACK ----
        if (!wait_phase(2, cfg.timeoutSec))
            throw std::runtime_error("aborted before messaging");
        {
            auto t0 = now_us();
            kc::Message m;
            m.from = uid; m.to = mainUid; m.type = kc::MsgType::Text;
            m.msg_id = 1000000ULL + static_cast<std::uint64_t>(idx);
            m.content = "hello-" + std::to_string(idx);
            send_text_with_retry(*session, m, cfg.timeoutSec);
            res.lat.send.push_back(now_us() - t0);
        }
        {
            auto t0 = now_us();
            kc::Message reply;
            try {
                reply = wait_event_until(*session, kc::MsgType::Text,
                                         Clock::now() + std::chrono::seconds(cfg.timeoutSec));
            } catch (const std::exception& e) {
                throw std::runtime_error(std::string("wait reply: ") + e.what());
            }
            const std::string expect = "reply-" + std::to_string(idx);
            if (reply.content != expect)
                throw std::runtime_error("reply mismatch: got '" + reply.content +
                                         "', expected '" + expect + "'");
            kc::Message ack;
            ack.from = uid; ack.type = kc::MsgType::Ack;
            ack.conversation_id = reply.conversation_id;
            ack.seq = reply.seq;
            ack.content = "delivered";
            session->send(ack);
            res.lat.recv.push_back(now_us() - t0);
        }
        g_messaged.fetch_add(1);
        if (cfg.friends <= 10) std::cout << "[stress03] friend " << idx << " messaged\n";

        // ---- 阶段 3：每个好友查询自己的好友列表 ----
        if (!wait_phase(3, cfg.timeoutSec))
            throw std::runtime_error("aborted before list");
        {
            auto t0 = now_us();
            HttpResponse r = http_call(cfg.host, cfg.httpPort, "GET",
                                       "/api/friends", token, "");
            res.lat.list.push_back(now_us() - t0);
            expect_http_ok(r, "list friends");
        }
        g_listed.fetch_add(1);
        if (cfg.friends <= 10) std::cout << "[stress03] friend " << idx << " listed\n";

        // ---- 阶段 4：每个好友删除主用户（删除是双向的） ----
        if (!wait_phase(4, cfg.timeoutSec))
            throw std::runtime_error("aborted before delete");
        {
            auto t0 = now_us();
            HttpResponse r = http_call(cfg.host, cfg.httpPort, "DELETE",
                "/api/friends/" + std::to_string(mainUid), token, "");
            res.lat.del.push_back(now_us() - t0);
            expect_http_ok(r, "delete friend");
        }
        g_deleted.fetch_add(1);
        if (cfg.friends <= 10) std::cout << "[stress03] friend " << idx << " deleted, stopping session\n";

        session->stop();
        if (cfg.friends <= 10) std::cout << "[stress03] friend " << idx << " stopped\n";
        res.ok = true;
        if (cfg.friends <= 10) std::cout << "[stress03] friend " << idx << " worker exiting" << std::endl;
    } catch (const std::exception& e) {
        res.err = e.what();
        g_failed.fetch_add(1);
        g_stop.store(true);
        if (session) session->stop();
    }
}

// ---------------------------------------------------------------------------
// 统计与输出
// ---------------------------------------------------------------------------
struct Percentiles { double p50, p95, p99, avg; std::int64_t max; };

Percentiles calc(const LatencySamples& in) {
    if (in.empty()) return {0, 0, 0, 0, 0};
    LatencySamples v = in;
    std::sort(v.begin(), v.end());
    auto at = [&](double p) {
        std::size_t i = static_cast<std::size_t>(std::llround((p / 100.0) * (v.size() - 1)));
        return static_cast<double>(v[i]) / 1000.0;  // ms
    };
    double sum = 0;
    for (auto x : v) sum += static_cast<double>(x);
    return {at(50), at(95), at(99), sum / v.size() / 1000.0, v.back() / 1000};
}

void print_latency(const char* name, const LatencySamples& samples) {
    const Percentiles p = calc(samples);
    std::cout << std::left << std::setw(18) << name << " : "
              << std::fixed << std::setprecision(2)
              << p.p50 << " / " << p.p95 << " / " << p.p99 << " / "
              << p.avg << " / " << static_cast<double>(p.max)
              << " ms  (n=" << samples.size() << ")\n";
}

void print_usage(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n"
              << "  --host HOST        服务器地址（默认 127.0.0.1）\n"
              << "  --http-port PORT   HTTP 端口（默认 8080）\n"
              << "  --tcp-port PORT    TCP 端口（默认 8888）\n"
              << "  --friends N        好友数量（默认 100）\n"
              << "  --hb-interval SEC  心跳间隔秒（默认 5）\n"
              << "  --timeout SEC      单阶段超时秒（默认 120）\n"
              << "  --help             帮助\n";
}

int parse_int(const std::string& s, const std::string& name) {
    try {
        int v = std::stoi(s);
        if (v < 0) throw std::invalid_argument("neg");
        return v;
    } catch (...) { throw std::runtime_error("invalid " + name + ": " + s); }
}

void merge_append(LatencySamples& dst, const LatencySamples& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << std::unitbuf;   // 阶段较多，逐条刷新，便于定位卡在哪一步
    std::cerr << std::unitbuf;
    Config cfg;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto val = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(a + " requires value");
                return argv[++i];
            };
            if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
            else if (a == "--host") cfg.host = val();
            else if (a == "--http-port") cfg.httpPort = (unsigned short)parse_int(val(), a);
            else if (a == "--tcp-port") cfg.tcpPort = (unsigned short)parse_int(val(), a);
            else if (a == "--friends") cfg.friends = parse_int(val(), a);
            else if (a == "--hb-interval") cfg.hbIntervalSec = parse_int(val(), a);
            else if (a == "--timeout") cfg.timeoutSec = parse_int(val(), a);
            else throw std::runtime_error("unknown option: " + a);
        }
        if (cfg.friends <= 0) throw std::runtime_error("friends must be > 0");
        if (cfg.timeoutSec <= 0) throw std::runtime_error("timeout must be > 0");
    } catch (const std::exception& e) {
        std::cerr << "[stress03] " << e.what() << "\n\n"; print_usage(argv[0]); return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::uint64_t runId = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) % 1000000000ULL;

    std::cout << "[stress03] host=" << cfg.host
              << " http=" << cfg.httpPort << " tcp=" << cfg.tcpPort
              << " friends=" << cfg.friends
              << " runId=" << runId
              << " hb=" << cfg.hbIntervalSec << "s"
              << " timeout=" << cfg.timeoutSec << "s\n";

    const auto runStart = Clock::now();
    const int N = cfg.friends;

    std::vector<FriendCtx> ctxs(N);
    std::vector<FriendResult> results(N);
    std::vector<std::thread> friendThreads;
    LatencySamples mainReg, mainLogin, mainList, requestLat;
    std::mutex requestLatMu;
    std::uint64_t mainUid = 0;
    std::string mainToken;

    try {
        // ---- 主用户：注册 + HTTP 登录 + TCP 登录 ----
        const std::string mainUser = "s3M_" + std::to_string(runId);
        http_register(cfg, mainUser, mainReg);
        auto [uid, token] = http_login(cfg, mainUser, mainLogin);
        mainUid = uid;
        mainToken = token;

        boost::asio::io_context mainIo;
        Session mainSession(mainIo, mainUid, mainToken, cfg.hbIntervalSec);
        if (!mainSession.connect(cfg.host, cfg.tcpPort))
            throw std::runtime_error("main tcp login failed");
        std::cout << "[stress03] main userId=" << mainUid
                  << ", starting " << N << " friend workers...\n";

        // ---- 启动好友线程：注册/登录/TCP 建连 ----
        friendThreads.reserve(N);
        for (int i = 0; i < N; ++i) {
            friendThreads.emplace_back(friend_worker, std::cref(cfg), i, runId,
                                       mainUid, std::ref(ctxs), std::ref(results));
        }

        if (!wait_counter(g_ready, N, cfg.timeoutSec))
            throw std::runtime_error("friends not ready: " +
                                     std::to_string(g_ready.load()) + "/" + std::to_string(N));
        std::cout << "[stress03] all friends registered + TCP logged in; creating requests...\n";

        // ---- 主用户向所有好友并发创建好友申请 ----
        std::vector<std::thread> reqThreads;
        reqThreads.reserve(N);
        try {
            for (int i = 0; i < N; ++i) {
                reqThreads.emplace_back([&, i] {
                    try {
                        LatencySamples local;
                        const std::uint64_t rid =
                            http_create_request(cfg, mainToken, ctxs[i].uid, local);
                        ctxs[i].requestId = rid;
                        std::lock_guard<std::mutex> lk(requestLatMu);
                        merge_append(requestLat, local);
                    } catch (const std::exception& e) {
                        results[i].err = std::string("create request: ") + e.what();
                        g_failed.fetch_add(1);
                        g_stop.store(true);
                    }
                });
            }
        } catch (...) {
            for (auto& t : reqThreads) if (t.joinable()) t.join();
            throw;
        }
        for (auto& t : reqThreads) t.join();
        if (g_stop.load())
            throw std::runtime_error("create friend requests failed");
        for (int i = 0; i < N; ++i) {
            if (ctxs[i].requestId == 0)
                throw std::runtime_error("friend " + std::to_string(i) + " missing requestId");
        }
        std::cout << "[stress03] friend requests created; waiting accepts...\n";

        // ---- 阶段 1：好友并发通过 ----
        g_phase.store(1);
        if (!wait_counter(g_accepted, N, cfg.timeoutSec))
            throw std::runtime_error("accept timeout: " +
                                     std::to_string(g_accepted.load()) + "/" + std::to_string(N));

        // ---- 阶段 2：好友 -> 主用户发消息；主用户 ACK + 回复 ----
        g_phase.store(2);
        std::cout << "[stress03] friends accepted; exchanging messages...\n";
        std::unordered_map<std::uint64_t, int> uidToIdx;
        uidToIdx.reserve(static_cast<std::size_t>(N) * 2);
        for (int i = 0; i < N; ++i) uidToIdx[ctxs[i].uid] = i;

        std::vector<kc::Message> received;
        received.reserve(N);
        const auto recvDeadline = Clock::now() + std::chrono::seconds(cfg.timeoutSec);
        for (int i = 0; i < N; ++i) {
            kc::Message got = wait_event_until(mainSession, kc::MsgType::Text, recvDeadline);
            if (!uidToIdx.count(got.from))
                throw std::runtime_error("text from unknown user: " + std::to_string(got.from));
            received.push_back(got);
        }
        // 主用户逐条回复并等待 SendAck（遇到瞬态错误按同一 msg_id 重试）。
        // 先回复、后补投递 ACK，避免 ACK 与 Reply 同时写同一行
        // conversation_members 触发 MariaDB 1020。
        int replyAcks = 0;
        for (const auto& got : received) {
            const int idx = uidToIdx[got.from];
            kc::Message reply;
            reply.from = mainUid; reply.to = got.from;
            reply.type = kc::MsgType::Text;
            reply.msg_id = 2000000ULL + static_cast<std::uint64_t>(idx);
            reply.content = "reply-" + std::to_string(idx);
            send_text_with_retry(mainSession, reply, cfg.timeoutSec);
            ++replyAcks;
        }
        // 回复全部落地后，再统一补发主用户对“好友文本”的投递 ACK。
        for (const auto& got : received) {
            kc::Message ack;
            ack.from = mainUid; ack.type = kc::MsgType::Ack;
            ack.conversation_id = got.conversation_id;
            ack.seq = got.seq;
            ack.content = "delivered";
            mainSession.send(ack);
        }
        std::cout << "[stress03] replies acked=" << replyAcks << "/" << N
                  << ", send retries=" << g_sendRetries.load() << "\n";

        if (!wait_counter(g_messaged, N, cfg.timeoutSec))
            throw std::runtime_error("messaging timeout: " +
                                     std::to_string(g_messaged.load()) + "/" + std::to_string(N));

        // ---- 阶段 3：好友并发查列表，主用户也查一次 ----
        g_phase.store(3);
        std::cout << "[stress03] messages exchanged; querying friend lists...\n";
        {
            auto t0 = now_us();
            HttpResponse r = http_call(cfg.host, cfg.httpPort, "GET", "/api/friends", mainToken, "");
            mainList.push_back(now_us() - t0);
            expect_http_ok(r, "main list friends");
        }
        if (!wait_counter(g_listed, N, cfg.timeoutSec))
            throw std::runtime_error("list timeout: " +
                                     std::to_string(g_listed.load()) + "/" + std::to_string(N));

        // ---- 阶段 4：好友并发删除主用户 ----
        g_phase.store(4);
        std::cout << "[stress03] lists queried; deleting friends...\n";
        if (!wait_counter(g_deleted, N, cfg.timeoutSec))
            throw std::runtime_error("delete timeout: " +
                                     std::to_string(g_deleted.load()) + "/" + std::to_string(N));

        // ---- 校验主用户好友列表应为空 ----
        std::cout << "[stress03] all deletes acknowledged; verifying main friend list...\n";
        const std::size_t remaining = http_count_friends(cfg, mainToken);
        std::cout << "[stress03] verify done, remaining=" << remaining << "; joining friends...\n";
        g_phase.store(5);

        if (cfg.friends <= 10) std::cout << "[stress03] joining " << friendThreads.size() << " friend threads...\n";
        for (auto& t : friendThreads) t.join();
        std::cout << "[stress03] friends joined; stopping main session...\n";
        mainSession.stop();

        const auto runEnd = Clock::now();
        const double elapsed = std::chrono::duration<double>(runEnd - runStart).count();

        // ---- 合并延迟 ----
        LatencySamples reg = mainReg, login = mainLogin;
        LatencySamples accept, send, recv, list = mainList, del;
        for (const auto& r : results) {
            merge_append(reg, r.lat.reg);
            merge_append(login, r.lat.login);
            merge_append(accept, r.lat.accept);
            merge_append(send, r.lat.send);
            merge_append(recv, r.lat.recv);
            merge_append(list, r.lat.list);
            merge_append(del, r.lat.del);
        }

        int okCount = 0;
        for (const auto& r : results) if (r.ok) ++okCount;
        for (int i = 0; i < N; ++i) {
            if (!results[i].err.empty())
                std::cerr << "[stress03] friend " << i << " error: " << results[i].err << "\n";
        }

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n===== Stress03 Report =====\n";
        std::cout << "elapsed                  : " << elapsed << " s\n";
        std::cout << "friends                  : " << N << "\n";
        std::cout << "friends ready            : " << g_ready.load() << "\n";
        std::cout << "friend requests created  : " << requestLat.size() << "\n";
        std::cout << "accepted                 : " << g_accepted.load() << "\n";
        std::cout << "messages sent+acked      : " << g_messaged.load() << "\n";
        std::cout << "friend lists queried     : " << g_listed.load() << "\n";
        std::cout << "friends deleted          : " << g_deleted.load() << "\n";
        std::cout << "full lifecycle ok        : " << okCount << "/" << N << "\n";
        std::cout << "failed                   : " << g_failed.load() << "\n";
        std::cout << "send retries             : " << g_sendRetries.load() << "\n";
        std::cout << "main friends after delete: " << remaining << "\n";
        std::cout << "\nlatency (ms)        p50 / p95 / p99 / avg / max\n";
        print_latency("register", reg);
        print_latency("login", login);
        print_latency("create request", requestLat);
        print_latency("accept", accept);
        print_latency("tcp send", send);
        print_latency("tcp reply", recv);
        print_latency("list friends", list);
        print_latency("delete friend", del);
        std::cout << "===========================\n";

        const bool allOk = (g_failed.load() == 0) && (okCount == N) && (remaining == 0);
        return allOk ? 0 : 1;
    } catch (const std::exception& e) {
        g_stop.store(true);
        g_phase.store(99);
        g_failed.fetch_add(1);
        std::cerr << "[stress03] fatal: " << e.what() << "\n";
        for (auto& t : friendThreads) {
            if (t.joinable()) t.join();
        }
        for (int i = 0; i < N; ++i) {
            if (!results[i].err.empty())
                std::cerr << "[stress03] friend " << i << " error: " << results[i].err << "\n";
        }
        std::cout << "\n===== Stress03 Aborted =====\n";
        std::cout << "friends ready   : " << g_ready.load() << "/" << N << "\n";
        std::cout << "accepted        : " << g_accepted.load() << "/" << N << "\n";
        std::cout << "messages        : " << g_messaged.load() << "/" << N << "\n";
        std::cout << "listed          : " << g_listed.load() << "/" << N << "\n";
        std::cout << "deleted         : " << g_deleted.load() << "/" << N << "\n";
        std::cout << "failed          : " << g_failed.load() << "\n";
        std::cout << "============================\n";
        return 1;
    }
}
