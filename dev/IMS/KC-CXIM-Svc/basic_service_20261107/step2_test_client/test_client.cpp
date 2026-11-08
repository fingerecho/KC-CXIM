#include <boost/asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "message.h"
#include "message_codec.h"

// ============================================================================
// 自动化验收客户端（数据库接入版）
// ----------------------------------------------------------------------------
// 覆盖以下场景：
//   1) 登录校验（查 users 表）；
//   2) A -> B 在线消息（服务端分配 msg_id/seq/ts）；
//   3) client_msg_id 去重（重复发送不产生第二条消息）；
//   4) B 离线消息落库，上线后补推，客户端 ACK 后删除；
//   5) 历史分页（HistoryReq / HistoryResp）；
//   6) 非法用户登录返回 Error；
//   7) 断线重连后重新登录并继续发送。
//
// 运行前需要先导入 step3_verify/seed_test_data.sql（verify_tcp.sh 会自动做）。
// 用法：test_client.exe [port]
// ============================================================================

using boost::asio::ip::tcp;

// ---------------------------------------------------------------------------
// 按 [4 字节网络序长度][JSON] 的分帧协议收发。
// ---------------------------------------------------------------------------
static void send_frame(tcp::socket& s, const std::string& msg) {
    std::uint32_t net_len = htonl(static_cast<std::uint32_t>(msg.size()));
    std::vector<char> out(4 + msg.size());
    std::memcpy(out.data(), &net_len, 4);
    std::memcpy(out.data() + 4, msg.data(), msg.size());
    boost::asio::write(s, boost::asio::buffer(out));
}

static std::string recv_frame(tcp::socket& s) {
    char header[4];
    boost::asio::read(s, boost::asio::buffer(header, 4));
    std::uint32_t net_len = 0;
    std::memcpy(&net_len, header, 4);
    std::uint32_t len = ntohl(net_len);

    std::vector<char> body(len);
    boost::asio::read(s, boost::asio::buffer(body));
    return std::string(body.begin(), body.end());
}

static void send_msg(tcp::socket& s, const kc::Message& m) {
    send_frame(s, kc::serialize_message(m));
}

static kc::Message recv_msg(tcp::socket& s) {
    std::string raw = recv_frame(s);
    std::vector<char> body(raw.begin(), raw.end());
    kc::Message m;
    std::string err;
    if (!kc::parse_message(body, m, err)) {
        throw std::runtime_error("bad server message: " + err);
    }
    return m;
}

// ---------------------------------------------------------------------------
// 连接（带重试，便于服务器刚启动时运行）。
// ---------------------------------------------------------------------------
static bool connect_with_retry(tcp::socket& s, const std::string& host,
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

// 发送一条文本，并等待服务端的 SendAck。
static kc::Message send_text(tcp::socket& s, std::uint64_t from, std::uint64_t to,
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
        if (r.type == kc::MsgType::Error) throw std::runtime_error("send error: " + r.content);
        // 其余类型忽略
    }
}

// 发送投递/已读回执。
static void send_ack(tcp::socket& s, std::uint64_t from,
                     std::uint64_t conversationId, std::uint64_t seq, bool read) {
    kc::Message m;
    m.from = from;
    m.type = kc::MsgType::Ack;
    m.conversation_id = conversationId;
    m.seq = seq;
    if (!read) m.content = "delivered";   // 只确认投递，不更新已读
    send_msg(s, m);
}

// 等指定类型的下行消息（跳过无关消息）。
static kc::Message wait_for_type(tcp::socket& s, kc::MsgType type) {
    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == type) return r;
        if (r.type == kc::MsgType::Error) throw std::runtime_error("server error: " + r.content);
    }
}

