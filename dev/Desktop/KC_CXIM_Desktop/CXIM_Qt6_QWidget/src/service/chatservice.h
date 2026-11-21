#ifndef CXIM_CHATSERVICE_H
#define CXIM_CHATSERVICE_H

#include <QObject>
#include "../net/tcpclient.h"
#include "../core/session.h"
#include "../core/imstypes.h"

/**
 * @file chatservice.h
 * @brief 消息与会话语义层(协议 5.7~5.13): TCP 帧 → 业务信号 + 本地会话存储
 *
 * 这是"功能"与"UI"分离的关键枢纽:
 *  - 输入: TcpClient 的原始帧(type + JSON), 输出: 领域信号(ChatMessage/Conversation);
 *  - UI 永远不接触 JSON 帧, service 之下不认识 QWidget;
 *  - 本地会话存储(内存): 每个会话一个按 seq 排序的消息表 + (conversation_id,seq) 去重集,
 *    重连后由上层重新拉历史/全量对齐(协议 1.2: 事件不承载事实);
 *  - 关联策略(沿用旧 Win32 客户端定案):
 *      SendAck(113)/Error(102) 不携带请求方信息 → 与待确认发送 FIFO 按 isGroup 配对;
 *      HistoryResp(111) → 与历史请求 FIFO 严格同序配对, 因此本类串行化历史请求;
 *  - 回执: 收到在线消息立刻回 delivered(112), 会话打开时按最大 seq 回已读。
 */
class ChatService : public QObject
{
    Q_OBJECT
public:
    ChatService(TcpClient *tcp, Session *session, QObject *parent = nullptr);

    /// 会话本地键: "s:<peerId>" / "g:<groupId>"
    static QString convKey(quint64 peerId, bool isGroup);

    // ---------------- 发送(返回 clientMsgId, 供 UI 关联气泡状态) ----------------
    quint64 sendText(quint64 peerId, bool isGroup, const QString &text);
    /// type 传 MsgType::Image / MsgType::File; fileId 来自 FileUploader commit(协议 5.8)
    quint64 sendMedia(quint64 peerId, bool isGroup, int type,
                      const QString &fileId, const QString &displayName);

    // ---------------- 历史(内部串行排队, FIFO 配对响应) ----------------
    void requestHistory(quint64 peerId, bool isGroup, quint64 beforeSeq, int count = 20);
    bool historyLoadedOnce(quint64 peerId, bool isGroup) const;

    // ---------------- 已读回执 ----------------
    void markRead(quint64 peerId, bool isGroup);

    // ---------------- 本地会话存储(供 UI 渲染) ----------------
    QList<Conversation> conversations() const;
    QList<ChatMessage> messages(quint64 peerId, bool isGroup) const;
    void clearConversation(quint64 peerId, bool isGroup);   // 退群/删好友后清理
    int totalUnread() const;

signals:
    // ---- TCP 生命周期 ----
    void tcpLoggedIn();            // 101 之后: 上层应做全量 HTTP 对齐(好友/群/申请)
    void tcpError(const QString &reason);

    // ---- 消息流 ----
    void messageIncoming(const QString &convKey, const ChatMessage &msg);
    void messageLocalAdded(const QString &convKey, const ChatMessage &msg);
    void messageStatusChanged(const QString &convKey, quint64 clientMsgId,
                              MessageStatus status, const QString &errorText);
    void historyLoaded(const QString &convKey, const QList<ChatMessage> &msgs,
                       quint64 oldestSeq, bool reachedEnd);

    // ---- 会话列表 ----
    void conversationChanged(const QString &convKey, const Conversation &conv);
    void totalUnreadChanged(int total);

    // ---- 好友事件(130/131/132, payload 为二次解析后的 content JSON) ----
    void friendRequestEvent(const QJsonObject &payload);
    void friendRequestResultEvent(const QJsonObject &payload);
    void friendListChangedEvent(const QJsonObject &payload);

    // ---- 群事件(140~144) ----
    void groupMemberChangedEvent(const QJsonObject &payload);
    void groupDismissedEvent(const QJsonObject &payload);
    void groupInfoChangedEvent(const QJsonObject &payload);
    void groupRequestEvent(const QJsonObject &payload);       // 143
    void groupRequestResultEvent(const QJsonObject &payload); // 144

private slots:
    void onFrame(int type, const QJsonObject &frame);

private:
    struct PeerState {
        QList<ChatMessage> items;              // 按 seq 升序
        QSet<QString> seen;                    // "conv:seq" 去重
        Conversation conv;
        bool firstHistoryLoaded = false;
        quint64 lastSeq = 0;                   // 已收到的最大 seq(已读回执用)
        quint64 convId = 0;
    };

    struct PendingSend {
        quint64 clientMsgId;
        QString key;
        bool isGroup;
    };
    struct HistoryReq {
        quint64 peerId;
        bool isGroup;
        quint64 beforeSeq;
    };

    void sendDelivered(const ChatMessage &m);
    void handleSendAck(const QJsonObject &frame);
    void handleError(const QJsonObject &frame);
    void handleHistoryResp(const QJsonObject &frame);
    void upsertMessage(PeerState &st, const ChatMessage &m);
    void touchConversation(PeerState &st, const ChatMessage &m, bool countUnread);
    static QJsonObject parseEventContent(const QJsonObject &frame);

    TcpClient *m_tcp;
    Session *m_session;
    QHash<QString, PeerState> m_peers;         // key → 会话状态
    QList<PendingSend> m_pendingSends;         // SendAck/Error 配对 FIFO
    QList<HistoryReq> m_historyFifo;           // HistoryResp 配对 FIFO
    quint64 m_msgIdCounter = 0;
};

#endif // CXIM_CHATSERVICE_H
