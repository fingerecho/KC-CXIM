// ============================================================================
// test_stress_01.cpp：好友全链路并发压测客户端
// ----------------------------------------------------------------------------
// 每个线程循环执行一遍完整好友流程：
//   注册 A/B -> HTTP 登录 A/B -> TCP 登录 A/B -> A 发好友申请
//   -> B 通过 -> A 发消息给 B -> A 查询好友列表 -> A 删除好友。
//
// 流程中的每一步都会校验结果，只有整条链路全部成功才计入成功次数。
// 结束后统计总成功次数、失败次数与总耗时。
//
// 用法：
//   test_stress_01 [http_port] [tcp_port] [threads] [loops]
//   test_stress_01 --http-port 8080 --tcp-port 8888 --threads 20 --loops 10
//   test_stress_01 --threads 8 --loops 0      # loops=0 表示一直循环到 Ctrl+C
//
// 默认：http=8080, tcp=8888, threads=10, loops=1
// ============================================================================

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

struct Config {
    std::string host = "127.0.0.1";
    unsigned short httpPort = 8080;
    unsigned short tcpPort = 8888;
    int threads = 10;
    int loops = 1;
};

struct Stats {
    std::atomic<std::uint64_t> success{0};
    std::atomic<std::uint64_t> failure{0};
};

struct HttpUser {
    std::uint64_t uid = 0;
    std::string token;
};

struct HttpResponse {
    int status = 0;
    std::string body;
};

void set_recv_timeout(tcp::socket& s, int seconds) {
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
        try { out.status = std::stoi(raw.substr(firstSpace + 1, 3)); }
        catch (...) { out.status = 0; }
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
    char header[4];
    boost::asio::read(s, boost::asio::buffer(header, 4));
    std::uint32_t net_len = 0;
    std::memcpy(&net_len, header, 4);
    const std::uint32_t len = ntohl(net_len);

    std::vector<char> body(len);
    boost::asio::read(s, boost::asio::buffer(body));
    return std::string(body.begin(), body.end());
}

void send_msg(tcp::socket& s, const kc::Message& m) {
    send_frame(s, kc::serialize_message(m));
}

kc::Message recv_msg(tcp::socket& s) {
    const std::string raw = recv_frame(s);
    std::vector<char> body(raw.begin(), raw.end());
    kc::Message m;
    std::string err;
    if (!kc::parse_message(body, m, err))
        throw std::runtime_error("bad server message: " + err);
    return m;
}

bool connect_with_retry(tcp::socket& s, const std::string& host,
                        unsigned short port, int attempts = 10) {
    for (int i = 1; i <= attempts; ++i) {
        try {
            s.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));
            return true;
        } catch (const std::exception& e) {
            if (i == attempts) {
                std::cerr << "[stress] tcp connect failed: " << e.what() << "\n";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    return false;
}

kc::Message login(tcp::socket& s, std::uint64_t uid, const std::string& token) {
    kc::Message m;
    m.from = uid;
    m.type = kc::MsgType::Login;
    m.content = token;
    send_msg(s, m);

    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == kc::MsgType::LoginAck || r.type == kc::MsgType::Error) return r;
    }
}

kc::Message wait_for_type(tcp::socket& s, kc::MsgType type) {
    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == type) return r;
        if (r.type == kc::MsgType::Error)
            throw std::runtime_error("server error: " + r.content);
    }
}

kc::Message send_text(tcp::socket& s, std::uint64_t from, std::uint64_t to,
                      std::uint64_t clientMsgId, const std::string& text) {
    kc::Message m;
    m.from = from;
    m.to = to;
    m.type = kc::MsgType::Text;
    m.msg_id = clientMsgId;
    m.content = text;
    send_msg(s, m);

    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == kc::MsgType::SendAck) return r;
        if (r.type == kc::MsgType::Error)
            throw std::runtime_error("send error: " + r.content);
    }
}

