#ifndef CXIM_IMSTYPES_H
#define CXIM_IMSTYPES_H

#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QList>
#include <QMetaType>

/**
 * @file imstypes.h
 * @brief IMS 业务数据模型(与 docs/IMS_communication_protocol.md 第 4/5/7 章字段一一对应)
 *
 * 设计要点:
 *  - 本文件只描述"数据", 不包含任何网络/UI 逻辑, 供 net/service/ui 三层共用;
 *  - 每个结构体提供 fromJson()/toJson(), JSON 字段名严格采用协议文档口径,
 *    服务端字段调整时只需改本文件的解析函数, 不影响其他层;
 *  - 使用 Q_DECLARE_METATYPE 注册后, 结构体可直接跨线程 queued signal 传递。
 */

// ---------------------------------------------------------------------------
// TCP 消息类型 (协议 5.3)
// ---------------------------------------------------------------------------
namespace MsgType {
enum Type {
    Text            = 1,    // 文本消息(双向)
    Image           = 2,    // 图片消息(双向, content 为媒体元数据 JSON)
    Heartbeat       = 3,    // 心跳(客户端→服务端)
    File            = 4,    // 文件消息(双向, content 为媒体元数据 JSON)
    Login           = 100,  // TCP 登录(客户端→服务端)
    LoginAck        = 101,  // 登录成功(服务端→客户端)
    Error           = 102,  // 错误通知(服务端→客户端)
    HistoryReq      = 110,  // 历史消息请求
    HistoryResp     = 111,  // 历史消息响应
    Ack             = 112,  // 投递/已读回执(客户端→服务端)
    SendAck         = 113,  // 发送成功确认(服务端→客户端)
    FriendRequest   = 130,  // 收到好友申请/登录补推
    FriendRequestResult = 131,
    FriendListChanged   = 132,
    GroupMemberChanged  = 140,
    GroupDismissed      = 141,
    GroupInfoChanged    = 142,
    GroupRequest        = 143,
    GroupRequestResult  = 144,
};
}

// ---------------------------------------------------------------------------
// 会话内消息的本地状态(仅客户端视角, 不上线)
// ---------------------------------------------------------------------------
enum class MessageStatus {
    Sending,    // 已入本地队列, 未收到 SendAck
    Sent,       // SendAck content=="sent"
    Duplicate,  // SendAck content=="duplicate"(去重命中, 等价成功)
    Failed,     // Error 帧判定失败
};

// ---------------------------------------------------------------------------
// 用户公开资料(搜索结果 / 申请列表中的展示信息)
// ---------------------------------------------------------------------------
struct UserCard {
    quint64 userId = 0;
    QString username;
    QString nickname;
    QString avatarUrl;

    static UserCard fromJson(const QJsonObject &o);

    QString displayName() const;   // nickname > username > ID 字符串
};

// ---------------------------------------------------------------------------
// 好友(协议 4.5)
// ---------------------------------------------------------------------------
struct FriendInfo {
    quint64 userId = 0;
    QString username;
    QString nickname;
    QString avatarUrl;
    QString remark;
    int status = 1;   // 1=好友 2=拉黑

    static FriendInfo fromJson(const QJsonObject &o);

    QString displayName() const;   // remark > nickname > username
};

// ---------------------------------------------------------------------------
// 好友申请(协议 4.6/4.8, 注意单条 createdTs / 批量 createdAt 两个口径)
// ---------------------------------------------------------------------------
struct FriendRequestInfo {
    quint64 requestId = 0;
    quint64 requesterId = 0;
    quint64 targetId = 0;
    QString requesterUsername;
    QString requesterNickname;
    QString targetUsername;
    QString targetNickname;
    QString message;
    int status = 0;           // 0待处理 1通过 2拒绝 3取消
    quint64 createdTs = 0;    // 毫秒

    static FriendRequestInfo fromJson(const QJsonObject &o);
    QString requesterName() const;
    QString targetName() const;
};

// ---------------------------------------------------------------------------
// 群(协议 4.14)
// ---------------------------------------------------------------------------
struct GroupInfo {
    quint64 groupId = 0;
    quint64 conversationId = 0;
    QString name;
    QString announcement;
    QString intro;
    QString avatarUrl;
    quint64 ownerId = 0;
    int status = 0;           // 0正常 1解散
    int memberLimit = 200;
    int memberCount = 0;
    int myRole = 0;           // 1群主 2管理员 3成员 0非成员
    quint64 createdTs = 0;

