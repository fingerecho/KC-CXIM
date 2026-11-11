// ============================================================================
// test_friend_client.cpp：好友模块（BE-FRIEND-01 ~ 06）验收客户端
// ----------------------------------------------------------------------------
// 这个客户端故意同时使用两种协议，以验证项目的最终分工：
//   * HTTP：注册、登录、搜索用户、加好友、通过/拒绝、好友列表、备注、删除、拉黑；
//   * TCP ：登录后被动接收好友事件（FriendRequest=130、
//           FriendRequestResult=131、FriendListChanged=132）。
//
// 覆盖场景：
//   1) 注册 A/B 并通过 HTTP 登录拿到 JWT；
//   2) A 搜索 B（BE-FRIEND-01）；
//   3) A 向 B 发好友申请，B 的 TCP 长连接实时收到 FriendRequest（BE-FRIEND-03/06）；
//   4) B 通过申请，A 收到 FriendRequestResult + FriendListChanged（BE-FRIEND-04/06）；
//   5) A 查询好友列表，能看到 B（BE-FRIEND-05）；
//   6) A 修改备注，列表里的 remark 生效（BE-FRIEND-05）；
//   7) A 删除好友，B 收到 FriendListChanged（BE-FRIEND-05/06）；
//   8) A 再次申请、B 拒绝，A 收到 rejected 结果（BE-FRIEND-03/04/06）；
//   9) A 拉黑/取消拉黑 B，接口可用（BE-FRIEND-05）。
//
// 依赖：服务器已启动，且数据库已导入 db/schema.sql（friend_requests 表存在）。
// 用法：test_friend_client.exe [http_port] [tcp_port]
//   默认：http=8080, tcp=8888
// ============================================================================

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#ifndef _WIN32
#include <sys/time.h>       // Linux/macOS 下 SO_RCVTIMEO 需要 timeval
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#include "message.h"
#include "message_codec.h"
#include "rapidjson/document.h"

using boost::asio::ip::tcp;

namespace {

// ---------------------------------------------------------------------------
// 简单断言：所有检查都打印 PASS/FAIL，最后根据 fail 数量决定退出码。
// ---------------------------------------------------------------------------
int g_fail = 0;

void check(bool cond, const std::string& desc, const std::string& extra = "") {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (cond) {
        std::cout << "  PASS: " << desc << "\n";
    } else {
        std::cout << "  FAIL: " << desc;
        if (!extra.empty()) std::cout << " (" << extra << ")";
        std::cout << "\n";
        ++g_fail;
    }
}

// ---------------------------------------------------------------------------
// 阻塞 socket 接收超时：防止服务端没推事件时测试永久卡死。
// Windows 用 DWORD 毫秒，Linux 用 timeval。
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// 极简 HTTP/1.1 客户端
// ----------------------------------------------------------------------------
// 服务器是“短连接 + Content-Length + 回完即关”，所以客户端只要：
//   1) 拼请求头（Host/Connection: close/Content-Length/Authorization）；
//   2) 写 socket；
//   3) 读到 EOF，再按 \r\n\r\n 拆出响应体。
// 这样不需要引入额外 HTTP 库，也能把注意力放在好友业务上。
// ---------------------------------------------------------------------------
struct HttpResponse {
    int status = 0;
    std::string body;
};

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

    // 状态行形如：HTTP/1.1 200 OK
    const std::size_t firstSpace = raw.find(' ');
    if (firstSpace != std::string::npos) {
        try { out.status = std::stoi(raw.substr(firstSpace + 1, 3)); }
        catch (...) { out.status = 0; }
    }
    return out;
}

// 解析 JSON 字符串响应体；失败直接抛异常，让测试尽早暴露协议问题。
rapidjson::Document parse_json(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject())
        throw std::runtime_error("bad json: " + text);
    return doc;
}

// ---------------------------------------------------------------------------
// TCP 帧协议（与 test_client.cpp 保持一致：4 字节大端长度 + JSON body）
// ---------------------------------------------------------------------------
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
                std::cerr << "[connect] failed: " << e.what() << "\n";
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

