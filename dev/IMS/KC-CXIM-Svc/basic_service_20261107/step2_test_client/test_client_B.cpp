#include <boost/asio.hpp>
#include <boost/system/system_error.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "message_codec.h"

#if defined(_WIN32)
#  include <windows.h>
#endif

// ============================================================================
// 交互式聊天客户端 B（数据库接入版）
// ----------------------------------------------------------------------------
// 与 test_client_A 对称：B = 1002，对方 A = 1001。
// 收到消息后自动回 Ack；支持 /history。
// ============================================================================

using boost::asio::ip::tcp;

static constexpr std::uint64_t MY_ID     = 1002;
static constexpr std::uint64_t PEER_ID   = 1001;
static constexpr const char*   MY_TOKEN  = "tokenB";
static constexpr const char*   MY_NAME   = "B";
static constexpr const char*   PEER_NAME = "A";
static constexpr int HEARTBEAT_SECONDS = 25;

#if defined(_WIN32)
static std::string console_to_utf8(const std::string& in) {
    if (in.empty()) return in;
    UINT cp = GetConsoleCP();
    if (cp == CP_UTF8) return in;

    int wlen = MultiByteToWideChar(cp, 0, in.data(), static_cast<int>(in.size()), nullptr, 0);
    if (wlen <= 0) return in;
    std::wstring w(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(cp, 0, in.data(), static_cast<int>(in.size()), w.data(), wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return in;
    std::string out(static_cast<std::size_t>(ulen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, out.data(), ulen, nullptr, nullptr);
    return out;
}
#else
static std::string console_to_utf8(const std::string& in) { return in; }
#endif

static void send_message(tcp::socket& s, const kc::Message& m) {
    std::string payload = kc::serialize_message(m);
    std::uint32_t net_len = htonl(static_cast<std::uint32_t>(payload.size()));
    std::vector<char> out(4 + payload.size());
    std::memcpy(out.data(), &net_len, 4);
    std::memcpy(out.data() + 4, payload.data(), payload.size());
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

static void receiver_loop(tcp::socket& s, std::atomic<bool>& running,
                          std::mutex& print_mtx, std::mutex& send_mtx) {
    try {
        while (running.load()) {
            std::string raw = recv_frame(s);
            std::vector<char> body(raw.begin(), raw.end());
            kc::Message msg;
            std::string err;
            if (!kc::parse_message(body, msg, err)) {
                std::lock_guard<std::mutex> lk(print_mtx);
                std::cout << "\n[recv] 消息解析失败: " << err << std::endl;
                continue;
            }

            {
                std::lock_guard<std::mutex> lk(print_mtx);
                switch (msg.type) {
                case kc::MsgType::Text:
                    std::cout << "\n[" << PEER_NAME << "] " << msg.content
                              << "  (seq=" << msg.seq << ")" << std::endl;
                    break;
                case kc::MsgType::Image:
                    std::cout << "\n[" << PEER_NAME << " 图片] " << msg.content << std::endl;
                    break;
                case kc::MsgType::LoginAck:
                    std::cout << "\n[system] 登录成功：" << msg.content << std::endl;
                    break;
                case kc::MsgType::SendAck:
                    std::cout << "\n[system] 已发送 msg_id=" << msg.msg_id
                              << " seq=" << msg.seq << " (" << msg.content << ")" << std::endl;
                    break;
                case kc::MsgType::Error:
                    std::cout << "\n[error] " << msg.content << std::endl;
                    break;
                case kc::MsgType::HistoryResp: {
                    std::vector<kc::StoredMessage> hist;
                    std::string herr;
                    if (kc::parse_history(msg.content, hist, herr))
                        std::cout << "\n[history] 共 " << hist.size() << " 条" << std::endl;
                    else
                        std::cout << "\n[history] 解析失败: " << herr << std::endl;
                    break;
                }
                default:
                    std::cout << "\n[recv type=" << static_cast<int>(msg.type)
                              << "] " << msg.content << std::endl;
                    break;
                }
            }

            if ((msg.type == kc::MsgType::Text || msg.type == kc::MsgType::Image)
                && msg.conversation_id != 0 && msg.seq != 0) {
                kc::Message ack;
                ack.from = MY_ID;
                ack.type = kc::MsgType::Ack;
                ack.conversation_id = msg.conversation_id;
                ack.seq = msg.seq;
                ack.content = "read";
                try {
                    std::lock_guard<std::mutex> lk(send_mtx);
                    send_message(s, ack);
                } catch (...) {
                    running.store(false);
                    break;
                }
            }
        }
    } catch (const boost::system::system_error& e) {
        if (running.load()) {
            std::lock_guard<std::mutex> lk(print_mtx);
            std::cerr << "\n[recv] 连接断开，错误码=" << e.code().value() << std::endl;
        }
        running.store(false);
    } catch (const std::exception&) {
        if (running.load()) {
            std::lock_guard<std::mutex> lk(print_mtx);
            std::cerr << "\n[recv] 连接异常" << std::endl;
        }
        running.store(false);
    }
}

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string host = "127.0.0.1";
    unsigned short port = 8888;
    if (argc > 1) port = static_cast<unsigned short>(std::stoi(argv[1]));

    try {
        boost::asio::io_context io;
        tcp::socket s(io);
        s.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

        std::atomic<bool> running{true};
        std::mutex print_mtx;
        std::mutex send_mtx;

        auto send_locked = [&](const kc::Message& m) {
            std::lock_guard<std::mutex> lk(send_mtx);
            send_message(s, m);
        };

        kc::Message login;
        login.from = MY_ID;
        login.type = kc::MsgType::Login;
        login.content = MY_TOKEN;
        send_locked(login);

        std::thread rx(receiver_loop, std::ref(s), std::ref(running),
                       std::ref(print_mtx), std::ref(send_mtx));

        std::thread hb([&] {
            int elapsed = 0;
            while (running.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!running.load()) break;
                if (++elapsed < HEARTBEAT_SECONDS) continue;
                elapsed = 0;

                kc::Message h;
                h.from = MY_ID;
                h.type = kc::MsgType::Heartbeat;
                try { send_locked(h); }
                catch (...) { running.store(false); break; }
            }
        });

        {
            std::lock_guard<std::mutex> lk(print_mtx);
            std::cout << "已连接。我是 " << MY_NAME << "(" << MY_ID << ")，对方是 "
                      << PEER_NAME << "(" << PEER_ID << ")。\n"
                      << "输入内容后按回车发送；/history 拉取历史；/quit 退出。\n";
        }

        std::uint64_t next_msg_id = 1;
        char buf[1024];
        while (running.load()) {
            {
                std::lock_guard<std::mutex> lk(print_mtx);
                std::cout << MY_NAME << "> " << std::flush;
            }

            int n = std::scanf(" %1023[^\n]", buf);
            if (n == EOF) break;
            if (n == 0) {
                int c;
                while ((c = std::getchar()) != '\n' && c != EOF) {}
                continue;
            }

            std::string line(buf);
            if (line == "/quit") break;
            if (line.empty()) continue;

            try {
                if (line == "/history") {
                    kc::Message req;
                    req.from = MY_ID;
                    req.to = PEER_ID;
                    req.type = kc::MsgType::HistoryReq;
                    req.seq = 0;
                    req.content = "20";
                    send_locked(req);
                    continue;
                }

                kc::Message m;
                m.from = MY_ID;
                m.to = PEER_ID;
                m.type = kc::MsgType::Text;
                m.msg_id = next_msg_id++;
                m.content = console_to_utf8(line);
                send_locked(m);
            } catch (const std::exception&) {
                std::lock_guard<std::mutex> lk(print_mtx);
                std::cerr << "\n[error] 发送失败，连接可能已断开" << std::endl;
                running.store(false);
                break;
            }
        }

        running.store(false);
        boost::system::error_code ignored;
        s.close(ignored);
        if (rx.joinable()) rx.join();
        if (hb.joinable()) hb.join();
    } catch (const std::exception& e) {
        std::cerr << "[client] error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
