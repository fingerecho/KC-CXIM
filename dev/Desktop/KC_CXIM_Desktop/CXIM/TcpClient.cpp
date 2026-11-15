#include "TcpClient.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

#include "ImsSession.h"
#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

namespace {

constexpr uint32_t kMaxFrameSize = 8u * 1024u * 1024u;
constexpr int kHeartbeatIntervalMs = 25000;
constexpr int kLoginAckTimeoutMs = 10000;
constexpr int kMinBackoffMs = 1000;
constexpr int kMaxBackoffMs = 30000;

int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct WsaGuard
{
    WsaGuard()
    {
#ifdef _WIN32
        WSADATA wsaData;
        ::WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    }
};

WsaGuard g_wsaGuard;

} // namespace

TcpClient& TcpClient::Instance()
{
    static TcpClient instance;
    return instance;
}

TcpClient::~TcpClient()
{
    Stop();
}

void TcpClient::SetServer(const std::string& host, uint16_t port)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (!host.empty()) {
        m_host = host;
    }
    if (port != 0) {
        m_port = port;
    }
}

bool TcpClient::Start(const std::string& token)
{
    if (token.empty()) {
        return false;
    }
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true)) {
        return true; // 已在运行
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_token = token;
    }
    m_stopRequested = false;
    m_thread = std::thread([this]() { Run(); });
    return true;
}

void TcpClient::Stop()
{
    m_stopRequested = true;
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running = false;
    m_loggedIn = false;
}

bool TcpClient::IsLoggedIn() const
{
    return m_loggedIn;
}

bool TcpClient::IsRunning() const
{
    return m_running;
}

void TcpClient::SetMessageCallback(const MessageCallback& callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_messageCallback = callback;
}

void TcpClient::SetLoginCallback(const LoginCallback& callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_loginCallback = callback;
}

void TcpClient::SetDisconnectedCallback(const DisconnectedCallback& callback)
{
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_disconnectedCallback = callback;
}

bool TcpClient::Send(const ImsMessage& msg)
{
    return SendRaw(SerializeImsMessage(msg));
}

bool TcpClient::SendRaw(const std::string& json)
{
    std::vector<char> frame(4 + json.size());
    const uint32_t length = static_cast<uint32_t>(json.size());
    frame[0] = static_cast<char>((length >> 24) & 0xFF);
    frame[1] = static_cast<char>((length >> 16) & 0xFF);
    frame[2] = static_cast<char>((length >> 8) & 0xFF);
    frame[3] = static_cast<char>(length & 0xFF);
    std::memcpy(frame.data() + 4, json.data(), json.size());

    std::lock_guard<std::mutex> lock(m_sendMutex);
    SOCKET sock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        sock = static_cast<SOCKET>(reinterpret_cast<uintptr_t>(m_socket));
    }
    if (sock == INVALID_SOCKET) {
        return false;
    }

    size_t sent = 0;
    while (sent < frame.size()) {
        const int n = ::send(sock, frame.data() + sent, static_cast<int>(frame.size() - sent), 0);
        if (n == SOCKET_ERROR) {
#ifdef _WIN32
            if (::WSAGetLastError() == WSAEINTR) {
                continue;
            }
#endif
            return false;
        }
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool TcpClient::HandleFrame(const std::string& body)
{
    ImsMessage msg;
    std::string err;
    if (!ParseImsMessage(body, msg, err)) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->warn("TCP 帧解析失败: {}, body: {}", err, body);
        }
        return true;
    }

    MessageCallback messageCallback;
    LoginCallback loginCallback;
    {
        std::lock_guard<std::mutex> lock(m_callbackMutex);
        messageCallback = m_messageCallback;
        loginCallback = m_loginCallback;
    }

    if (msg.type == static_cast<int>(ImsMsgType::LoginAck)) {
        const bool firstLogin = !m_loggedIn.exchange(true);
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->info("TCP 登录成功{}", firstLogin ? "" : "（重连）");
        }
        if (loginCallback) {
            loginCallback(true, std::string(), firstLogin);
        }
        return true;
    }

    if ((msg.type == static_cast<int>(ImsMsgType::Error)) && !m_loggedIn) {
        // 登录阶段的 102：token 无效或过期，不再重连
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->error("TCP 登录失败: {}", msg.content);
        }
        if (loginCallback) {
            loginCallback(false, msg.content, true);
        }
        return false;
    }

    if (messageCallback) {
        messageCallback(msg);
    }
    return true;
}

