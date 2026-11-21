#ifndef CXIM_TCPCLIENT_H
#define CXIM_TCPCLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QByteArray>
#include "../core/imstypes.h"

/**
 * @file tcpclient.h
 * @brief TCP 数据面客户端(协议第 5 章): 长度头分帧 + JWT 登录 + 心跳 + 断线重连
 *
 * 设计要点:
 *  - 本类只管"帧的收发与连接生命周期", 不解析业务语义; 业务分发在 service 层
 *    (ChatService 监听 frameReceived 并按 type 处理);
 *  - 分帧: 4 字节大端长度 + UTF-8 JSON; 上行单帧 body 上限 4096(超出本地拒发,
 *    协议规定服务端会直接断连接), 下行不设 4096 限制(历史响应可能很大),
 *    仅做 8MB 恶意长度防护;
 *  - 心跳 25s(服务端空闲 60s 踢人), 登录 ACK 10s 超时触发重连;
 *  - 重连采用指数退避 min(30s, 1s*2^n)+随机抖动, 每次成功登录后退避复位;
 *  - 登录失败(102, token 非法/过期)属致命错误, 停止重连并上抛, 由上层决定
 *    刷新 token 或回登录页(协议 3.3)。
 */
class TcpClient : public QObject
{
    Q_OBJECT
public:
    enum class State { Disconnected, Connecting, Connected, LoggedIn, Fatal };
    Q_ENUM(State)

    explicit TcpClient(QObject *parent = nullptr);

    /// host/port 来自 AppConfig; token 为 HTTP 登录获得的 access JWT
    void start(const QString &host, quint16 port, const QString &accessToken);
    /// 主动停止(退出登录/程序退出), 不再重连
    void stop();
    /// token 刷新成功后用新 token 重新建立会话
    void updateToken(const QString &accessToken);

    bool isLoggedIn() const { return m_state == State::LoggedIn; }
    State state() const { return m_state; }

    /// 发送一帧; body 超 4096 字节时返回 false 且不发送(保护连接)
    bool sendFrame(const QJsonObject &envelope);

    /// 构造协议 5.2 信封: 与旧 Win32 客户端口径一致, 所有字段总是序列化
    static QJsonObject envelope(quint64 fromUid, quint64 toUid, int type,
                                quint64 msgId = 0, quint64 ts = 0,
                                quint64 conversationId = 0, quint64 seq = 0,
                                const QString &content = QString(),
                                bool isGroup = false);

    void setHeartbeatIntervalMs(int ms) { m_heartbeatMs = ms; }

signals:
    void stateChanged(TcpClient::State s);
    /// 登录结果: success=false 时 reason 为服务端 102 文本(如 invalid or expired token)
    void loginResult(bool success, const QString &reason);
    /// 收到任意下行帧(type 见 MsgType), 业务层按类型分发
    void frameReceived(int type, const QJsonObject &frame);

private slots:
    void onConnected();
    void onDisconnected();
    void onSocketError();
    void onReadyRead();
    void onHeartbeat();
    void onReconnectTimeout();
    void onLoginAckTimeout();

private:
    void setState(State s);
    void scheduleReconnect();
    void sendLogin();

    QTcpSocket *m_socket;
    QTimer m_heartbeatTimer;
    QTimer m_reconnectTimer;
    QTimer m_loginAckTimer;

    QString m_host;
    quint16 mPort = 0;
    QString m_token;

    State m_state = State::Disconnected;
    QByteArray m_rxBuffer;             // 下行组帧缓冲
    quint32 m_pendingFrameLen = 0;     // 当前正在收的 body 长度
    bool m_headerReady = false;

    int m_heartbeatMs = 25000;
    int m_backoffMs = 1000;            // 重连退避, 成功登录后复位为 1s
    static constexpr int kBackoffCapMs = 30000;
    static constexpr int kMaxFrameBytes = 8 * 1024 * 1024;   // 下行防护
    static constexpr int kMaxUplinkBody = 4096;              // 上行限制(协议 5.1)
};

#endif // CXIM_TCPCLIENT_H
