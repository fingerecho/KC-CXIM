#ifndef CXIM_TCP_CLIENT_H_
#define CXIM_TCP_CLIENT_H_

#include "CXIMAppConfig.h"

#ifdef CXIM_IMS_CORE_SDK_VERSION

#include "ImsTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

/** @class TcpClient
* @brief IMS TCP 长连接客户端：4 字节大端长度头 + JSON body 分帧，
*        登录后自动心跳保活，断线指数退避自动重连。
* @note  回调在接收线程触发，UI 层必须自行 PostTask 回 UI 线程再刷新。
*/
class TcpClient
{
public:
    using MessageCallback = std::function<void(const ImsMessage&)>;
    using LoginCallback = std::function<void(bool success, const std::string& error, bool firstLogin)>;
    using DisconnectedCallback = std::function<void()>;

    static TcpClient& Instance();

    TcpClient() = default;
    ~TcpClient();

    /** 设置 TCP 服务器地址，需在 Start 之前调用 */
    void SetServer(const std::string& host, uint16_t port);

    /** 启动连接线程，token 为 HTTP 登录获得的 JWT */
    bool Start(const std::string& token);

    /** 停止连接线程并关闭 socket */
    void Stop();

    bool IsLoggedIn() const;
    bool IsRunning() const;

    /** 线程安全发送一条消息（内部序列化为 JSON 并加长度头） */
    bool Send(const ImsMessage& msg);

    void SetMessageCallback(const MessageCallback& callback);
    void SetLoginCallback(const LoginCallback& callback);
    void SetDisconnectedCallback(const DisconnectedCallback& callback);

private:
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    void Run();
    /** 发送一帧原始 JSON，返回是否成功 */
    bool SendRaw(const std::string& json);
    /** 处理收到的一帧，返回 false 表示应断开连接 */
    bool HandleFrame(const std::string& body);

private:
    std::string m_host = "127.0.0.1";
    uint16_t m_port = 8888;
    std::string m_token;

    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ true };
    std::atomic<bool> m_loggedIn{ false };

    std::thread m_thread;
    void* m_socket = nullptr;       // 实际为 Windows SOCKET，仅在 .cpp 内使用
    std::mutex m_sendMutex;
    std::mutex m_stateMutex;

    MessageCallback m_messageCallback;
    LoginCallback m_loginCallback;
    DisconnectedCallback m_disconnectedCallback;
    std::mutex m_callbackMutex;
};

#endif // CXIM_IMS_CORE_SDK_VERSION

#endif // CXIM_TCP_CLIENT_H_
