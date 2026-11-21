#include "tcpclient.h"

#include <QJsonDocument>
#include <QRandomGenerator>

TcpClient::TcpClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
{
    connect(m_socket, &QTcpSocket::connected, this, &TcpClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &TcpClient::onDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &TcpClient::onSocketError);
    connect(m_socket, &QTcpSocket::readyRead, this, &TcpClient::onReadyRead);

    m_heartbeatTimer.setSingleShot(false);
    connect(&m_heartbeatTimer, &QTimer::timeout, this, &TcpClient::onHeartbeat);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &TcpClient::onReconnectTimeout);

    // 登录 ACK 超时(参考旧客户端 10s): 视为连接异常, 走重连
    m_loginAckTimer.setSingleShot(true);
    m_loginAckTimer.setInterval(10000);
    connect(&m_loginAckTimer, &QTimer::timeout, this, &TcpClient::onLoginAckTimeout);
}

void TcpClient::start(const QString &host, quint16 port, const QString &accessToken)
{
    m_host = host;
    mPort = port;
    m_token = accessToken;
    m_backoffMs = 1000;

    if (m_state == State::LoggedIn || m_state == State::Connected)
        return;                       // 已在服务中

    setState(State::Connecting);
    m_socket->abort();                // 清理半开连接
    m_socket->connectToHost(host, port);
}

void TcpClient::stop()
{
    m_reconnectTimer.stop();
    m_loginAckTimer.stop();
    m_heartbeatTimer.stop();
    m_token.clear();                  // 不再重连
    setState(State::Disconnected);
    m_socket->disconnectFromHost();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->abort();
}

void TcpClient::updateToken(const QString &accessToken)
{
    m_token = accessToken;
    // token 轮换后重走 TCP 登录(旧 token 的连接可能已被踢或即将过期)
    if (m_state != State::Disconnected) {
        m_socket->abort();
        setState(State::Connecting);
        m_socket->connectToHost(m_host, mPort);
    }
}

void TcpClient::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void TcpClient::onConnected()
{
    setState(State::Connected);
    m_rxBuffer.clear();
    m_headerReady = false;
    m_pendingFrameLen = 0;
    sendLogin();
    m_loginAckTimer.start();
}

void TcpClient::sendLogin()
{
    // 协议 5.5: type=100, content 放 access JWT; from 填 0, 服务端只信 JWT sub
    sendFrame(envelope(0, 0, MsgType::Login, 0, 0, 0, 0, m_token, false));
}

void TcpClient::onDisconnected()
{
    m_heartbeatTimer.stop();
    m_loginAckTimer.stop();
    const bool wasLoggedIn = (m_state == State::LoggedIn);
    if (m_state != State::Fatal)
        setState(State::Disconnected);
    // 意外断开(含被重复登录踢掉)才重连; stop() 后 m_token 为空不会触发
    if (wasLoggedIn || m_state == State::Disconnected) {
        if (!m_token.isEmpty())
            scheduleReconnect();
    }
}

void TcpClient::onSocketError()
{
    m_loginAckTimer.stop();
    if (m_state == State::Fatal)
        return;
    setState(State::Disconnected);
    if (!m_token.isEmpty())
        scheduleReconnect();
}

void TcpClient::onLoginAckTimeout()
{
    // 10s 没等到 101/102: 连接不可信, 强制重连
    m_socket->abort();
}

void TcpClient::scheduleReconnect()
{
    if (m_state == State::Fatal)
        return;
    // 指数退避 + 抖动(协议 3.3: min(60s,1s*2^n)+rand(0,1s), 旧客户端封顶 30s)
    const int jitter = QRandomGenerator::global()->bounded(1000);
    m_reconnectTimer.start(m_backoffMs + jitter);
    m_backoffMs = qMin(m_backoffMs * 2, kBackoffCapMs);
}

void TcpClient::onReconnectTimeout()
{
    if (m_token.isEmpty() || m_state == State::Fatal)
        return;
    setState(State::Connecting);
    m_socket->connectToHost(m_host, mPort);
}

void TcpClient::onHeartbeat()
{
    // 协议 5.6: 心跳不要求回复, 仅刷新服务端空闲计时
    if (m_state == State::LoggedIn)
        sendFrame(envelope(0, 0, MsgType::Heartbeat));
}

