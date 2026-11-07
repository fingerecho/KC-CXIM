#include <boost/asio.hpp>
#include <iostream>
#include <vector>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>

// ============================================================================
// 测试客户端
// ----------------------------------------------------------------------------
// 作用：用阻塞式 API 主动连服务器，发几条消息，验证：
//   1) 粘包 / 半包（老用例，已通过 if(0) 关掉）；
//   2) 两个用户登录，A 能发给 B（证明服务器能按 userId 找到连接）。
//
// 这里用的是 Boost.Asio 的"同步"接口（write/read），逻辑直观，适合入门。
// ============================================================================

using boost::asio::ip::tcp;

// ---------------------------------------------------------------------------
// send_frame：按协议发送一条消息。
// 协议：[4 字节网络序长度][消息体字节]
// ---------------------------------------------------------------------------
static void send_frame(tcp::socket& s, const std::string& msg) {
    // htonl：把本机字节序的长度转成网络字节序（大端）。
    std::uint32_t net_len = htonl(static_cast<std::uint32_t>(msg.size()));

    // 准备一整块连续缓冲：前 4 字节放长度，后面放消息体。
    std::vector<char> out(4 + msg.size());
    std::memcpy(out.data(), &net_len, 4);
    std::memcpy(out.data() + 4, msg.data(), msg.size());

    // boost::asio::write 是同步写，保证整块数据都写到内核发送缓冲区才返回。
    boost::asio::write(s, boost::asio::buffer(out));
}

