// ============================================================================
// test_send_msg_to_blockFriendAndBlackFriend.cpp：发消息关系校验（BE-SEND-01）验收客户端
// ----------------------------------------------------------------------------
// 对应服务端 session.cpp::do_send 新增的 areFriends 两段校验：
//   * 陌生人（无好友关系）        -> 拒绝，Error="not friends"，不落库；
//   * 被对方拉黑（reverse=2）     -> 拒绝，Error="blocked by recipient"；
//   * 自己拉黑对方后发送（forward=2）-> 同样被拒（当前文案与上一条相同，
//     因为拉黑删除了双向好友行，实现里 blocked=forward==2||reverse==2）；
//   * 正常好友                    -> 不受影响，SendAck + 实时投递。
//
// 覆盖场景：
//   1) 注册 A/B/C 并 HTTP 登录拿 JWT；A/B/C 建立 TCP 长连接并登录；
//   2) 陌生人校验：A -> C 发消息，A 收到 "not friends"，C 一无所收；
//   3) 好友基线：A 申请、B 通过，A -> B 发消息成功（SendAck + B 收到 Text）；
//   4) 被拉黑校验：B 拉黑 A 后 A -> B 被拒 "blocked by recipient"；
//      同场景下 B -> A 也被拒（拉黑同时切断了拉黑者自己的发送路径）；
//   5) 主动拉黑校验：B 取消拉黑 A，A 拉黑 B 后 A -> B 被拒。
//
// 依赖：服务器已启动（HTTP+TCP），数据库已导入 db/schema.sql。
// 用法：test_send_msg_to_blockFriendAndBlackFriend.exe [http_port] [tcp_port]
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
// 每条检查后短暂 sleep，给服务端把异步事件推完的余量。
// ---------------------------------------------------------------------------
int g_fail = 0;