void send_delivery_ack(tcp::socket& s, std::uint64_t from,
                       std::uint64_t conversationId, std::uint64_t seq) {
    kc::Message m;
    m.from = from;
    m.type = kc::MsgType::Ack;
    m.conversation_id = conversationId;
    m.seq = seq;
    m.content = "delivered";
    send_msg(s, m);
}

void register_user(const Config& cfg, const std::string& username,
                   const std::string& password) {
    const std::string body =
        "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}";
    HttpResponse r = http_call(cfg.host, cfg.httpPort, "POST", "/api/register", "", body);
    expect_http_ok(r, "register " + username);
}

HttpUser http_login(const Config& cfg, const std::string& username,
                    const std::string& password) {
    const std::string body =
        "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}";
    HttpResponse r = http_call(cfg.host, cfg.httpPort, "POST", "/api/login", "", body);
    expect_http_ok(r, "http login " + username);

    rapidjson::Document d = parse_json(r.body);
    if (!d.HasMember("data") || !d["data"].IsObject())
        throw std::runtime_error("login response missing data: " + r.body);

    HttpUser u;
    u.uid = json_uint(d["data"], "userId");
    u.token = json_string(d["data"], "token");
    if (u.uid == 0 || u.token.empty())
        throw std::runtime_error("login response missing userId/token: " + r.body);
    return u;
}

void tcp_login(tcp::socket& s, const HttpUser& user) {
    kc::Message ack = login(s, user.uid, user.token);
    if (ack.type != kc::MsgType::LoginAck)
        throw std::runtime_error("tcp login failed: " + ack.content);
}