// ---------------------------------------------------------------------------
// recv_frame：按协议接收一条消息。
// 先读固定 4 字节长度头，再按长度读 body，最后转成 std::string 返回。
// ---------------------------------------------------------------------------
static std::string recv_frame(tcp::socket& s) {
    char header[4];
    // 同步读满 4 字节头（内部会循环，直到读满或出错）。
    boost::asio::read(s, boost::asio::buffer(header, 4));

    std::uint32_t net_len = 0;
    std::memcpy(&net_len, header, 4);
    std::uint32_t len = ntohl(net_len);   // 网络字节序 -> 本机字节序

    std::vector<char> body(len);
    boost::asio::read(s, boost::asio::buffer(body));   // 读满 body
    return std::string(body.begin(), body.end());
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    host = "v0.fyping.cn";
    host = "110.40.206.253";
    unsigned short port = 8888;

    // 用法：test_client.exe [port]
    if (argc > 1) port = static_cast<unsigned short>(std::stoi(argv[1]));

    try {
        boost::asio::io_context io;   // 同步接口也需要 io_context（socket 的宿主）
        tcp::socket s(io);

        // make_address：把 "127.0.0.1" 解析成 IP 地址对象（比 from_string 更安全）。
        s.connect(tcp::endpoint(boost::asio::ip::make_address(host), port));

        // ------------------------------------------------------------------
        // 以下两段是"旧协议（纯文本）"的测试，用 if(0) 暂时关闭。
        // 因为新服务器要求 JSON，发 "hello" 会被判定协议错误并断开。
        // ------------------------------------------------------------------
        if (0) {
            // 用例 1：正常单条
            send_frame(s, "hello");
            std::cout << "[case1] echo = " << recv_frame(s) << "\n";

            // 用例 2：粘包——两个完整包连续写入
            {
                std::string a = "AAA", b = "BBBB";
                std::vector<char> out;
                auto append = [&out](const std::string& m) {
                    std::uint32_t n = htonl(static_cast<std::uint32_t>(m.size()));
                    out.insert(out.end(), reinterpret_cast<char*>(&n),
                               reinterpret_cast<char*>(&n) + 4);
                    out.insert(out.end(), m.begin(), m.end());
                };
                append(a); append(b);
                boost::asio::write(s, boost::asio::buffer(out));
                std::cout << "[case2] echo1 = " << recv_frame(s) << "\n";
                std::cout << "[case2] echo2 = " << recv_frame(s) << "\n";
            }

            // 用例 3：半包——分两次写入，中间 sleep
            {
                std::string m = "half-packet";
                std::uint32_t n = htonl(static_cast<std::uint32_t>(m.size()));
                char head[4];
                std::memcpy(head, &n, 4);
                boost::asio::write(s, boost::asio::buffer(head, 2)); // 先发 2 字节头
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                std::vector<char> rest;
                rest.insert(rest.end(), head + 2, head + 4);         // 剩余 2 字节头
                rest.insert(rest.end(), m.begin(), m.end());         // 消息体
                boost::asio::write(s, boost::asio::buffer(rest));
                std::cout << "[case3] echo = " << recv_frame(s) << "\n";
            }

            // 用例 4：非法长度（声明 5000 字节，超过 MAX_BODY）
            {
                std::uint32_t bad = htonl(5000);
                boost::asio::write(s, boost::asio::buffer(&bad, 4));
                boost::system::error_code ec;
                char tmp[4];
                boost::asio::read(s, boost::asio::buffer(tmp, 4), ec);
                std::cout << "[case4] server closed? " << (ec ? "yes" : "no") << "\n";
            }
        }

        // ------------------------------------------------------------------
        // 旧协议下的 JSON 用例（同样暂时关闭）。
        // ------------------------------------------------------------------
        if (0) {
            // 用例 5：一条文本私聊
            send_frame(s, R"({"from":1001,"to":1002,"type":1,"msg_id":1,"ts":1730000000000,"content":"hello"})");
            std::cout << "[case1] echo = " << recv_frame(s) << "\n";

            // 用例 6：粘包——两条结构化消息连续写
            {
                std::string a = R"({"from":1001,"to":1002,"type":1,"msg_id":2,"content":"AAA"})";
                std::string b = R"({"from":1002,"to":1001,"type":2,"msg_id":3,"content":"http://x/a.png"})";
                std::vector<char> out;
                auto append = [&out](const std::string& m) {
                    std::uint32_t n = htonl(static_cast<std::uint32_t>(m.size()));
                    out.insert(out.end(), reinterpret_cast<char*>(&n), reinterpret_cast<char*>(&n) + 4);
                    out.insert(out.end(), m.begin(), m.end());
                };
                append(a); append(b);
                boost::asio::write(s, boost::asio::buffer(out));
                std::cout << "[case2] echo1 = " << recv_frame(s) << "\n";
                std::cout << "[case2] echo2 = " << recv_frame(s) << "\n";
            }

            // 用例 7：半包——分两次写 JSON
            {
                std::string m = R"({"from":1001,"to":0,"type":3,"ts":1730000001000})"; // 心跳
                std::uint32_t n = htonl(static_cast<std::uint32_t>(m.size()));
                char head[4]; std::memcpy(head, &n, 4);
                boost::asio::write(s, boost::asio::buffer(head, 2));
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                std::vector<char> rest;
                rest.insert(rest.end(), head + 2, head + 4);
                rest.insert(rest.end(), m.begin(), m.end());
                boost::asio::write(s, boost::asio::buffer(rest));
                std::cout << "[case3] echo = " << recv_frame(s) << "\n";
            }
        }

        // ------------------------------------------------------------------
        // 用例 8：登录 + 私聊转发（当前启用的主验证用例）
        // 用一个 io_context 开两条独立连接，分别模拟用户 A(1001)、B(1002)。
        // ------------------------------------------------------------------
        {
            tcp::socket a(io), b(io);
            // 用命令行传入的 port，避免硬编码 8888 导致换端口就失效。
            //a.connect({boost::asio::ip::make_address(host), port});
            b.connect({boost::asio::ip::make_address(host), port});

            // 两个用户分别登录：type=100(Login)，from 就是 userId。
            //send_frame(a, R"({"from":1001,"to":0,"type":100,"content":"tokenA"})");
            send_frame(b, R"({"from":1002,"to":0,"type":100,"content":"tokenB"})");

            // 各自读取服务器返回的 LoginAck（type=101）。
            //std::cout << "A ack: " << recv_frame(a) << "\n";
            std::cout << "B ack: " << recv_frame(b) << "\n";

            // 关键验证：A 发给 1002，B 必须收到。
            // 若 B 收到，说明服务器确实按 userId 在 SessionManager 里找到了 B 的连接。
            //send_frame(a, R"({"from":1001,"to":1002,"type":1,"msg_id":42,"content":"hi B"})");
            std::cout << "B recv: " << recv_frame(b) << "\n";

            // 掉线验证：关闭 A，服务器应打印 logout 且在线人数减 1。
            //a.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

    } catch (const std::exception& e) {
        std::cerr << "[client] error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
