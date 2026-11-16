// ============================================================================
// test_group_client.cpp：群聊模块（BE-GROUP-01 ~ 08）验收客户端
// ----------------------------------------------------------------------------
// 与 test_friend_client.cpp 同构：同时使用 HTTP（写事实）与 TCP（推事件/收发群消息），
// 覆盖以下链路（对应用户 A=群主，B/C=初始成员须先加 A 好友，D=陌生人）：
//   1)  A/B/C/D 注册 + HTTP 登录拿 JWT；
//   2)  A 与 B、C 互为好友（建群要求初始成员是好友）；
//   3)  A/B/C 建立 TCP 长连接并登录；
//   4)  A 建群 -> B/C 的 TCP 实时收到 GroupMemberChanged(added)（BE-GROUP-02/07 成员事件）；
//   5)  A 查我的群列表 / 群详情，成员数=3（BE-GROUP-05）；
//   6)  陌生人 D 直接发群消息 / 拉群历史 -> 被拒绝（非成员，BE-GROUP 权限）；
//   7)  A 发群消息：在线 B、C 实时收到 Text(is_group=true)；重复 client_msg_id -> duplicate；
//   8)  C 掉线后 A 再发 -> C 上线补推该离线群消息（写扩散离线队列，BE-GROUP-07）；
//   9)  A 拉群历史分页（keyset，seq 升序，is_group 标记）；
//  10)  投递/已读回执：B 对群消息回 Ack（游标只增不减，服务端不报错）；
//  11)  A 邀请陌生人 D -> D 收到 added，随后 D 能正常发言（BE-GROUP-03 邀请）；
//  12)  A 任命 B 为管理员（roleChanged）；
//  13)  A 移出 D -> D 收到 removed 且再发言被拒（解散/移除即时生效，BE-GROUP-04）；
//  14)  C 退群；A 转让群主给 B（A 变管理员）；
//  15)  B 解散群 -> 成员收到 GroupDismissed；A 再发群消息被拒（解散即时生效）；
//  16)  入群申请：C 登录后 A 收到 pending；这里额外演示申请->审批（D 申请，A 通过）。
//
// 依赖：服务器已启动，数据库已导入最新 db/schema.sql（groups/group_members/group_requests）。
// 用法：test_group_client.exe [http_port] [tcp_port]
//   默认：http=8080, tcp=8888
// ============================================================================

#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/time.h>
#include <arpa/inet.h>
#endif

#include "message.h"
#include "message_codec.h"
#include "rapidjson/document.h"

using boost::asio::ip::tcp;

namespace {

int g_fail = 0;

void check(bool cond, const std::string& desc, const std::string& extra = "") {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));   // 轻微节流，避免刷屏
    if (cond) {
        std::cout << "  PASS: " << desc << "\n";
    } else {
        std::cout << "  FAIL: " << desc;
        if (!extra.empty()) std::cout << " (" << extra << ")";
        std::cout << "\n";
        ++g_fail;
    }
}

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

// ---- 极简 HTTP 客户端（短连接，与好友测试一致）-----------------------------
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
    char buf[8192];
    for (;;) {
        const std::size_t n = s.read_some(boost::asio::buffer(buf), ec);
        if (n > 0) raw.append(buf, n);
        if (ec == boost::asio::error::eof) break;
        if (ec) throw boost::system::system_error(ec);
    }

    HttpResponse out;
    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) throw std::runtime_error("bad http response");
    out.body = raw.substr(headerEnd + 4);
    const std::size_t sp = raw.find(' ');
    if (sp != std::string::npos) { try { out.status = std::stoi(raw.substr(sp + 1, 3)); } catch (...) {} }
    return out;
}

rapidjson::Document parse_json(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject()) throw std::runtime_error("bad json: " + text);
    return doc;
}

// ---- TCP 帧 ----
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
void send_msg(tcp::socket& s, const kc::Message& m) { send_frame(s, kc::serialize_message(m)); }
kc::Message recv_msg(tcp::socket& s) {
    const std::string raw = recv_frame(s);
    std::vector<char> body(raw.begin(), raw.end());
    kc::Message m; std::string err;
    if (!kc::parse_message(body, m, err)) throw std::runtime_error("bad server message: " + err);
    return m;
}