void run_one_iteration(const Config& cfg, int threadIdx, int iter,
                       std::uint64_t runId,
                       std::atomic<std::uint64_t>& userSeq,
                       std::atomic<std::uint64_t>& msgSeq,
                       Stats& stats) {
    boost::asio::io_context io;
    tcp::socket connA(io), connB(io);

    try {
        const std::uint64_t seq = userSeq.fetch_add(1, std::memory_order_relaxed);
        const std::string userA = "stA_" + std::to_string(runId) + "_" + std::to_string(seq);
        const std::string userB = "stB_" + std::to_string(runId) + "_" + std::to_string(seq);
        const std::string password = "secret123";

        // 1) 注册 A/B，随后走 HTTP 登录拿 JWT。
        register_user(cfg, userA, password);
        register_user(cfg, userB, password);
        HttpUser a = http_login(cfg, userA, password);
        HttpUser b = http_login(cfg, userB, password);

        // 2) 双方建立 TCP 长连接并登录，用于接收好友事件和消息。
        if (!connect_with_retry(connA, cfg.host, cfg.tcpPort) ||
            !connect_with_retry(connB, cfg.host, cfg.tcpPort))
            throw std::runtime_error("tcp connect failed");
        set_recv_timeout(connA, 30);
        set_recv_timeout(connB, 30);
        tcp_login(connA, a);
        tcp_login(connB, b);

        // 3) A 发好友申请，B 实时收到 FriendRequest。
        HttpResponse reqResp = http_call(
            cfg.host, cfg.httpPort, "POST", "/api/friends/requests", a.token,
            "{\"toUserId\":" + std::to_string(b.uid) + ",\"message\":\"stress hello\"}");
        expect_http_ok(reqResp, "create friend request");

        rapidjson::Document reqDoc = parse_json(reqResp.body);
        if (!reqDoc.HasMember("data") || !reqDoc["data"].IsObject() ||
            !reqDoc["data"].HasMember("request") || !reqDoc["data"]["request"].IsObject())
            throw std::runtime_error("create friend request missing data.request: " + reqResp.body);
        const std::uint64_t requestId = json_uint(reqDoc["data"]["request"], "requestId");
        if (requestId == 0)
            throw std::runtime_error("create friend request missing requestId: " + reqResp.body);

        kc::Message reqEvent = wait_for_type(connB, kc::MsgType::FriendRequest);
        if (reqEvent.msg_id != requestId)
            throw std::runtime_error("FriendRequest requestId mismatch");

        // 4) B 通过申请，A 收到 accepted + 列表变更，B 收到列表变更。
        HttpResponse acceptResp = http_call(
            cfg.host, cfg.httpPort, "POST",
            "/api/friends/requests/" + std::to_string(requestId) + "/accept",
            b.token, "");
        expect_http_ok(acceptResp, "accept friend request");

        kc::Message resultEvent = wait_for_type(connA, kc::MsgType::FriendRequestResult);
        rapidjson::Document resultDoc;
        resultDoc.Parse(resultEvent.content.c_str(), resultEvent.content.size());
        if (json_string(resultDoc, "event") != "accepted")
            throw std::runtime_error("FriendRequestResult is not accepted: " + resultEvent.content);

        (void)wait_for_type(connA, kc::MsgType::FriendListChanged);
        kc::Message changedB = wait_for_type(connB, kc::MsgType::FriendListChanged);
        rapidjson::Document changedBDoc;
        changedBDoc.Parse(changedB.content.c_str(), changedB.content.size());
        if (json_string(changedBDoc, "action") != "added")
            throw std::runtime_error("B FriendListChanged is not added: " + changedB.content);

        // 5) A 通过 TCP 给 B 发一条消息，等待 SendAck 与 B 的 Text。
        const std::string text = "stress-" + std::to_string(threadIdx) + "-" +
                                 std::to_string(iter) + "-" + std::to_string(seq);
        const std::uint64_t msgId = msgSeq.fetch_add(1, std::memory_order_relaxed);
        (void)send_text(connA, a.uid, b.uid, msgId, text);

        kc::Message got = wait_for_type(connB, kc::MsgType::Text);
        if (got.content != text)
            throw std::runtime_error("received text mismatch");
        send_delivery_ack(connB, b.uid, got.conversation_id, got.seq);

        // 6) A 查询好友列表，确认 B 在列表中。
        HttpResponse listResp = http_call(
            cfg.host, cfg.httpPort, "GET", "/api/friends", a.token, "");
        expect_http_ok(listResp, "list friends");

        rapidjson::Document listDoc = parse_json(listResp.body);
        if (!listDoc.HasMember("data") || !listDoc["data"].IsObject())
            throw std::runtime_error("friend list response missing data: " + listResp.body);
        bool found = false;
        if (listDoc["data"].HasMember("friends") && listDoc["data"]["friends"].IsArray()) {
            for (const auto& f : listDoc["data"]["friends"].GetArray()) {
                if (json_uint(f, "userId") == b.uid) {
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            throw std::runtime_error("friend list does not contain B");

        // 7) A 删除好友，B 收到 deleted 列表变更。
        HttpResponse delResp = http_call(
            cfg.host, cfg.httpPort, "DELETE",
            "/api/friends/" + std::to_string(b.uid), a.token, "");
        expect_http_ok(delResp, "delete friend");

        kc::Message delEvent = wait_for_type(connB, kc::MsgType::FriendListChanged);
        rapidjson::Document delDoc;
        delDoc.Parse(delEvent.content.c_str(), delEvent.content.size());
        if (json_string(delDoc, "action") != "deleted")
            throw std::runtime_error("B FriendListChanged is not deleted: " + delEvent.content);

        stats.success.fetch_add(1, std::memory_order_relaxed);
    } catch (const std::exception& e) {
        stats.failure.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[stress] thread=" << threadIdx << " iter=" << iter
                  << " failed: " << e.what() << "\n";
    }
}

void print_usage(const char* prog) {
    std::cout
        << "用法: " << prog << " [http_port] [tcp_port] [threads] [loops]\n"
        << "选项:\n"
        << "  --host HOST       服务器地址（默认 127.0.0.1）\n"
        << "  --http-port PORT  HTTP 端口（默认 8080）\n"
        << "  --tcp-port PORT   TCP 端口（默认 8888）\n"
        << "  --threads N       并发线程数（默认 10）\n"
        << "  --loops N         每个线程循环次数（默认 1，0=一直循环到 Ctrl+C）\n"
        << "  --help            显示帮助\n";
}

int parse_non_negative(const std::string& text, const std::string& name) {
    try {
        const int v = std::stoi(text);
        if (v < 0) throw std::invalid_argument("negative");
        return v;
    } catch (...) {
        throw std::runtime_error("invalid " + name + ": " + text);
    }
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif

    Config cfg;
    std::vector<std::string> positional;

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto need_value = [&](const std::string& name) -> std::string {
                if (i + 1 >= argc)
                    throw std::runtime_error(name + " requires a value");
                return argv[++i];
            };

            if (arg == "--help" || arg == "-h") {
                print_usage(argv[0]);
                return 0;
            } else if (arg == "--host") {
                cfg.host = need_value(arg);
            } else if (arg == "--http-port") {
                cfg.httpPort = static_cast<unsigned short>(parse_non_negative(need_value(arg), arg));
            } else if (arg == "--tcp-port") {
                cfg.tcpPort = static_cast<unsigned short>(parse_non_negative(need_value(arg), arg));
            } else if (arg == "--threads") {
                cfg.threads = parse_non_negative(need_value(arg), arg);
            } else if (arg == "--loops") {
                cfg.loops = parse_non_negative(need_value(arg), arg);
            } else if (!arg.empty() && arg[0] == '-') {
                throw std::runtime_error("unknown option: " + arg);
            } else {
                positional.push_back(arg);
            }
        }

        if (positional.size() > 0)
            cfg.httpPort = static_cast<unsigned short>(parse_non_negative(positional[0], "http_port"));
        if (positional.size() > 1)
            cfg.tcpPort = static_cast<unsigned short>(parse_non_negative(positional[1], "tcp_port"));
        if (positional.size() > 2)
            cfg.threads = parse_non_negative(positional[2], "threads");
        if (positional.size() > 3)
            cfg.loops = parse_non_negative(positional[3], "loops");
        if (positional.size() > 4)
            throw std::runtime_error("too many positional arguments");

        if (cfg.threads <= 0)
            throw std::runtime_error("threads must be > 0");
    } catch (const std::exception& e) {
        std::cerr << "[stress] " << e.what() << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::uint64_t runId = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) % 1000000000ULL;

    std::atomic<std::uint64_t> userSeq{1};
    std::atomic<std::uint64_t> msgSeq{1};
    Stats stats;

    std::cout << "[stress] host=" << cfg.host
              << " http=" << cfg.httpPort
              << " tcp=" << cfg.tcpPort
              << " threads=" << cfg.threads
              << " loops=" << (cfg.loops == 0 ? std::string("infinite") : std::to_string(cfg.loops))
              << "\n";

    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(cfg.threads));
    for (int t = 0; t < cfg.threads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; !g_stop && (cfg.loops == 0 || i < cfg.loops); ++i) {
                run_one_iteration(cfg, t, i, runId, userSeq, msgSeq, stats);
            }
        });
    }
    for (auto& th : workers) th.join();

    const auto end = std::chrono::steady_clock::now();
    const double elapsed =
        std::chrono::duration<double>(end - start).count();

    const std::uint64_t success = stats.success.load();
    const std::uint64_t failure = stats.failure.load();

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "[stress] done: success=" << success
              << " failure=" << failure
              << " elapsed=" << elapsed << "s";
    if (elapsed > 0.0)
        std::cout << " throughput=" << (static_cast<double>(success) / elapsed) << "/s";
    std::cout << "\n";

    return failure == 0 ? 0 : 1;
}