    static GroupInfo fromJson(const QJsonObject &o);
};

struct GroupMember {
    quint64 userId = 0;
    QString username;
    QString nickname;
    QString avatarUrl;
    QString groupNickname;
    int role = 3;             // 1群主 2管理员 3成员
    quint64 joinedTs = 0;

    static GroupMember fromJson(const QJsonObject &o);
    QString displayName() const;   // groupNickname > nickname > username
};

// 入群申请(协议 4.14.7 / 5.13 GroupRequest 事件字段口径)
struct GroupRequestInfo {
    quint64 requestId = 0;
    quint64 groupId = 0;
    QString groupName;
    quint64 ownerId = 0;
    quint64 requesterId = 0;
    QString requesterUsername;
    QString requesterNickname;
    quint64 handlerId = 0;
    QString message;
    int status = 0;
    quint64 createdTs = 0;

    static GroupRequestInfo fromJson(const QJsonObject &o);
    QString requesterName() const;
};

// ---------------------------------------------------------------------------
// 聊天消息(TCP Message 信封, 协议 5.2; 历史数组元素 5.9)
// ---------------------------------------------------------------------------
struct ChatMessage {
    quint64 msgId = 0;            // 服务端消息 ID(发送时为客户端临时 msg_id)
    quint64 clientMsgId = 0;      // 客户端去重键(与 msgId 同源, 单独保留便于重试复用)
    quint64 conversationId = 0;
    quint64 seq = 0;
    quint64 fromUid = 0;
    quint64 toUid = 0;            // 单聊=对方 userId; 群聊=groupId
    int type = MsgType::Text;     // 1文本 2图片 4文件
    QString content;              // 文本内容 或 媒体元数据 JSON 字符串
    quint64 ts = 0;               // 毫秒
    bool isGroup = false;
    MessageStatus status = MessageStatus::Sent;
    bool outgoing = false;        // 是否本人发出(UI 决定气泡左右)

    /** 从下行 Message 信封解析 */
    static ChatMessage fromFrame(const QJsonObject &o);
    /** 构造上行 Message 信封(协议 5.2 必填: from/type; from 可为 0) */
    QJsonObject toFrame(quint64 fromUid = 0) const;

    /** type==2/4 时解析 content 中的媒体元数据 {file_id,url,name,size,mime,qetag} */
    QJsonObject mediaMeta() const;
    QString displayPreview() const;   // 会话列表用的单行摘要
};

// ---------------------------------------------------------------------------
// 媒体文件元数据(协议 4.15.2 commit 响应 / 5.8 服务端重建元数据)
// ---------------------------------------------------------------------------
struct MediaMeta {
    QString fileId;
    QString url;        // 服务端换取入口 GET /api/files/{uuid}
    QString name;
    qint64 size = 0;
    QString mime;
    QString qetag;

    static MediaMeta fromJson(const QJsonObject &o);
    QJsonObject toJson() const;
    bool isImage() const;
};

// ---------------------------------------------------------------------------
// 会话(客户端本地聚合模型: 单聊以好友为键, 群聊以群为键)
// ---------------------------------------------------------------------------
struct Conversation {
    quint64 peerId = 0;       // 单聊=friendId, 群聊=groupId
    bool isGroup = false;
    QString title;
    QString avatarUrl;
    ChatMessage lastMessage;
    bool hasLast = false;
    int unread = 0;

    QString key() const;      // "s:<id>" / "g:<id>"
};

Q_DECLARE_METATYPE(UserCard)
Q_DECLARE_METATYPE(FriendInfo)
Q_DECLARE_METATYPE(FriendRequestInfo)
Q_DECLARE_METATYPE(GroupInfo)
Q_DECLARE_METATYPE(GroupMember)
Q_DECLARE_METATYPE(GroupRequestInfo)
Q_DECLARE_METATYPE(ChatMessage)
Q_DECLARE_METATYPE(MediaMeta)
Q_DECLARE_METATYPE(Conversation)

// 通用 JSON 数组 → QList<T> 辅助(T 需提供静态 fromJson)
template <typename T>
QList<T> parseJsonArray(const QJsonValue &v)
{
    QList<T> out;
    const QJsonArray arr = v.toArray();
    out.reserve(arr.size());
    for (const QJsonValue &item : arr)
        out.append(T::fromJson(item.toObject()));
    return out;
}

#endif // CXIM_IMSTYPES_H
