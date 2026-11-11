// ============================================================================
// test_stress_02.cpp：好友 + 消息链路真正压测客户端（完整版）
// 用法：
//   test_stress_02 --host H --threads N --duration SEC [--warmup SEC]
//                  [--http-port P] [--tcp-port P] [--hb-interval SEC]
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
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

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
    int threads = 20;
    int durationSec = 30;
    int warmupSec = 5;
    int hbIntervalSec = 5;
};

using LatencySamples = std::vector<std::int64_t>;  // 微秒

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch()).count();
}

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
            if (i == attempts) { std::cerr << "[stress] tcp connect failed: " << e.what() << "\n"; return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
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
            if (r->type == kc::MsgType::Error) { std::cerr << "[stress] login error: " << r->content << "\n"; return false; }
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
            catch (const std::exception& e) { if (!stopped_.load()) std::cerr << "[stress] recv exit: " << e.what() << "\n"; }
            queue_.close();
        });
    }
    void start_heartbeat_thread() {
        hbThread_ = std::thread([this] {
            while (!stopped_.load()) {
                for (int i = 0; i < hbInterval_ * 10 && !stopped_.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (stopped_.load()) break;
                try {
                    kc::Message hb; hb.from = uid_; hb.type = kc::MsgType::Heartbeat; hb.content = "";
                    send(hb);
                } catch (const std::exception& e) {
                    if (!stopped_.load()) std::cerr << "[stress] hb failed: " << e.what() << "\n";
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

struct WorkerCtx {
    std::uint64_t uidA = 0, uidB = 0;
    std::string tokenA, tokenB;
    std::string userA, userB;
};

WorkerCtx prepare_one(const Config& cfg, int idx, std::uint64_t runId) {
    WorkerCtx w;
    w.userA = "s2A_" + std::to_string(runId) + "_" + std::to_string(idx);
    w.userB = "s2B_" + std::to_string(runId) + "_" + std::to_string(idx);
    const std::string password = "secret123";
    auto reg = [&](const std::string& u) {
        HttpResponse r = http_call(cfg.host, cfg.httpPort, "POST", "/api/register", "",
            "{\"username\":\"" + u + "\",\"password\":\"" + password + "\"}");
        expect_http_ok(r, "register " + u);
    };
    auto login_http = [&](const std::string& u) {
        HttpResponse r = http_call(cfg.host, cfg.httpPort, "POST", "/api/login", "",
            "{\"username\":\"" + u + "\",\"password\":\"" + password + "\"}");
        expect_http_ok(r, "login " + u);
        rapidjson::Document d = parse_json(r.body);
        if (!d.HasMember("data") || !d["data"].IsObject())
            throw std::runtime_error("login missing data: " + r.body);
        return std::make_pair(json_uint(d["data"], "userId"), json_string(d["data"], "token"));
    };
    reg(w.userA); reg(w.userB);
    auto [uidA, tokA] = login_http(w.userA);
    auto [uidB, tokB] = login_http(w.userB);
    if (!uidA || !uidB || tokA.empty() || tokB.empty()) throw std::runtime_error("prepare login failed");
    w.uidA = uidA; w.uidB = uidB; w.tokenA = tokA; w.tokenB = tokB;

    HttpResponse reqResp = http_call(cfg.host, cfg.httpPort, "POST",
        "/api/friends/requests", w.tokenA,
        "{\"toUserId\":" + std::to_string(w.uidB) + ",\"message\":\"setup\"}");
    expect_http_ok(reqResp, "create friend request");
    rapidjson::Document reqDoc = parse_json(reqResp.body);
    if (!reqDoc["data"].HasMember("request")) throw std::runtime_error("missing request: " + reqResp.body);
    const std::uint64_t rid = json_uint(reqDoc["data"]["request"], "requestId");
    if (!rid) throw std::runtime_error("missing requestId");
    HttpResponse acc = http_call(cfg.host, cfg.httpPort, "POST",
        "/api/friends/requests/" + std::to_string(rid) + "/accept", w.tokenB, "");
    expect_http_ok(acc, "accept friend request");
    return w;
}

kc::Message wait_event(Session& s, kc::MsgType type,
                       std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remain.count() <= 0) throw std::runtime_error("wait_event timeout");
        auto m = s.queue().wait(remain);
        if (!m) throw std::runtime_error("wait_event closed");
        if (m->type == type) return *m;
        if (m->type == kc::MsgType::Error) throw std::runtime_error("server error: " + m->content);
    }
}

// 施压阶段单次操作：A 发 Text -> A 收 Ack -> B 收 Text -> B 回 Ack -> A 查列表
void one_iter(const Config& cfg, Session& sa, Session& sb, const WorkerCtx& w,
              std::uint64_t msgId, LatencySamples& tcpLat, LatencySamples& httpLat) {
    const std::string text = "p-" + std::to_string(msgId);

    // TCP 往返：A 发 + 等 SendAck
    auto t0 = now_us();
    kc::Message m; m.from = w.uidA; m.to = w.uidB; m.type = kc::MsgType::Text;
    m.msg_id = msgId; m.content = text;
    sa.send(m);
    (void)wait_event(sa, kc::MsgType::SendAck);
    tcpLat.push_back(now_us() - t0);

    // B 收 Text 并回 Ack
    auto t1 = now_us();
    kc::Message got = wait_event(sb, kc::MsgType::Text);
    if (got.content != text) throw std::runtime_error("text mismatch");
    kc::Message delivery; delivery.from = w.uidB; delivery.type = kc::MsgType::Ack;
    delivery.conversation_id = got.conversation_id; delivery.seq = got.seq;
    delivery.content = "delivered";
    sb.send(delivery);
    tcpLat.push_back(now_us() - t1);

    // HTTP 查询好友列表
    auto t2 = now_us();
    HttpResponse r = http_call(cfg.host, cfg.httpPort, "GET", "/api/friends", w.tokenA, "");
    expect_http_ok(r, "list friends");
    httpLat.push_back(now_us() - t2);
}

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

void print_usage(const char* prog) {
    std::cout << "用法: " << prog << " [选项]\n"
              << "  --host HOST        服务器地址（默认 127.0.0.1）\n"
              << "  --http-port PORT   HTTP 端口（默认 8080）\n"
              << "  --tcp-port PORT    TCP 端口（默认 8888）\n"
              << "  --threads N        并发线程数（默认 20）\n"
              << "  --duration SEC     施压时长秒（默认 30）\n"
              << "  --warmup SEC       预热秒数，不计入统计（默认 5）\n"
              << "  --hb-interval SEC  心跳间隔秒（默认 5）\n"
              << "  --help             帮助\n";
}

int parse_int(const std::string& s, const std::string& name) {
    try {
        int v = std::stoi(s);
        if (v < 0) throw std::invalid_argument("neg");
        return v;
    } catch (...) { throw std::runtime_error("invalid " + name + ": " + s); }
}

struct ThreadResult {
    LatencySamples tcpLat, httpLat;
    std::uint64_t ok = 0, fail = 0;
    std::string firstErr;
};

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
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
            else if (a == "--threads") cfg.threads = parse_int(val(), a);
            else if (a == "--duration") cfg.durationSec = parse_int(val(), a);
            else if (a == "--warmup") cfg.warmupSec = parse_int(val(), a);
            else if (a == "--hb-interval") cfg.hbIntervalSec = parse_int(val(), a);
            else throw std::runtime_error("unknown option: " + a);
        }
        if (cfg.threads <= 0) throw std::runtime_error("threads must be > 0");
        if (cfg.durationSec <= 0) throw std::runtime_error("duration must be > 0");
    } catch (const std::exception& e) {
        std::cerr << "[stress] " << e.what() << "\n\n"; print_usage(argv[0]); return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::uint64_t runId = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) % 1000000000ULL;

    std::cout << "[stress] host=" << cfg.host
              << " http=" << cfg.httpPort << " tcp=" << cfg.tcpPort
              << " threads=" << cfg.threads
              << " duration=" << cfg.durationSec << "s"
              << " warmup=" << cfg.warmupSec << "s"
              << " hb=" << cfg.hbIntervalSec << "s\n";

    // ---------- 准备阶段：每个线程注册/登录/建连/建立好友关系 ----------
    std::cout << "[stress] preparing " << cfg.threads << " workers...\n";
    std::vector<WorkerCtx> ctxs(cfg.threads);
    std::atomic<int> prepOk{0};
    {
        std::vector<std::thread> prep;
        for (int i = 0; i < cfg.threads; ++i) {
            prep.emplace_back([&, i] {
                try { ctxs[i] = prepare_one(cfg, i, runId); prepOk.fetch_add(1); }
                catch (const std::exception& e) { std::cerr << "[stress] prepare " << i << " failed: " << e.what() << "\n"; }
            });
        }
        for (auto& t : prep) t.join();
    }
    if (prepOk.load() != cfg.threads) {
        std::cerr << "[stress] preparation failed (" << prepOk.load() << "/" << cfg.threads << "), abort\n";
        return 1;
    }
    std::cout << "[stress] preparation done\n";

    // ---------- 施压阶段 ----------
    std::vector<ThreadResult> results(cfg.threads);
    std::atomic<bool> warmupDone{false};
    std::atomic<std::uint64_t> globalMsgId{1};

    const auto runStart = Clock::now();
    std::vector<std::thread> workers;
    for (int i = 0; i < cfg.threads; ++i) {
        workers.emplace_back([&, i] {
            boost::asio::io_context io;
            Session sa(io, ctxs[i].uidA, ctxs[i].tokenA, cfg.hbIntervalSec);
            Session sb(io, ctxs[i].uidB, ctxs[i].tokenB, cfg.hbIntervalSec);
            if (!sa.connect(cfg.host, cfg.tcpPort) || !sb.connect(cfg.host, cfg.tcpPort)) {
                results[i].fail++;
                results[i].firstErr = "tcp connect/login failed";
                return;
            }
            // 等预热结束再开始计时统计
            while (!warmupDone.load() && !g_stop.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

            while (!g_stop.load()) {
                try {
                    std::uint64_t id = globalMsgId.fetch_add(1);
                    one_iter(cfg, sa, sb, ctxs[i], id, results[i].tcpLat, results[i].httpLat);
                    results[i].ok++;
                } catch (const std::exception& e) {
                    results[i].fail++;
                    if (results[i].firstErr.empty()) results[i].firstErr = e.what();
                }
            }
            sa.stop(); sb.stop();
        });
    }

    // 主线程计时：预热 -> 施压 duration
    std::this_thread::sleep_for(std::chrono::seconds(cfg.warmupSec));
    // 清空预热阶段的延迟样本（简单做法：记录当前大小，最后只统计之后的）
    // 这里为了简单，直接在预热结束时标记并清空各线程的样本不可行（线程在跑），
    // 折中：预热时长内样本也保留，但报告里注明含预热；若需严格，可缩短 warmup。
    warmupDone.store(true);
    const auto stressStart = Clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(cfg.durationSec));
    g_stop.store(true);
    for (auto& t : workers) t.join();
    const auto stressEnd = Clock::now();

    // ---------- 汇总 ----------
    std::uint64_t ok = 0, fail = 0;
    LatencySamples tcpAll, httpAll;
    for (auto& r : results) {
        ok += r.ok; fail += r.fail;
        tcpAll.insert(tcpAll.end(), r.tcpLat.begin(), r.tcpLat.end());
        httpAll.insert(httpAll.end(), r.httpLat.begin(), r.httpLat.end());
        if (!r.firstErr.empty()) std::cerr << "[stress] sample error: " << r.firstErr << "\n";
    }
    const double elapsed = std::chrono::duration<double>(stressEnd - stressStart).count();
    const auto tcpP = calc(tcpAll);
    const auto httpP = calc(httpAll);

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n===== Stress Report =====\n";
    std::cout << "elapsed        : " << elapsed << " s\n";
    std::cout << "workers        : " << cfg.threads << "\n";
    std::cout << "iterations ok  : " << ok << "\n";
    std::cout << "iterations fail: " << fail << "\n";
    std::cout << "iter throughput: " << (ok + fail) / (elapsed > 0 ? elapsed : 1) << " iter/s\n";
    std::cout << "tcp requests   : " << tcpAll.size() << "\n";
    std::cout << "  tcp   p50/p95/p99/avg/max : "
              << tcpP.p50 << " / " << tcpP.p95 << " / " << tcpP.p99 << " / "
              << tcpP.avg << " / " << (double)tcpP.max << " ms\n";
    std::cout << "http requests  : " << httpAll.size() << "\n";
    std::cout << "  http  p50/p95/p99/avg/max : "
              << httpP.p50 << " / " << httpP.p95 << " / " << httpP.p99 << " / "
              << httpP.avg << " / " << (double)httpP.max << " ms\n";
    std::cout << "=========================\n";

    return fail == 0 ? 0 : 1;
}