// ---------------------------------------------------------------------------
// 分帧接收: 4 字节大端长度 + N 字节 JSON body(协议 5.1)
// ---------------------------------------------------------------------------
void TcpClient::onReadyRead()
{
    m_rxBuffer.append(m_socket->readAll());

    for (;;) {
        if (!m_headerReady) {
            if (m_rxBuffer.size() < 4)
                return;
            const quint8 *h = reinterpret_cast<const quint8 *>(m_rxBuffer.constData());
            m_pendingFrameLen = (quint32(h[0]) << 24) | (quint32(h[1]) << 16)
                              | (quint32(h[2]) << 8) | quint32(h[3]);
            if (m_pendingFrameLen == 0 || m_pendingFrameLen > quint32(kMaxFrameBytes)) {
                // 恶意/异常长度: 断开重连(下行合法帧没有 4096 限制, 不能按上行限制裁)
                m_socket->abort();
                return;
            }
            m_rxBuffer.remove(0, 4);
            m_headerReady = true;
        }

        if (quint32(m_rxBuffer.size()) < m_pendingFrameLen)
            return;                   // body 未收全, 等下一次 readyRead

        const QByteArray body = m_rxBuffer.left(int(m_pendingFrameLen));
        m_rxBuffer.remove(0, int(m_pendingFrameLen));
        m_headerReady = false;
        m_pendingFrameLen = 0;

        const QJsonObject frame = QJsonDocument::fromJson(body).object();
        if (frame.isEmpty())
            continue;                 // 非法 JSON: 忽略该帧, 不断连接

        const int type = frame.value(QStringLiteral("type")).toInt();
        if (type == MsgType::LoginAck) {
            m_loginAckTimer.stop();
            m_backoffMs = 1000;       // 登录成功, 退避复位
            setState(State::LoggedIn);
            m_heartbeatTimer.start(m_heartbeatMs);
            emit loginResult(true, QString());
            continue;                 // 101 属于连接管理, 不下发业务层
        }
        if (type == MsgType::Error && m_state != State::LoggedIn) {
            // 登录阶段的 102 = token 非法/过期, 重连无意义(协议 5.5)
            m_loginAckTimer.stop();
            m_heartbeatTimer.stop();
            setState(State::Fatal);
            const QString reason = frame.value(QStringLiteral("content")).toString();
            emit loginResult(false, reason);
            m_socket->abort();
            continue;
        }

        emit frameReceived(type, frame);
    }
}

// ---------------------------------------------------------------------------
bool TcpClient::sendFrame(const QJsonObject &envelope)
{
    if (m_socket->state() != QTcpSocket::ConnectedState)
        return false;

    const QByteArray body = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    if (body.isEmpty() || body.size() > kMaxUplinkBody) {
        // 协议 5.1: N==0 或 N>4096 会被服务端直接断连, 客户端必须自律
        return false;
    }

    QByteArray frame;
    frame.reserve(body.size() + 4);
    const quint32 n = static_cast<quint32>(body.size());
    frame.append(char((n >> 24) & 0xFF));
    frame.append(char((n >> 16) & 0xFF));
    frame.append(char((n >> 8) & 0xFF));
    frame.append(char(n & 0xFF));
    frame.append(body);

    m_socket->write(frame);
    return true;
}

QJsonObject TcpClient::envelope(quint64 fromUid, quint64 toUid, int type,
                                quint64 msgId, quint64 ts, quint64 conversationId,
                                quint64 seq, const QString &content, bool isGroup)
{
    // 与旧 Win32 客户端一致: 所有字段总是序列化, 避免服务端解析歧义
    QJsonObject o;
    o[QStringLiteral("from")]  = static_cast<double>(fromUid);
    o[QStringLiteral("to")]    = static_cast<double>(toUid);
    o[QStringLiteral("type")]  = type;
    o[QStringLiteral("msg_id")] = static_cast<double>(msgId);
    o[QStringLiteral("ts")]    = static_cast<double>(ts);
    o[QStringLiteral("conversation_id")] = static_cast<double>(conversationId);
    o[QStringLiteral("seq")]   = static_cast<double>(seq);
    o[QStringLiteral("content")] = content;
    o[QStringLiteral("is_group")] = isGroup;
    return o;
}