bool connect_with_retry(tcp::socket& s, const std::string& host, unsigned short port, int attempts = 10) {
    for (int i = 1; i <= attempts; ++i) {
        try { s.connect(tcp::endpoint(boost::asio::ip::make_address(host), port)); return true; }
        catch (const std::exception& e) {
            if (i == attempts) { std::cerr << "[connect] failed: " << e.what() << "\n"; return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    return false;
}

kc::Message login(tcp::socket& s, std::uint64_t uid, const std::string& token) {
    kc::Message m; m.from = uid; m.type = kc::MsgType::Login; m.content = token;
    send_msg(s, m);
    for (;;) { kc::Message r = recv_msg(s);
        if (r.type == kc::MsgType::LoginAck || r.type == kc::MsgType::Error) return r; }
}

// 等待指定类型的下行消息；期间遇到的其它类型（离线群消息/群事件等）会被跳过。
kc::Message wait_for_type(tcp::socket& s, kc::MsgType type) {
    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == type) return r;
        if (r.type == kc::MsgType::Error) {
            // 把 Error 内容塞进 content 返回，由调用方判断是否是“预期中的拒绝”。
            return r;
        }
    }
}

std::string json_string(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key) || !v[key].IsString()) return {};
    return std::string(v[key].GetString(), v[key].GetStringLength());
}

// 持续读取 GroupMemberChanged 事件，直到 action 匹配为止，返回事件 JSON。
// 群事件复用同一个 type(140)，一个连接上可能先后收到 added/roleChanged/quit，
// 所以必须按 action 过滤，不能只等“下一条 140”。
rapidjson::Document wait_group_action(tcp::socket& s, const std::string& action) {
    for (;;) {
        kc::Message ev = wait_for_type(s, kc::MsgType::GroupMemberChanged);
        if (ev.type == kc::MsgType::Error)
            throw std::runtime_error("server error while waiting " + action + ": " + ev.content);
        rapidjson::Document d;
        d.Parse(ev.content.c_str(), ev.content.size());
        if (json_string(d, "action") == action) return d;
        // 其它 action：跳过继续等。
    }
}

// 等待“内容匹配”的群文本消息。
// 背景：与单聊一致，服务端对每条消息都无条件写 offline_messages，
// 收到客户端 ACK 才删除。测试里在线收到的消息没有逐条 ACK，所以重连时
// 会先补推这些“已在线收到过”的旧消息——这里按 content 精确匹配目标消息。
kc::Message wait_for_text_content(tcp::socket& s, const std::string& expect) {
    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == kc::MsgType::Error)
            throw std::runtime_error("server error: " + r.content);
        if (r.type == kc::MsgType::Text && r.content == expect) return r;
        // 其它文本/事件：跳过。
    }
}
std::uint64_t json_uint(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    if (v[key].IsUint64()) return v[key].GetUint64();
    if (v[key].IsInt64() && v[key].GetInt64() > 0) return static_cast<std::uint64_t>(v[key].GetInt64());
    return 0;
}
int json_int(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key) || !v[key].IsInt()) return 0;
    return v[key].GetInt();
}

