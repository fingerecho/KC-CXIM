#include "chatservice.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>
#include <utility>   // std::exchange

ChatService::ChatService(TcpClient *tcp, Session *session, QObject *parent)
    : QObject(parent)
    , m_tcp(tcp)
    , m_session(session)
{
    connect(tcp, &TcpClient::frameReceived, this, &ChatService::onFrame);
    connect(tcp, &TcpClient::loginResult, this, [this](bool ok, const QString &) {
        if (ok)
            emit tcpLoggedIn();     // 上层做全量对齐(协议 5.15 补偿矩阵)
    });
    // 断线瞬间清空 FIFO 配对队列: 否则重连后旧在途项会错配新响应,
    // 未确认的发送统一标记失败(重试请复用同一 clientMsgId, 协议 5.7 幂等)
    connect(tcp, &TcpClient::stateChanged, this, [this](TcpClient::State s) {
        if (s != TcpClient::State::Disconnected && s != TcpClient::State::Fatal)
            return;
        if (!m_pendingSends.isEmpty()) {
            const auto pend = std::exchange(m_pendingSends, {});
            for (const PendingSend &ps : pend) {
                PeerState &peer = m_peers[ps.key];
                for (ChatMessage &m : peer.items) {
                    if (m.clientMsgId == ps.clientMsgId) {
                        m.status = MessageStatus::Failed;
                        break;
                    }
                }
                emit messageStatusChanged(ps.key, ps.clientMsgId,
                                          MessageStatus::Failed,
                                          QStringLiteral("连接已断开"));
            }
        }
        m_historyFifo.clear();
    });
}

QString ChatService::convKey(quint64 peerId, bool isGroup)
{
    return (isGroup ? QStringLiteral("g:") : QStringLiteral("s:")) + QString::number(peerId);
}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------
static quint64 genClientMsgId(quint64 &counter)
{
    // 与旧客户端一致: 毫秒时间戳做高位 + 进程内自增低位, 保证重启后不碰撞;
    // 重试必须复用同一 msg_id(协议 5.7 幂等)
    return static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 16 + (++counter % 16);
}

quint64 ChatService::sendText(quint64 peerId, bool isGroup, const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return 0;

    const quint64 id = genClientMsgId(m_msgIdCounter);
    const QString key = convKey(peerId, isGroup);

    ChatMessage m;
    m.clientMsgId = id;
    m.toUid = peerId;
    m.fromUid = m_session->userId();
    m.type = MsgType::Text;
    m.content = trimmed;
    m.isGroup = isGroup;
    m.ts = QDateTime::currentMSecsSinceEpoch();
    m.status = MessageStatus::Sending;
    m.outgoing = true;

    // 协议 5.7: from/ts/conversation_id/seq 由服务端生成, 上行置 0
    if (!m_tcp->sendFrame(TcpClient::envelope(0, peerId, MsgType::Text, id,
                                              0, 0, 0, trimmed, isGroup))) {
        m.status = MessageStatus::Failed;   // 未连接/超帧长
        PeerState &st = m_peers[key];
        st.conv.peerId = peerId;
        st.conv.isGroup = isGroup;
        upsertMessage(st, m);
        emit messageLocalAdded(key, m);
        emit messageStatusChanged(key, id, MessageStatus::Failed,
                                  QStringLiteral("未连接服务器"));
        return id;
    }

    m_pendingSends.append({id, key, isGroup});
    PeerState &st = m_peers[key];
    st.conv.peerId = peerId;
    st.conv.isGroup = isGroup;
    upsertMessage(st, m);
    emit messageLocalAdded(key, m);
    return id;
}