// 等待指定类型的下行消息；其他类型（例如好友列表变化）直接跳过。
kc::Message wait_for_type(tcp::socket& s, kc::MsgType type) {
    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == type) return r;
        if (r.type == kc::MsgType::Error)
            throw std::runtime_error("server error: " + r.content);
    }
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

// 每 10 秒给连接发一次心跳帧，直到 stop 变为 true。
// 心跳帧用协议里服务端能接受的最小合法帧；这里假设 MsgType::Heartbeat 存在。
// 若协议里没有 Heartbeat，换成服务端会忽略的任意合法帧，或改成向服务端确认的类型。
std::thread start_heartbeat(tcp::socket& s, std::uint64_t uid,
                            std::atomic<bool>& stop) {
    return std::thread([&s, uid, &stop] {
        for (;;) {
            // 分片睡眠，保证结束时能很快退出（100ms 一片，10 秒 = 100 片）
            for (int i = 0; i < 100 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stop.load()) break;
            try {
                kc::Message hb;
                hb.from = uid;
                hb.type = kc::MsgType::Heartbeat;   // ← 关键：换成你协议的心跳类型
                hb.content = "";
                send_msg(s, hb);
            } catch (const std::exception& e) {
                std::cerr << "  [heartbeat] send failed: " << e.what() << "\n";
                break;
            }
        }
    });
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string host = "127.0.0.1";
    unsigned short httpPort = 8080;
    unsigned short tcpPort = 8888;
    if (argc > 1) httpPort = static_cast<unsigned short>(std::stoi(argv[1]));
    if (argc > 2) tcpPort = static_cast<unsigned short>(std::stoi(argv[2]));

    try {
        // ---- 0. 注册并登录两个测试用户 -----------------------------------
        const std::string suffix = std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() % 1000000);
        const std::string userA = "friendA_" + suffix;
        const std::string userB = "friendB_" + suffix;
        const std::string password = "secret123";

        {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/register", "",
                                       "{\"username\":\"" + userA + "\",\"password\":\"" + password + "\"}");
            check(r.status == 200, "HTTP 注册用户A", r.body);
        }
        {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/register", "",
                                       "{\"username\":\"" + userB + "\",\"password\":\"" + password + "\"}");
            check(r.status == 200, "HTTP 注册用户B", r.body);
        }

        std::uint64_t uidA = 0, uidB = 0;
        std::string tokenA, tokenB;
        {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/login", "",
                                       "{\"username\":\"" + userA + "\",\"password\":\"" + password + "\"}");
            check(r.status == 200, "HTTP 登录用户A", r.body);
            rapidjson::Document d = parse_json(r.body);
            uidA = json_uint(d["data"], "userId");
            tokenA = json_string(d["data"], "token");
        }
        {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/login", "",
                                       "{\"username\":\"" + userB + "\",\"password\":\"" + password + "\"}");
            check(r.status == 200, "HTTP 登录用户B", r.body);
            rapidjson::Document d = parse_json(r.body);
            uidB = json_uint(d["data"], "userId");
            tokenB = json_string(d["data"], "token");
        }
        if (!uidA || !uidB || tokenA.empty() || tokenB.empty()) {
            std::cerr << "[friend-test] login/register failed, abort\n";
            return 1;
        }

        // ---- 1. 建立 TCP 长连接，准备接收实时好友事件 ---------------------
        boost::asio::io_context io;
        tcp::socket connA(io), connB(io);
        if (!connect_with_retry(connA, host, tcpPort) ||
            !connect_with_retry(connB, host, tcpPort)) {
            std::cerr << "[friend-test] tcp connect failed\n";
            return 1;
        }
        set_recv_timeout(connA, 5);
        set_recv_timeout(connB, 5);

        kc::Message ackA = login(connA, uidA, tokenA);
        check(ackA.type == kc::MsgType::LoginAck, "A TCP 登录成功", ackA.content);
        kc::Message ackB = login(connB, uidB, tokenB);
        check(ackB.type == kc::MsgType::LoginAck, "B TCP 登录成功", ackB.content);

        // ----1.1 登录之后设置每10秒心跳保活----------------------------------
        std::atomic<bool> stopA{false}, stopB{false};
        std::thread hbA = start_heartbeat(connA, uidA, stopA);
        std::thread hbB = start_heartbeat(connB, uidB, stopB);

        // ---- 2. BE-FRIEND-01：搜索用户 -----------------------------------
        {
            HttpResponse r = http_call(host, httpPort, "GET",
                                       "/api/users/search?keyword=" + userB,
                                       tokenA, "");
            check(r.status == 200, "A 搜索用户B", r.body);
            rapidjson::Document d = parse_json(r.body);
            bool found = false;
            if (d["data"].HasMember("users") && d["data"]["users"].IsArray()) {
                for (const auto& u : d["data"]["users"].GetArray()) {
                    if (json_uint(u, "userId") == uidB) { found = true; break; }
                }
            }
            check(found, "搜索结果包含用户B");
        }

        // ---- 3. BE-FRIEND-03/06：A 申请 B，B 实时收到 FriendRequest -------
        std::uint64_t requestId = 0;
        {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/friends/requests",
                                       tokenA,
                                       "{\"toUserId\":" + std::to_string(uidB) +
                                       ",\"message\":\"hello from A\"}");
            check(r.status == 200, "A 发送好友申请", r.body);
            rapidjson::Document d = parse_json(r.body);
            requestId = json_uint(d["data"]["request"], "requestId");
            check(requestId != 0, "返回 requestId");
        }
        {
            kc::Message ev = wait_for_type(connB, kc::MsgType::FriendRequest);
            rapidjson::Document d;
            d.Parse(ev.content.c_str(), ev.content.size());
            check(ev.msg_id == requestId, "B 收到 FriendRequest 且 requestId 匹配");
            check(json_uint(d, "requester_id") == uidA, "事件里的申请人正确");
        }

        // ---- 4. BE-FRIEND-04/06：B 通过，A 收到结果与列表变更 -------------
        {
            HttpResponse r = http_call(host, httpPort, "POST",
                                       "/api/friends/requests/" + std::to_string(requestId) + "/accept",
                                       tokenB, "");
            check(r.status == 200, "B 通过好友申请", r.body);
        }
        {
            kc::Message ev = wait_for_type(connA, kc::MsgType::FriendRequestResult);
            rapidjson::Document d;
            d.Parse(ev.content.c_str(), ev.content.size());
            check(json_string(d, "event") == "accepted", "A 收到 accepted 结果");
            kc::Message changed = wait_for_type(connA, kc::MsgType::FriendListChanged);
            (void)changed;
            check(true, "A 收到 FriendListChanged");

            // B 作为“通过申请”的一方也会收到 added 事件；这里主动消费掉，
            // 避免后面等待 deleted 事件时先读到这条 added。
            kc::Message changedB = wait_for_type(connB, kc::MsgType::FriendListChanged);
            rapidjson::Document db;
            db.Parse(changedB.content.c_str(), changedB.content.size());
            check(json_string(db, "action") == "added", "B 收到 added 列表变更");
        }

        // ---- 5. BE-FRIEND-05：好友列表 -----------------------------------
        {
            HttpResponse r = http_call(host, httpPort, "GET", "/api/friends", tokenA, "");
            check(r.status == 200, "A 查询好友列表", r.body);
            rapidjson::Document d = parse_json(r.body);
            bool found = false;
            if (d["data"].HasMember("friends") && d["data"]["friends"].IsArray()) {
                for (const auto& f : d["data"]["friends"].GetArray()) {
                    if (json_uint(f, "userId") == uidB) { found = true; break; }
                }
            }
            check(found, "好友列表包含 B");
        }

        // ---- 6. BE-FRIEND-05：修改备注 -----------------------------------
        {
            HttpResponse r = http_call(host, httpPort, "PATCH",
                                       "/api/friends/" + std::to_string(uidB),
                                       tokenA, "{\"remark\":\"B-remark\"}");
            check(r.status == 200, "A 修改 B 的备注", r.body);
        }
        {
            HttpResponse r = http_call(host, httpPort, "GET", "/api/friends", tokenA, "");
            rapidjson::Document d = parse_json(r.body);
            std::string remark;
            if (d["data"].HasMember("friends") && d["data"]["friends"].IsArray()) {
                for (const auto& f : d["data"]["friends"].GetArray()) {
                    if (json_uint(f, "userId") == uidB) {
                        remark = json_string(f, "remark");
                        break;
                    }
                }
            }
            check(remark == "B-remark", "好友列表里的备注已更新", remark);
        }

        // ---- 7. BE-FRIEND-05/06：删除好友，B 收到列表变更 -----------------
        {
            HttpResponse r = http_call(host, httpPort, "DELETE",
                                       "/api/friends/" + std::to_string(uidB),
                                       tokenA, "");
            check(r.status == 200, "A 删除好友 B", r.body);
        }
        {
            kc::Message ev = wait_for_type(connB, kc::MsgType::FriendListChanged);
            rapidjson::Document d;
            d.Parse(ev.content.c_str(), ev.content.size());
            check(json_string(d, "action") == "deleted", "B 收到 deleted 列表变更");
        }

        // ---- 8. 再次申请 -> B 拒绝 -> A 收到 rejected --------------------
        std::uint64_t requestId2 = 0;
        {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/friends/requests",
                                       tokenA,
                                       "{\"toUserId\":" + std::to_string(uidB) + "}");
            check(r.status == 200, "A 删除后再次申请 B", r.body);
            rapidjson::Document d = parse_json(r.body);
            requestId2 = json_uint(d["data"]["request"], "requestId");
        }
        {
            kc::Message ev = wait_for_type(connB, kc::MsgType::FriendRequest);
            check(ev.msg_id == requestId2, "B 再次收到 FriendRequest");
        }
        {
            HttpResponse r = http_call(host, httpPort, "POST",
                                       "/api/friends/requests/" + std::to_string(requestId2) + "/reject",
                                       tokenB, "");
            check(r.status == 200, "B 拒绝好友申请", r.body);
        }
        {
            kc::Message ev = wait_for_type(connA, kc::MsgType::FriendRequestResult);
            rapidjson::Document d;
            d.Parse(ev.content.c_str(), ev.content.size());
            check(json_string(d, "event") == "rejected", "A 收到 rejected 结果");
        }

        // ---- 9. BE-FRIEND-05：拉黑 / 取消拉黑 ----------------------------
        {
            HttpResponse r = http_call(host, httpPort, "POST",
                                       "/api/friends/" + std::to_string(uidB) + "/block",
                                       tokenA, "");
            check(r.status == 200, "A 拉黑 B", r.body);
        }
        {
            HttpResponse r = http_call(host, httpPort, "POST",
                                       "/api/friends/" + std::to_string(uidB) + "/unblock",
                                       tokenA, "");
            check(r.status == 200, "A 取消拉黑 B", r.body);
        }

        // ---- 10. 申请列表接口 --------------------------------------------
        {
            HttpResponse r = http_call(host, httpPort, "GET",
                                       "/api/friends/requests?direction=outgoing",
                                       tokenA, "");
            check(r.status == 200, "A 查询发出的申请列表", r.body);
        }

        stopA.store(true);
        stopB.store(true);
        if (hbA.joinable()) hbA.join();
        if (hbB.joinable()) hbB.join();
        connA.close();
        connB.close();
    } catch (const std::exception& e) {
        std::cerr << "[friend-test] error: " << e.what() << "\n";
        return 1;
    }

    if (g_fail == 0) {
        std::cout << "[friend-test] ALL PASS\n";
        return 0;
    }
    std::cout << "[friend-test] SOME FAILED (" << g_fail << ")\n";
    return 1;
}