// 登录并等待 LoginAck / Error。
static kc::Message login(tcp::socket& s, std::uint64_t uid, const std::string& token) {
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

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    unsigned short port = 8888;
    if (argc > 1) port = static_cast<unsigned short>(std::stoi(argv[1]));

    try {
        boost::asio::io_context io;

        // ---- 1. A/B 登录 -------------------------------------------------
        tcp::socket a(io), b(io);
        if (!connect_with_retry(a, host, port) || !connect_with_retry(b, host, port)) {
            std::cerr << "[client] connect failed\n";
            return 1;
        }

        kc::Message ackA = login(a, 1001, "tokenA");
        std::cout << "A ack: " << ackA.content << "\n";
        if (ackA.type != kc::MsgType::LoginAck) return 1;

        kc::Message ackB = login(b, 1002, "tokenB");
        std::cout << "B ack: " << ackB.content << "\n";
        if (ackB.type != kc::MsgType::LoginAck) return 1;

        // ---- 2. A -> B 在线消息 -----------------------------------------
        kc::Message sendAck = send_text(a, 1001, 1002, 1001, "hi B");
        std::cout << "A SendAck: msg_id=" << sendAck.msg_id
                  << " seq=" << sendAck.seq << " status=" << sendAck.content << "\n";

        kc::Message got = wait_for_type(b, kc::MsgType::Text);
        std::cout << "B recv: " << got.content
                  << " (msg_id=" << got.msg_id << ", seq=" << got.seq << ")\n";
        send_ack(b, 1002, got.conversation_id, got.seq, true);
        // 给服务端一点时间处理 ACK（删除离线队列），避免后面关闭 B 时产生竞态。
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // ---- 3. 重复 client_msg_id 去重 ---------------------------------
        kc::Message dup = send_text(a, 1001, 1002, 1001, "hi B");
        if (dup.content == "duplicate") {
            std::cout << "duplicate ignored\n";
        } else {
            std::cout << "duplicate NOT ignored: " << dup.content << "\n";
        }

        // ---- 4. B 离线 -> 消息落库 -> B 上线补推 -------------------------
        b.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        kc::Message offAck = send_text(a, 1001, 1002, 1002, "offline-1");
        std::cout << "A SendAck(offline): msg_id=" << offAck.msg_id
                  << " seq=" << offAck.seq << "\n";

        b = tcp::socket(io);
        if (!connect_with_retry(b, host, port)) return 1;
        kc::Message bReAck = login(b, 1002, "tokenB");
        std::cout << "B ack(reconnect): " << bReAck.content << "\n";

        kc::Message offline = wait_for_type(b, kc::MsgType::Text);
        std::cout << "offline delivered: " << offline.content
                  << " (seq=" << offline.seq << ")\n";
        send_ack(b, 1002, offline.conversation_id, offline.seq, true);

        // ---- 5. 历史分页 --------------------------------------------------
        kc::Message hreq;
        hreq.from = 1001;
        hreq.to = 1002;
        hreq.type = kc::MsgType::HistoryReq;
        hreq.seq = 0;          // 0 = 从最新往前取
        hreq.content = "20";
        send_msg(a, hreq);

        kc::Message hresp = wait_for_type(a, kc::MsgType::HistoryResp);
        std::vector<kc::StoredMessage> history;
        std::string herr;
        if (!kc::parse_history(hresp.content, history, herr)) {
            std::cerr << "history parse failed: " << herr << "\n";
        }
        std::cout << "history count: " << history.size() << "\n";

        // ---- 6. 非法用户登录 ---------------------------------------------
        tcp::socket c(io);
        if (connect_with_retry(c, host, port)) {
            kc::Message bad = login(c, 9999, "whatever");
            if (bad.type == kc::MsgType::Error) {
                std::cout << "invalid user rejected: " << bad.content << "\n";
            } else {
                std::cout << "invalid user NOT rejected\n";
            }
            c.close();
        }

        // ---- 7. 断线重连 --------------------------------------------------
        tcp::socket r(io);
        if (connect_with_retry(r, host, port)) {
            kc::Message rAck = login(r, 1003, "tokenC");
            std::cout << "R ack: " << rAck.content << "\n";

            kc::Message r1 = send_text(r, 1003, 1002, 2001, "before-drop");
            std::cout << "A SendAck(reconnect-1): seq=" << r1.seq << "\n";

            // 模拟掉线
            r.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            r = tcp::socket(io);
            if (connect_with_retry(r, host, port)) {
                kc::Message r2Ack = login(r, 1003, "tokenC");
                std::cout << "R ack(reconnect): " << r2Ack.content << "\n";
                kc::Message r2 = send_text(r, 1003, 1002, 2002, "after-reconnect");
                std::cout << "A SendAck(reconnect-2): seq=" << r2.seq << "\n";
                std::cout << "after-reconnect\n";
                r.close();
            }
        }

        a.close();
        b.close();
    } catch (const std::exception& e) {
        std::cerr << "[client] error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