void TcpClient::Run()
{
    bool fatalLoginFailure = false;
    int backoffMs = kMinBackoffMs;
    bool everLoggedIn = false;

    while (!m_stopRequested && !fatalLoginFailure) {
        std::string host;
        uint16_t port = 0;
        std::string token;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            host = m_host;
            port = m_port;
            token = m_token;
        }

        char portStr[16];
        std::snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));
        struct addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* addresses = nullptr;
        SOCKET sock = INVALID_SOCKET;
        if (::getaddrinfo(host.c_str(), portStr, &hints, &addresses) == 0) {
            for (struct addrinfo* ai = addresses; ai != nullptr; ai = ai->ai_next) {
                sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (sock == INVALID_SOCKET) {
                    continue;
                }
                if (::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
                    break;
                }
                ::closesocket(sock);
                sock = INVALID_SOCKET;
            }
            ::freeaddrinfo(addresses);
        }
        if (sock == INVALID_SOCKET) {
            auto logger = spdlog::get("im_file_logger");
            if (logger) {
                logger->warn("TCP 连接失败: {}:{}，{}ms 后重试", host, port, backoffMs);
            }
            // 可中断的退避等待
            const int64_t deadline = NowMs() + backoffMs;
            while (!m_stopRequested && (NowMs() < deadline)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            backoffMs = (backoffMs * 2 > kMaxBackoffMs) ? kMaxBackoffMs : backoffMs * 2;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_socket = reinterpret_cast<void*>(static_cast<uintptr_t>(sock));
        }
        m_loggedIn = false;

        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->info("TCP 已连接 {}:{}，发送登录帧", host, port);
        }

        // 发送登录帧
        ImsMessage loginMsg;
        loginMsg.type = static_cast<int>(ImsMsgType::Login);
        loginMsg.content = token;
        SendRaw(SerializeImsMessage(loginMsg));

        std::vector<char> buffer;
        bool connectionBroken = false;
        bool fatal = false;
        const int64_t connectTs = NowMs();
        int64_t lastHeartbeatTs = NowMs();

        while (!m_stopRequested && !connectionBroken && !fatal) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100ms
            const int sel = ::select(static_cast<int>(sock + 1), &readSet, nullptr, nullptr, &tv);
            if (sel == SOCKET_ERROR) {
#ifdef _WIN32
                const int wsaErr = ::WSAGetLastError();
                if (wsaErr == WSAENOTSOCK) {
                    connectionBroken = true;
                    break;
                }
#endif
                connectionBroken = true;
                break;
            }

            if (sel > 0) {
                char chunk[4096];
                const int n = ::recv(sock, chunk, static_cast<int>(sizeof(chunk)), 0);
                if (n <= 0) {
                    connectionBroken = true;
                    break;
                }
                buffer.insert(buffer.end(), chunk, chunk + n);

                // 循环拆帧（处理粘包/半包）
                while (buffer.size() >= 4) {
                    const uint32_t frameLen =
                        (static_cast<uint32_t>(static_cast<unsigned char>(buffer[0])) << 24) |
                        (static_cast<uint32_t>(static_cast<unsigned char>(buffer[1])) << 16) |
                        (static_cast<uint32_t>(static_cast<unsigned char>(buffer[2])) << 8) |
                        static_cast<uint32_t>(static_cast<unsigned char>(buffer[3]));
                    if (frameLen > kMaxFrameSize) {
                        connectionBroken = true; // 非法长度，断开重连
                        break;
                    }
                    if (buffer.size() < (4 + static_cast<size_t>(frameLen))) {
                        break; // 半包，等待更多数据
                    }
                    std::string body(buffer.begin() + 4, buffer.begin() + 4 + static_cast<int>(frameLen));
                    buffer.erase(buffer.begin(), buffer.begin() + 4 + static_cast<int>(frameLen));
                    if (!HandleFrame(body)) {
                        fatal = true;
                        break;
                    }
                }
                if (fatal) {
                    break;
                }
            }

            const int64_t now = NowMs();
            if (m_loggedIn && ((now - lastHeartbeatTs) >= kHeartbeatIntervalMs)) {
                ImsMessage heartbeat;
                heartbeat.type = static_cast<int>(ImsMsgType::Heartbeat);
                SendRaw(SerializeImsMessage(heartbeat));
                lastHeartbeatTs = now;
            }

            if (!m_loggedIn && ((now - connectTs) > kLoginAckTimeoutMs)) {
                auto log = spdlog::get("im_file_logger");
                if (log) {
                    log->warn("TCP 登录回执超时，断开重连");
                }
                connectionBroken = true;
            }
        }

        m_loggedIn = false;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_socket = nullptr;
        }
#ifdef _WIN32
        ::closesocket(sock);
#else
        ::close(sock);
#endif

        if (fatal) {
            fatalLoginFailure = true;
            break;
        }

        if (everLoggedIn) {
            DisconnectedCallback disconnectedCallback;
            {
                std::lock_guard<std::mutex> lock(m_callbackMutex);
                disconnectedCallback = m_disconnectedCallback;
            }
            if (disconnectedCallback) {
                disconnectedCallback();
            }
            backoffMs = kMinBackoffMs;
        }
        everLoggedIn = true;

        const int64_t deadline = NowMs() + backoffMs;
        while (!m_stopRequested && (NowMs() < deadline)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        backoffMs = (backoffMs * 2 > kMaxBackoffMs) ? kMaxBackoffMs : backoffMs * 2;
    }

    m_loggedIn = false;
    m_running = false;
}

#endif // CXIM_IMS_CORE_SDK_VERSION