quint64 ChatService::sendMedia(quint64 peerId, bool isGroup, int type,
                               const QString &fileId, const QString &displayName)
{
    const quint64 id = genClientMsgId(m_msgIdCounter);
    const QString key = convKey(peerId, isGroup);

    // 协议 5.8: content 只放 {"file_id":...}, 服务端校验权属后重建权威元数据
    QJsonObject meta;
    meta[QStringLiteral("file_id")] = fileId;
    const QString content = QString::fromUtf8(
        QJsonDocument(meta).toJson(QJsonDocument::Compact));

    ChatMessage m;
    m.clientMsgId = id;
    m.toUid = peerId;
    m.fromUid = m_session->userId();
    m.type = type;
    m.content = content;
    m.isGroup = isGroup;
    m.ts = QDateTime::currentMSecsSinceEpoch();
    m.status = MessageStatus::Sending;
    m.outgoing = true;

    if (!m_tcp->sendFrame(TcpClient::envelope(0, peerId, type, id, 0, 0, 0, content, isGroup))) {
        m.status = MessageStatus::Failed;
        PeerState &st = m_peers[key];
        st.conv.peerId = peerId;
        st.conv.isGroup = isGroup;
        upsertMessage(st, m);
        emit messageLocalAdded(key, m);
        emit messageStatusChanged(key, id, MessageStatus::Failed,
                                  QStringLiteral("未连接服务器"));
        return id;
    }

    m_pendingSends.append({id, key, isGroup});
    PeerState &st = m_peers[key];
    st.conv.peerId = peerId;
    st.conv.isGroup = isGroup;
    upsertMessage(st, m);
    emit messageLocalAdded(key, m);
    return id;
}

// ---------------------------------------------------------------------------
// 历史(协议 5.9): 响应不带请求标识 → 严格 FIFO 配对, 串行排队发送
// ---------------------------------------------------------------------------
void ChatService::requestHistory(quint64 peerId, bool isGroup, quint64 beforeSeq, int count)
{
    const HistoryReq req{peerId, isGroup, beforeSeq};
    const bool queueEmpty = m_historyFifo.isEmpty();
    m_historyFifo.append(req);
    if (!queueEmpty)
        return;   // 已有在途请求, 排队等响应(保证 FIFO 语义成立)

    // seq 字段复用为 before_seq, content 是"条数"的字符串(协议 5.9)
    m_tcp->sendFrame(TcpClient::envelope(0, peerId, MsgType::HistoryReq,
                                         0, 0, 0, beforeSeq,
                                         QString::number(qBound(1, count, 100)), isGroup));
}

bool ChatService::historyLoadedOnce(quint64 peerId, bool isGroup) const
{
    return m_peers.value(convKey(peerId, isGroup)).firstHistoryLoaded;
}

void ChatService::markRead(quint64 peerId, bool isGroup)
{
    PeerState &st = m_peers[convKey(peerId, isGroup)];
    if (st.convId == 0 || st.lastSeq == 0)
        return;
    // 协议 5.10: content 非 "delivered" 即视为已读; 只增不减, 幂等
    m_tcp->sendFrame(TcpClient::envelope(0, 0, MsgType::Ack, 0, 0,
                                         st.convId, st.lastSeq, QString(), isGroup));
    if (st.conv.unread > 0) {
        st.conv.unread = 0;
        emit conversationChanged(st.conv.key(), st.conv);
        emit totalUnreadChanged(totalUnread());
    }
}

// ---------------------------------------------------------------------------
// 本地存储
// ---------------------------------------------------------------------------
QList<Conversation> ChatService::conversations() const
{
    QList<Conversation> out;
    out.reserve(m_peers.size());
    for (const PeerState &st : m_peers)
        if (st.conv.hasLast)
            out.append(st.conv);
    std::sort(out.begin(), out.end(), [](const Conversation &a, const Conversation &b) {
        return a.lastMessage.ts > b.lastMessage.ts;   // 最近活跃在前
    });
    return out;
}

QList<ChatMessage> ChatService::messages(quint64 peerId, bool isGroup) const
{
    return m_peers.value(convKey(peerId, isGroup)).items;
}

void ChatService::clearConversation(quint64 peerId, bool isGroup)
{
    m_peers.remove(convKey(peerId, isGroup));
    emit conversationChanged(convKey(peerId, isGroup), Conversation{});
    emit totalUnreadChanged(totalUnread());
}

int ChatService::totalUnread() const
{
    int sum = 0;
    for (const PeerState &st : m_peers)
        sum += st.conv.unread;
    return sum;
}