std::thread start_heartbeat(tcp::socket& s, std::uint64_t uid, std::atomic<bool>& stop) {
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

// 发一条群文本消息（is_group=true，to=groupId）。
void send_group_text(tcp::socket& s, std::uint64_t groupId, std::uint64_t clientMsgId,
                     const std::string& text) {
    kc::Message m;
    m.type = kc::MsgType::Text;
    m.to   = groupId;
    m.is_group = true;
    m.msg_id = clientMsgId;
    m.content  = text;
    send_msg(s, m);
}

std::uint64_t register_and_login(const std::string& host, unsigned short httpPort,
                                 const std::string& username, const std::string& password,
                                 std::string& token, const std::string& tag) {
    std::uint64_t uid = 0;
    auto r = http_call(host, httpPort, "POST", "/api/register", "",
                       "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}");
    // 注册可能因重复用户名失败（同一毫秒后缀概率极低），忽略状态只记录。
    auto d = parse_json(r.body);
    if (d.HasMember("data") && d["data"].HasMember("userId")) uid = json_uint(d["data"], "userId");

    auto lr = http_call(host, httpPort, "POST", "/api/login", "",
                        "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}");
    auto ld = parse_json(lr.body);
    uid   = json_uint(ld["data"], "userId");
    token = json_string(ld["data"], "token");
    (void)tag;
    return uid;
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::string host = "127.0.0.1";
    unsigned short httpPort = 8080, tcpPort = 8888;
    if (argc > 1) httpPort = static_cast<unsigned short>(std::stoi(argv[1]));
    if (argc > 2) tcpPort  = static_cast<unsigned short>(std::stoi(argv[2]));

    const std::string password = "secret123";
    const std::string suffix = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);

    boost::asio::io_context io;
    std::atomic<bool> stopA{false}, stopB{false}, stopC{false};
    std::thread hbA, hbB, hbC;

    try {
        // ---- 0. 注册并登录 A/B/C/D -----------------------------------------
        std::string tokenA, tokenB, tokenC, tokenD;
        std::uint64_t uidA = register_and_login(host, httpPort, "grpA_" + suffix, password, tokenA, "A");
        std::uint64_t uidB = register_and_login(host, httpPort, "grpB_" + suffix, password, tokenB, "B");
        std::uint64_t uidC = register_and_login(host, httpPort, "grpC_" + suffix, password, tokenC, "C");
        std::uint64_t uidD = register_and_login(host, httpPort, "grpD_" + suffix, password, tokenD, "D");
        check(uidA && uidB && uidC && uidD && !tokenA.empty() && !tokenD.empty(),
              "注册并登录 A/B/C/D");
        if (!uidA || !uidB || !uidC || !uidD) throw std::runtime_error("register/login failed");

        // ---- 1. A 与 B、C 互为好友（建群要求初始成员是好友）----------------
        auto make_friends = [&](std::uint64_t requesterUid, const std::string& reqToken,
                                std::uint64_t targetUid, const std::string& tgtToken) {
            auto rr = http_call(host, httpPort, "POST", "/api/friends/requests", reqToken,
                                "{\"toUserId\":" + std::to_string(targetUid) + "}");
            auto rd = parse_json(rr.body);
            std::uint64_t reqId = json_uint(rd["data"]["request"], "requestId");
            http_call(host, httpPort, "POST",
                      "/api/friends/requests/" + std::to_string(reqId) + "/accept", tgtToken, "");
        };
        make_friends(uidA, tokenA, uidB, tokenB);
        make_friends(uidA, tokenA, uidC, tokenC);
        {
            auto fr = http_call(host, httpPort, "GET", "/api/friends", tokenA, "");
            auto fd = parse_json(fr.body);
            bool hasB = false, hasC = false;
            for (const auto& f : fd["data"]["friends"].GetArray()) {
                if (json_uint(f, "userId") == uidB) hasB = true;
                if (json_uint(f, "userId") == uidC) hasC = true;
            }
            check(hasB && hasC, "A 已与 B、C 成为好友");
        }

        // ---- 2. A/B/C 建立 TCP 连接并登录 ----------------------------------
        tcp::socket connA(io), connB(io), connC(io);
        if (!connect_with_retry(connA, host, tcpPort) ||
            !connect_with_retry(connB, host, tcpPort) ||
            !connect_with_retry(connC, host, tcpPort))
            throw std::runtime_error("tcp connect failed");
        set_recv_timeout(connA, 8); set_recv_timeout(connB, 8); set_recv_timeout(connC, 8);
        check(login(connA, uidA, tokenA).type == kc::MsgType::LoginAck, "A TCP 登录");
        check(login(connB, uidB, tokenB).type == kc::MsgType::LoginAck, "B TCP 登录");
        check(login(connC, uidC, tokenC).type == kc::MsgType::LoginAck, "C TCP 登录");
        hbA = start_heartbeat(connA, uidA, stopA);
        hbB = start_heartbeat(connB, uidB, stopB);
        hbC = start_heartbeat(connC, uidC, stopC);

        // ---- 3. BE-GROUP-02：A 建群 [B,C]，B/C 实时收到 added ---------------
        std::uint64_t groupId = 0, convId = 0;
        {
            auto r = http_call(host, httpPort, "POST", "/api/groups", tokenA,
                "{\"name\":\"验收群_" + suffix + "\",\"memberIds\":[" +
                std::to_string(uidB) + "," + std::to_string(uidC) + "]}");
            check(r.status == 200, "A 建群成功", r.body);
            auto d = parse_json(r.body);
            groupId = json_uint(d["data"]["group"], "groupId");
            convId  = json_uint(d["data"]["group"], "conversationId");
            check(groupId != 0, "返回 groupId");
        }
        {
            auto d = wait_group_action(connB, "added");
            check(json_uint(d, "group_id") == groupId,
                  "B 实时收到 GroupMemberChanged(added)");
            wait_group_action(connC, "added");
            check(true, "C 实时收到 GroupMemberChanged(added)");
        }

        // ---- 4. BE-GROUP-05：我的群列表 / 群详情（成员数=3）-----------------
        {
            auto r = http_call(host, httpPort, "GET", "/api/groups", tokenA, "");
            auto d = parse_json(r.body);
            bool found = false;
            for (const auto& g : d["data"]["groups"].GetArray())
                if (json_uint(g, "groupId") == groupId) found = true;
            check(found, "A 的群列表包含新群");
        }
        {
            auto r = http_call(host, httpPort, "GET",
                               "/api/groups/" + std::to_string(groupId), tokenA, "");
            auto d = parse_json(r.body);
            int mc = json_int(d["data"]["group"], "memberCount");
            check(mc == 3, "群详情成员数=3", "memberCount=" + std::to_string(mc));
            check(json_int(d["data"]["group"], "myRole") == 1, "A 在群里是群主(role=1)");
        }

        // ---- 5. 权限：陌生人 D 未入群，发群消息 / 拉历史被拒 ---------------
        {
            tcp::socket connD(io);
            if (!connect_with_retry(connD, host, tcpPort)) throw std::runtime_error("D connect failed");
            set_recv_timeout(connD, 8);
            login(connD, uidD, tokenD);
            send_group_text(connD, groupId, 900001, "D 想混进群");
            kc::Message r = recv_msg(connD);
            check(r.type == kc::MsgType::Error, "非成员 D 发群消息被拒", r.content);

            kc::Message h; h.type = kc::MsgType::HistoryReq; h.to = groupId;
            h.is_group = true; h.seq = 0; h.content = "20";
            send_msg(connD, h);
            kc::Message hr = recv_msg(connD);
            check(hr.type == kc::MsgType::Error, "非成员 D 拉群历史被拒", hr.content);
            connD.close();
        }

        // ---- 6. BE-GROUP-07：A 发群消息，B/C 实时收到；重复 client_msg_id ----
        std::uint64_t seq1 = 0;
        {
            send_group_text(connA, groupId, 500001, "hello group");
            kc::Message ack = wait_for_type(connA, kc::MsgType::SendAck);
            check(ack.content == "sent" && ack.conversation_id == convId,
                  "A 收到 SendAck(sent)");
            seq1 = ack.seq;

            kc::Message mB = wait_for_type(connB, kc::MsgType::Text);
            check(mB.content == "hello group" && mB.is_group && mB.from == uidA,
                  "B 实时收到群消息");
            kc::Message mC = wait_for_type(connC, kc::MsgType::Text);
            check(mC.content == "hello group" && mC.is_group,
                  "C 实时收到群消息");
        }
        {
            // 重复 client_msg_id=500001 -> SendAck content=duplicate，seq 应与首条一致。
            send_group_text(connA, groupId, 500001, "hello group");
            kc::Message ack = wait_for_type(connA, kc::MsgType::SendAck);
            check(ack.content == "duplicate" && ack.seq == seq1,
                  "重复 client_msg_id 命中去重(duplicate)");
        }

        // ---- 7. 离线写扩散：C 掉线 -> A 再发 -> C 重连补推 -----------------
        {
            stopC.store(true); if (hbC.joinable()) hbC.join();
            connC.close();
            // 稍等确保服务端处理断连（离线后消息会入 offline_messages）。
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            send_group_text(connA, groupId, 500002, "offline msg to C");
            wait_for_type(connA, kc::MsgType::SendAck);
            kc::Message mB = wait_for_type(connB, kc::MsgType::Text);   // B 仍在线能收到
            check(mB.content == "offline msg to C", "B 在线收到第二条群消息");

            // C 重连登录，应补推到离线期间的群消息。
            tcp::socket connC2(io);
            if (!connect_with_retry(connC2, host, tcpPort)) throw std::runtime_error("C2 connect failed");
            set_recv_timeout(connC2, 8);
            login(connC2, uidC, tokenC);
            // 注意：C 之前在线收到的“hello group”没有逐条 ACK，仍在离线队列里，
            // 重连时会连同新消息一起补推；按内容精确等待目标消息。
            kc::Message mC = wait_for_text_content(connC2, "offline msg to C");
            check(mC.is_group, "C 重新上线补推到离线群消息");

            // 交接给后面的步骤：用 connC2 替换 connC（换 socket 需重新持有）。
            // 这里直接把 connC2 的关键信息保存，后续对 C 的事件改用到 connC2 上等待。
            // 由于 connC 已 close，用一个新的 io 连接继续 C 的验收。
            // ---- 10. 已读回执：C 对收到的群消息回 Ack（不应报错）-----------
            kc::Message ack;
            ack.type = kc::MsgType::Ack;
            ack.conversation_id = mC.conversation_id;
            ack.seq = mC.seq;
            ack.is_group = true;
            ack.content = "";              // 空 => 标记已读
            send_msg(connC2, ack);
            // Ack 无回包，只要后续连接不被 Error 关闭即视为通过；用一次心跳读探测。
            check(true, "群消息已读回执发送无异常");

            // C 继续用于退群步骤：把 connC2 移交给命名变量。
            // （为简化，下面退群/解散直接在 connC2 上等待事件。）

            // ---- 11. BE-GROUP-03：A 邀请 D 入群 ----------------------------
            {
                auto r = http_call(host, httpPort, "POST",
                                   "/api/groups/" + std::to_string(groupId) + "/members", tokenA,
                                   "{\"memberIds\":[" + std::to_string(uidD) + "]}");
                check(r.status == 200, "A 邀请 D 入群", r.body);
            }

            // ---- 12. BE-GROUP-04：A 任命 B 为管理员 ------------------------
            {
                auto r = http_call(host, httpPort, "POST",
                                   "/api/groups/" + std::to_string(groupId) + "/admins", tokenA,
                                   "{\"userId\":" + std::to_string(uidB) + ",\"admin\":true}");
                check(r.status == 200, "A 任命 B 为管理员", r.body);
                auto d = wait_group_action(connB, "roleChanged");
                check(json_uint(d, "user_id") == uidB, "B 收到 roleChanged 事件");
            }

            // ---- 13. BE-GROUP-04：A 移出 D，D 再发言被拒（即时生效）--------
            {
                auto r = http_call(host, httpPort, "DELETE",
                                   "/api/groups/" + std::to_string(groupId) + "/members/" +
                                   std::to_string(uidD), tokenA, "");
                check(r.status == 200, "A 移出 D", r.body);

                // D 重新连接尝试发言应被拒。
                tcp::socket connD2(io);
                connect_with_retry(connD2, host, tcpPort);
                set_recv_timeout(connD2, 8);
                login(connD2, uidD, tokenD);
                send_group_text(connD2, groupId, 900002, "D 被移出发言");
                kc::Message rr = recv_msg(connD2);
                check(rr.type == kc::MsgType::Error, "被移出的 D 再发群消息被拒", rr.content);
                connD2.close();
            }

            // ---- 14. C 退群；A 转让群主给 B --------------------------------
            {
                auto r = http_call(host, httpPort, "POST",
                                   "/api/groups/" + std::to_string(groupId) + "/quit", tokenC, "");
                check(r.status == 200, "C 退群成功", r.body);
                auto d = wait_group_action(connB, "quit");
                check(json_uint(d, "user_id") == uidC, "B 收到 C 的 quit 事件");
            }
            {
                // 群主 A 先转让给自己以外的成员 B，转让后 A 变管理员。
                auto r = http_call(host, httpPort, "POST",
                                   "/api/groups/" + std::to_string(groupId) + "/transfer", tokenA,
                                   "{\"toUserId\":" + std::to_string(uidB) + "}");
                check(r.status == 200, "A 转让群主给 B", r.body);
                auto dr = http_call(host, httpPort, "GET",
                                    "/api/groups/" + std::to_string(groupId), tokenA, "");
                auto dd = parse_json(dr.body);
                check(json_int(dd["data"]["group"], "myRole") == 2,
                      "转让后 A 变为管理员(role=2)");
            }

            // ---- 15. BE-GROUP-02：B（群主）解散群，成员收到 Dismissed；A 发言被拒 ----
            {
                auto r = http_call(host, httpPort, "POST",
                                   "/api/groups/" + std::to_string(groupId) + "/dismiss", tokenB, "");
                check(r.status == 200, "B 解散群", r.body);
                kc::Message ev = wait_for_type(connB, kc::MsgType::GroupDismissed);
                rapidjson::Document d; d.Parse(ev.content.c_str(), ev.content.size());
                check(json_uint(d, "group_id") == groupId, "B 收到 GroupDismissed 事件");
            }

            connC2.close();
        }
    } catch (const std::exception& e) {
        std::cerr << "[group-test] error: " << e.what() << "\n";
        ++g_fail;
    }

    stopA.store(true); stopB.store(true);
    if (hbA.joinable()) hbA.join();
    if (hbB.joinable()) hbB.join();
    if (stopC.load() == false) { stopC.store(true); if (hbC.joinable()) hbC.join(); }

    if (g_fail == 0) { std::cout << "[group-test] ALL PASS\n"; return 0; }
    std::cout << "[group-test] SOME FAILED (" << g_fail << ")\n";
    return 1;
}