void check(bool cond, const std::string& desc, const std::string& extra = "") {
    std::this_thread::sleep_for(std::chrono::seconds(1));
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
// 阻塞 socket 接收超时：防止服务端没推消息时测试永久卡死。
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
// 极简 HTTP/1.1 客户端（与 test_friend_client.cpp 相同）
// 服务器是"短连接 + Content-Length + 回完即关"，读满 EOF 即返回。
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

// ---------------------------------------------------------------------------
// TCP 帧协议（4 字节大端长度 + JSON body）
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

// ---------------------------------------------------------------------------
// 等待指定类型的下行消息，带超时：
//   * 好友事件（130/131/132）等非目标类型直接跳过并打印；
//   * 若中途收到非预期的 Error（目标不是 Error），立即抛异常方便定位；
//   * 超时抛 "timeout ..."。
// ---------------------------------------------------------------------------
kc::Message wait_type(tcp::socket& s, kc::MsgType type,
                      const std::string& what, int seconds = 8) {
    set_recv_timeout(s, seconds);
    for (;;) {
        kc::Message r;
        try {
            r = recv_msg(s);
        } catch (const std::exception&) {
            throw std::runtime_error("timeout waiting " + what);
        }
        if (r.type == type) return r;
        if (r.type == kc::MsgType::Error && type != kc::MsgType::Error)
            throw std::runtime_error("unexpected error while waiting " +
                                     what + ": " + r.content);
        std::cout << "  [skip] 非目标下行 type="
                  << static_cast<int>(r.type) << " content=" << r.content << "\n";
    }
}

// 断言一段时间内没有任何"消息投递"（Text/Image）下行：
// 用于验证被拒的消息"不落库、不投递"。
// 注意：拉黑/取消拉黑等操作会异步补发 FriendListChanged(132) 等好友事件，
// 它们不是聊天投递，属于正常噪声，跳过继续等；只有 Text/Image 才算失败。
bool expect_nothing(tcp::socket& s, const std::string& who, int seconds = 2) {
    set_recv_timeout(s, seconds);
    for (;;) {
        kc::Message m;
        try {
            m = recv_msg(s);
        } catch (const std::exception&) {
            set_recv_timeout(s, 8);      // 恢复后续等待用的超时
            return true;                  // 超时 = 没有收到任何投递
        }
        if (m.type == kc::MsgType::Text || m.type == kc::MsgType::Image) {
            std::cout << "  [unexpected] " << who << " 收到投递 type="
                      << static_cast<int>(m.type) << " content=" << m.content << "\n";
            return false;
        }
        std::cout << "  [skip] " << who << " 非投递下行(好友事件) type="
                  << static_cast<int>(m.type) << " content=" << m.content << "\n";
    }
}

// 发一条 Text 消息（msg_id 用递增号，避开服务端 client_msg_id 去重）。
std::uint64_t g_clientMsgId = 1;
void send_text(tcp::socket& s, std::uint64_t from, std::uint64_t to,
               const std::string& text) {
    kc::Message m;
    m.from    = from;
    m.to      = to;
    m.type    = kc::MsgType::Text;
    m.msg_id  = g_clientMsgId++;
    m.content = text;
    send_msg(s, m);
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

// 每 10 秒发一次心跳保活，防止空闲踢下线（与 test_friend_client 相同）。
std::thread start_heartbeat(tcp::socket& s, std::uint64_t uid,
                            std::atomic<bool>& stop) {
    return std::thread([&s, uid, &stop] {
        for (;;) {
            for (int i = 0; i < 100 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stop.load()) break;
            try {
                kc::Message hb;
                hb.from = uid;
                hb.type = kc::MsgType::Heartbeat;
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
        // ---- 0. 注册 A/B/C 三个用户：A 是发送方，B 是好友/拉黑方，C 是陌生人 ----
        const std::string suffix = std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() % 1000000);
        const std::string userA = "sendA_" + suffix;
        const std::string userB = "sendB_" + suffix;
        const std::string userC = "strangerC_" + suffix;   // 与 A 全程无好友关系
        const std::string password = "secret123";

        for (const auto& u : { userA, userB, userC }) {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/register", "",
                                       "{\"username\":\"" + u + "\",\"password\":\"" + password + "\"}");
            check(r.status == 200, "HTTP 注册用户 " + u, r.body);
        }

        std::uint64_t uidA = 0, uidB = 0, uidC = 0;
        std::string tokenA, tokenB, tokenC;
        auto http_login = [&](const std::string& user, std::uint64_t& uid,
                              std::string& token) {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/login", "",
                                       "{\"username\":\"" + user + "\",\"password\":\"" + password + "\"}");
            check(r.status == 200, "HTTP 登录 " + user, r.body);
            rapidjson::Document d = parse_json(r.body);
            uid   = json_uint(d["data"], "userId");
            token = json_string(d["data"], "token");
        };
        http_login(userA, uidA, tokenA);
        http_login(userB, uidB, tokenB);
        http_login(userC, uidC, tokenC);
        if (!uidA || !uidB || !uidC) {
            std::cerr << "[send-msg-test] login/register failed, abort\n";
            return 1;
        }

        // ---- 1. A/B/C 建立 TCP 长连接并登录 ----
        boost::asio::io_context io;
        tcp::socket connA(io), connB(io), connC(io);
        if (!connect_with_retry(connA, host, tcpPort) ||
            !connect_with_retry(connB, host, tcpPort) ||
            !connect_with_retry(connC, host, tcpPort)) {
            std::cerr << "[send-msg-test] tcp connect failed\n";
            return 1;
        }
        set_recv_timeout(connA, 5);
        set_recv_timeout(connB, 5);
        set_recv_timeout(connC, 5);

        check(login(connA, uidA, tokenA).type == kc::MsgType::LoginAck, "A TCP 登录成功");
        check(login(connB, uidB, tokenB).type == kc::MsgType::LoginAck, "B TCP 登录成功");
        check(login(connC, uidC, tokenC).type == kc::MsgType::LoginAck, "C TCP 登录成功");

        std::atomic<bool> stopA{false}, stopB{false}, stopC{false};
        std::thread hbA = start_heartbeat(connA, uidA, stopA);
        std::thread hbB = start_heartbeat(connB, uidB, stopB);
        std::thread hbC = start_heartbeat(connC, uidC, stopC);

        // ========================================================================
        // 场景 1：陌生人校验 —— A 与 C 从未建立好友关系
        // ============================================================================
        // 期望：A 收到 Error="not friends"；消息不落库、不投递，C 一无所收。
        std::cout << "\n[scenario 1] A -> 陌生人 C：应被 'not friends' 拒绝\n";
        send_text(connA, uidA, uidC, "hello stranger C");
        {
            kc::Message e = wait_type(connA, kc::MsgType::Error, "A 收到拒绝 Error");
            check(e.content == "not friends", "A 收到 not friends", e.content);
        }
        check(expect_nothing(connC, "C"), "陌生人 C 没有收到任何投递");

        // ========================================================================
        // 场景 2：好友基线 —— A 与 B 成为好友后，消息必须正常送达
        // ============================================================================
        // 目的：证明关系校验没有把合法链路一起拦掉（回归保护）。
        std::cout << "\n[scenario 2] A <-> B 建立好友，A -> B 应正常送达\n";
        std::uint64_t requestId = 0;
        {
            HttpResponse r = http_call(host, httpPort, "POST", "/api/friends/requests",
                                       tokenA,
                                       "{\"toUserId\":" + std::to_string(uidB) +
                                       ",\"message\":\"hi B\"}");
            check(r.status == 200, "A 发送好友申请", r.body);
            rapidjson::Document d = parse_json(r.body);
            requestId = json_uint(d["data"]["request"], "requestId");
        }
        {
            kc::Message ev = wait_type(connB, kc::MsgType::FriendRequest, "B 收到申请");
            check(ev.msg_id == requestId, "B 收到的申请 requestId 匹配");
        }
        {
            HttpResponse r = http_call(host, httpPort, "POST",
                                       "/api/friends/requests/" + std::to_string(requestId) + "/accept",
                                       tokenB, "");
            check(r.status == 200, "B 通过好友申请", r.body);
        }
        {
            // A 侧会先收到结果、再收到列表变更；wait_type 只等目标类型，其余跳过。
            kc::Message ev = wait_type(connA, kc::MsgType::FriendRequestResult, "A 收到结果");
            rapidjson::Document d;
            d.Parse(ev.content.c_str(), ev.content.size());
            check(json_string(d, "event") == "accepted", "A 收到 accepted 结果");
        }
        {
            kc::Message ev = wait_type(connB, kc::MsgType::FriendListChanged, "B 收到列表变更");
            rapidjson::Document d;
            d.Parse(ev.content.c_str(), ev.content.size());
            check(json_string(d, "action") == "added", "B 收到 added 列表变更");
        }

        // 好友关系生效：发送应拿到 SendAck("sent")，接收者实时收到 Text。
        send_text(connA, uidA, uidB, "hello friend B");
        {
            kc::Message ack = wait_type(connA, kc::MsgType::SendAck, "A 收到 SendAck");
            check(ack.content == "sent" && ack.msg_id != 0 && ack.seq != 0,
                  "A 收到 sent 回执（含 msg_id/seq）", ack.content);
        }
        {
            kc::Message m = wait_type(connB, kc::MsgType::Text, "B 收到 Text");
            check(m.from == uidA && m.content == "hello friend B", "B 实时收到消息", m.content);
        }

        // ========================================================================
        // 场景 3：被拉黑校验 —— B 拉黑 A 后，A -> B 必须被拒
        // ============================================================================
        // 拉黑会删除双向好友行、只留 (B,A,2)，所以：
        //   * A -> B：reverse=2 -> "blocked by recipient"，B 一无所收；
        //   * B -> A：好友行已双向删除，拉黑者自己也被拒（当前文案同为 blocked）。
        std::cout << "\n[scenario 3] B 拉黑 A：A -> B 与 B -> A 均应被拒\n";
        {
            HttpResponse r = http_call(host, httpPort, "POST",
                                       "/api/friends/" + std::to_string(uidA) + "/block",
                                       tokenB, "");
            check(r.status == 200, "B 拉黑 A", r.body);
        }
        send_text(connA, uidA, uidB, "after blocked");
        {
            kc::Message e = wait_type(connA, kc::MsgType::Error, "A 被拉黑拒绝");
            check(e.content == "blocked by recipient", "A->B 收到 blocked by recipient", e.content);
        }
        check(expect_nothing(connB, "B"), "被拒消息没有投递给 B");

        send_text(connB, uidB, uidA, "blocker to blocked");
        {
            kc::Message e = wait_type(connB, kc::MsgType::Error, "B->A 拒绝");
            // 注：当前实现 forward==2 也置 blocked，文案同"被对方拉黑"，
            // 若要区分"我拉黑了对方"可把 blocked 收紧为仅 reverse==2。
            check(e.content == "blocked by recipient", "B->A 同样被拒（blocked 文案）", e.content);
        }
        check(expect_nothing(connA, "A"), "B->A 被拒消息没有投递给 A");

        // ========================================================================
        // 场景 4：主动拉黑校验 —— 恢复原点后 A 拉黑 B，A -> B 也应被拒
        // ============================================================================
        // 前置：B 取消拉黑 A（此时双方已非好友，无需重新加好友）。
        {
            HttpResponse r = http_call(host, httpPort, "POST",
                                       "/api/friends/" + std::to_string(uidA) + "/unblock",
                                       tokenB, "");
            check(r.status == 200, "B 取消拉黑 A", r.body);
        }
        {
            HttpResponse r = http_call(host, httpPort, "POST",
                                       "/api/friends/" + std::to_string(uidB) + "/block",
                                       tokenA, "");
            check(r.status == 200, "A 拉黑 B", r.body);
        }
        send_text(connA, uidA, uidB, "I blocked you");
        {
            kc::Message e = wait_type(connA, kc::MsgType::Error, "A 拉黑他人后仍被拒");
            check(e.type == kc::MsgType::Error, "A 拉黑 B 后自己发送也被拒", e.content);
        }
        check(expect_nothing(connB, "B"), "A 拉黑 B 后的发送未投递给 B（B 只会有列表变更事件被跳过则说明没消息）");

        // ---- 收尾：清理心跳线程并关闭连接 ----
        stopA.store(true);
        stopB.store(true);
        stopC.store(true);
        if (hbA.joinable()) hbA.join();
        if (hbB.joinable()) hbB.join();
        if (hbC.joinable()) hbC.join();
        connA.close();
        connB.close();
        connC.close();
    } catch (const std::exception& e) {
        std::cerr << "[send-msg-test] error: " << e.what() << "\n";
        return 1;
    }

    if (g_fail == 0) {
        std::cout << "[send-msg-test] ALL PASS\n";
        return 0;
    }
    std::cout << "[send-msg-test] SOME FAILED (" << g_fail << ")\n";
    return 1;
}