void ChatService::upsertMessage(PeerState &st, const ChatMessage &m)
{
    // 去重: 服务端消息按 (conversation_id, seq); 本地发送按 clientMsgId
    if (m.seq > 0) {
        const QString dedupe = QStringLiteral("%1:%2").arg(m.conversationId).arg(m.seq);
        if (st.seen.contains(dedupe))
            return;
        st.seen.insert(dedupe);
    }
    for (ChatMessage &exist : st.items) {
        if (exist.clientMsgId != 0 && exist.clientMsgId == m.clientMsgId) {
            exist = m;                       // 覆盖发送中状态
            return;
        }
    }
    // 按 seq 升序插入(无 seq 的本地发送消息排最后)
    int i = st.items.size();
    while (i > 0 && st.items[i - 1].seq > m.seq && m.seq > 0)
        --i;
    st.items.insert(i, m);
}

void ChatService::touchConversation(PeerState &st, const ChatMessage &m, bool countUnread)
{
    st.conv.hasLast = true;
    st.conv.lastMessage = m;
    if (countUnread && !m.outgoing)
        ++st.conv.unread;
    if (m.conversationId)
        st.convId = m.conversationId;
    if (m.seq > st.lastSeq)
        st.lastSeq = m.seq;
    emit conversationChanged(st.conv.key(), st.conv);
    emit totalUnreadChanged(totalUnread());
}

// ---------------------------------------------------------------------------
// 帧分发
// ---------------------------------------------------------------------------
QJsonObject ChatService::parseEventContent(const QJsonObject &frame)
{
    // 事件公共约定(协议 5.12): content 是 JSON 文本, 需要二次解析
    return QJsonDocument::fromJson(
        frame.value(QStringLiteral("content")).toString().toUtf8()).object();
}

void ChatService::onFrame(int type, const QJsonObject &frame)
{
    switch (type) {
    case MsgType::Text:
    case MsgType::Image:
    case MsgType::File: {
        ChatMessage m = ChatMessage::fromFrame(frame);
        m.outgoing = false;
        m.status = MessageStatus::Sent;
        // 单聊下行: to==self 时 peer=from; 群聊: peer=to(groupId)
        const quint64 peer = m.isGroup ? m.toUid
                            : (m.fromUid == m_session->userId() ? m.toUid : m.fromUid);
        const QString key = convKey(peer, m.isGroup);
        PeerState &st = m_peers[key];
        st.conv.peerId = peer;
        st.conv.isGroup = m.isGroup;
        upsertMessage(st, m);
        touchConversation(st, m, true);
        emit messageIncoming(key, m);
        sendDelivered(m);
        break;
    }
    case MsgType::SendAck:
        handleSendAck(frame);
        break;
    case MsgType::Error:
        handleError(frame);
        break;
    case MsgType::HistoryResp:
        handleHistoryResp(frame);
        break;

    // ---- 好友事件: 本层只解析转发, 事实对齐由 UI/service 触发 HTTP 重拉 ----
    case MsgType::FriendRequest:         emit friendRequestEvent(parseEventContent(frame)); break;
    case MsgType::FriendRequestResult:   emit friendRequestResultEvent(parseEventContent(frame)); break;
    case MsgType::FriendListChanged:     emit friendListChangedEvent(parseEventContent(frame)); break;

    // ---- 群事件 ----
    case MsgType::GroupMemberChanged:    emit groupMemberChangedEvent(parseEventContent(frame)); break;
    case MsgType::GroupDismissed:        emit groupDismissedEvent(parseEventContent(frame)); break;
    case MsgType::GroupInfoChanged:      emit groupInfoChangedEvent(parseEventContent(frame)); break;
    case MsgType::GroupRequest:          emit groupRequestEvent(parseEventContent(frame)); break;
    case MsgType::GroupRequestResult:    emit groupRequestResultEvent(parseEventContent(frame)); break;
    default:
        break;   // 未知类型仅可能出现在下行(上行未知 type 会被服务端断连), 忽略
    }
}

void ChatService::sendDelivered(const ChatMessage &m)
{
    // 协议 5.10: 收到带 conv+seq 的消息立即回投递回执(幂等, 静默丢弃无害)
    if (m.conversationId == 0 || m.seq == 0)
        return;
    m_tcp->sendFrame(TcpClient::envelope(0, 0, MsgType::Ack, 0, 0,
                                         m.conversationId, m.seq,
                                         QStringLiteral("delivered"), m.isGroup));
}

void ChatService::handleSendAck(const QJsonObject &frame)
{
    // 113 不带 client msg_id → 与 FIFO 中首个同 isGroup 的待确认项配对(旧客户端定案)
    const bool isGroup = frame.value(QStringLiteral("is_group")).toBool();
    int idx = -1;
    for (int i = 0; i < m_pendingSends.size(); ++i) {
        if (m_pendingSends[i].isGroup == isGroup) { idx = i; break; }
    }
    if (idx < 0)
        return;
    const PendingSend ps = m_pendingSends.takeAt(idx);

    const QString content = frame.value(QStringLiteral("content")).toString();
    const MessageStatus st = (content == QStringLiteral("duplicate"))
                                 ? MessageStatus::Duplicate : MessageStatus::Sent;

    PeerState &peer = m_peers[ps.key];
    for (ChatMessage &m : peer.items) {
        if (m.clientMsgId == ps.clientMsgId) {
            m.status = st;
            m.msgId = quint64(frame.value(QStringLiteral("msg_id")).toDouble());
            m.seq = quint64(frame.value(QStringLiteral("seq")).toDouble());
            m.conversationId = quint64(frame.value(QStringLiteral("conversation_id")).toDouble());
            if (m.conversationId) peer.convId = m.conversationId;
            if (m.seq > peer.lastSeq) peer.lastSeq = m.seq;
            break;
        }
    }
    emit messageStatusChanged(ps.key, ps.clientMsgId, st, QString());
}

void ChatService::handleError(const QJsonObject &frame)
{
    const QString reason = frame.value(QStringLiteral("content")).toString();
    // 登录后 102 绝大多数来自最近一次发送(协议 5.7 失败语义), 弹 FIFO 队首标记失败
    if (!m_pendingSends.isEmpty()) {
        const PendingSend ps = m_pendingSends.takeFirst();
        PeerState &peer = m_peers[ps.key];
        for (ChatMessage &m : peer.items) {
            if (m.clientMsgId == ps.clientMsgId) {
                m.status = MessageStatus::Failed;
                break;
            }
        }
        emit messageStatusChanged(ps.key, ps.clientMsgId, MessageStatus::Failed, reason);
    }
    emit tcpError(reason);
}

void ChatService::handleHistoryResp(const QJsonObject &frame)
{
    if (m_historyFifo.isEmpty())
        return;
    const HistoryReq req = m_historyFifo.takeFirst();   // 严格同序配对
    const QString key = convKey(req.peerId, req.isGroup);
    PeerState &st = m_peers[key];
    st.conv.peerId = req.peerId;
    st.conv.isGroup = req.isGroup;

    // content 为数组文本(协议 5.9), 元素二次解析
    const QJsonArray arr = QJsonDocument::fromJson(
        frame.value(QStringLiteral("content")).toString().toUtf8()).array();
    QList<ChatMessage> msgs;
    msgs.reserve(arr.size());
    quint64 oldest = ~0ULL;
    for (const QJsonValue &v : arr) {
        ChatMessage m = ChatMessage::fromFrame(v.toObject());
        m.outgoing = (m.fromUid == m_session->userId());
        m.status = MessageStatus::Sent;
        upsertMessage(st, m);
        oldest = qMin(oldest, m.seq);
        msgs.append(m);
    }
    if (!msgs.isEmpty()) {
        // 历史消息不参与未读计数(打开会话即读)
        touchConversation(st, msgs.last(), false);
        st.firstHistoryLoaded = true;
    }
    emit historyLoaded(key, msgs, oldest == ~0ULL ? 0 : oldest, msgs.size() < 20);

    // 队列非空则发下一个排队请求(保持 FIFO)
    if (!m_historyFifo.isEmpty()) {
        const HistoryReq &n = m_historyFifo.first();
        m_tcp->sendFrame(TcpClient::envelope(0, n.peerId, MsgType::HistoryReq,
                                             0, 0, 0, n.beforeSeq,
                                             QStringLiteral("20"), n.isGroup));
    }
}
