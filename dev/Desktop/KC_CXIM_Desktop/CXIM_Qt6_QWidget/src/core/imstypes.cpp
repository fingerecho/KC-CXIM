#include "imstypes.h"

#include <QJsonDocument>

// Qt 的 uint64 在 QJsonValue 中是 double, 统一走 toDouble()+取整转换
static inline quint64 ju64(const QJsonObject &o, const char *key, quint64 def = 0)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isDouble())
        return static_cast<quint64>(v.toDouble());
    if (v.isString())     // 容错: 个别接口可能以字符串下发 ID
        return v.toString().toULongLong();
    return def;
}

static inline QString js(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

static inline int ji(const QJsonObject &o, const char *key, int def = 0)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? static_cast<int>(v.toDouble()) : def;
}

static inline bool jb(const QJsonObject &o, const char *key, bool def = false)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (v.isBool())
        return v.toBool();
    if (v.isDouble())     // 协议 5.2: is_group 接受 bool 或 int
        return v.toDouble() != 0;
    return def;
}

// ---------------------------------------------------------------------------
QString UserCard::displayName() const
{
    if (!nickname.isEmpty())
        return nickname;
    if (!username.isEmpty())
        return username;
    return QString::number(userId);
}

UserCard UserCard::fromJson(const QJsonObject &o)
{
    UserCard u;
    u.userId    = ju64(o, "userId");
    u.username  = js(o, "username");
    u.nickname  = js(o, "nickname");
    u.avatarUrl = js(o, "avatarUrl");
    return u;
}

// ---------------------------------------------------------------------------
QString FriendInfo::displayName() const
{
    if (!remark.isEmpty())
        return remark;
    return UserCard{userId, username, nickname, avatarUrl}.displayName();
}

FriendInfo FriendInfo::fromJson(const QJsonObject &o)
{
    FriendInfo f;
    f.userId    = ju64(o, "userId");
    f.username  = js(o, "username");
    f.nickname  = js(o, "nickname");
    f.avatarUrl = js(o, "avatarUrl");
    f.remark    = js(o, "remark");
    f.status    = ji(o, "status", 1);
    return f;
}

// ---------------------------------------------------------------------------
QString FriendRequestInfo::requesterName() const
{
    if (!requesterNickname.isEmpty())
        return requesterNickname;
    if (!requesterUsername.isEmpty())
        return requesterUsername;
    return QString::number(requesterId);
}

QString FriendRequestInfo::targetName() const
{
    if (!targetNickname.isEmpty())
        return targetNickname;
    if (!targetUsername.isEmpty())
        return targetUsername;
    return QString::number(targetId);
}

FriendRequestInfo FriendRequestInfo::fromJson(const QJsonObject &o)
{
    FriendRequestInfo r;
    r.requestId         = ju64(o, "requestId");
    r.requesterId       = ju64(o, "requesterId");
    r.targetId          = ju64(o, "targetId");
    r.requesterUsername = js(o, "requesterUsername");
    r.requesterNickname = js(o, "requesterNickname");
    r.targetUsername    = js(o, "targetUsername");
    r.targetNickname    = js(o, "targetNickname");
    r.message           = js(o, "message");
    r.status            = ji(o, "status");
    // 注意: 单条接口(4.6)字段为 createdTs, 批量接口(4.8)为 createdAt, 两者都兼容
    r.createdTs = ju64(o, "createdTs");
    if (r.createdTs == 0)
        r.createdTs = ju64(o, "createdAt");
    return r;
}

// ---------------------------------------------------------------------------
GroupInfo GroupInfo::fromJson(const QJsonObject &o)
{
    GroupInfo g;
    g.groupId        = ju64(o, "groupId");
    g.conversationId = ju64(o, "conversationId");
    g.name           = js(o, "name");
    g.announcement   = js(o, "announcement");
    g.intro          = js(o, "intro");
    g.avatarUrl      = js(o, "avatarUrl");
    g.ownerId        = ju64(o, "ownerId");
    g.status         = ji(o, "status");
    g.memberLimit    = ji(o, "memberLimit", 200);
    g.memberCount    = ji(o, "memberCount");
    g.myRole         = ji(o, "myRole");
    g.createdTs      = ju64(o, "createdTs");
    return g;
}

// ---------------------------------------------------------------------------
QString GroupMember::displayName() const
{
    if (!groupNickname.isEmpty())
        return groupNickname;
    if (!nickname.isEmpty())
        return nickname;
    if (!username.isEmpty())
        return username;
    return QString::number(userId);
}

GroupMember GroupMember::fromJson(const QJsonObject &o)
{
    GroupMember m;
    m.userId        = ju64(o, "userId");
    m.username      = js(o, "username");
    m.nickname      = js(o, "nickname");
    m.avatarUrl     = js(o, "avatarUrl");
    m.groupNickname = js(o, "groupNickname");
    m.role          = ji(o, "role", 3);
    m.joinedTs      = ju64(o, "joinedTs");
    return m;
}

// ---------------------------------------------------------------------------
QString GroupRequestInfo::requesterName() const
{
    if (!requesterNickname.isEmpty())
        return requesterNickname;
    if (!requesterUsername.isEmpty())
        return requesterUsername;
    return QString::number(requesterId);
}

GroupRequestInfo GroupRequestInfo::fromJson(const QJsonObject &o)
{
    GroupRequestInfo r;
    r.requestId         = ju64(o, "requestId");
    r.groupId           = ju64(o, "groupId");
    r.groupName         = js(o, "groupName");
    r.ownerId           = ju64(o, "ownerId");
    r.requesterId       = ju64(o, "requesterId");
    r.requesterUsername = js(o, "requesterUsername");
    r.requesterNickname = js(o, "requesterNickname");
    r.handlerId         = ju64(o, "handlerId");
    r.message           = js(o, "message");
    r.status            = ji(o, "status");
    r.createdTs         = ju64(o, "createdTs");
    return r;
}

// ---------------------------------------------------------------------------
ChatMessage ChatMessage::fromFrame(const QJsonObject &o)
{
    ChatMessage m;
    m.msgId           = ju64(o, "msg_id");
    m.clientMsgId     = m.msgId;      // 下行帧中 msg_id 即服务端 ID
    m.conversationId  = ju64(o, "conversation_id");
    m.seq             = ju64(o, "seq");
    m.fromUid         = ju64(o, "from");
    m.toUid           = ju64(o, "to");
    m.type            = ji(o, "type", MsgType::Text);
    m.content         = js(o, "content");
    m.ts              = ju64(o, "ts");
    m.isGroup         = jb(o, "is_group");
    return m;
}

QJsonObject ChatMessage::toFrame(quint64 senderUid) const
{
    // 协议 5.2: 上行必填 from/type; from 允许 0(服务端以 JWT 身份为准)
    QJsonObject o;
    o[QStringLiteral("from")] = static_cast<double>(senderUid ? senderUid : fromUid);
    o[QStringLiteral("to")]   = static_cast<double>(toUid);
    o[QStringLiteral("type")] = type;
    o[QStringLiteral("msg_id")] = static_cast<double>(clientMsgId ? clientMsgId : msgId);
    if (ts)
        o[QStringLiteral("ts")] = static_cast<double>(ts);
    if (conversationId)
        o[QStringLiteral("conversation_id")] = static_cast<double>(conversationId);
    if (seq)
        o[QStringLiteral("seq")] = static_cast<double>(seq);
    if (!content.isEmpty())
        o[QStringLiteral("content")] = content;
    if (isGroup)
        o[QStringLiteral("is_group")] = true;
    return o;
}

QJsonObject ChatMessage::mediaMeta() const
{
    if (type != MsgType::Image && type != MsgType::File)
        return {};
    // 服务端已将 content 重建为权威元数据 JSON(协议 5.8)
    return QJsonDocument::fromJson(content.toUtf8()).object();
}

QString ChatMessage::displayPreview() const
{
    switch (type) {
    case MsgType::Image: {
        const QJsonObject meta = mediaMeta();
        const QString name = meta.value(QStringLiteral("name")).toString();
        return QStringLiteral("[图片] %1").arg(name.isEmpty() ? QStringLiteral("[图片]") : name);
    }
    case MsgType::File: {
        const QJsonObject meta = mediaMeta();
        return QStringLiteral("[文件] %1").arg(meta.value(QStringLiteral("name")).toString());
    }
    default:
        return content;
    }
}

// ---------------------------------------------------------------------------
MediaMeta MediaMeta::fromJson(const QJsonObject &o)
{
    MediaMeta m;
    m.fileId = o.value(QLatin1String("fileId")).toString();
    if (m.fileId.isEmpty())
        m.fileId = o.value(QLatin1String("file_id")).toString();  // commit 响应为 fileId, 消息元数据为 file_id
    m.url   = o.value(QLatin1String("url")).toString();
    m.name  = o.value(QLatin1String("name")).toString();
    m.size  = static_cast<qint64>(o.value(QLatin1String("size")).toDouble());
    m.mime  = o.value(QLatin1String("mime")).toString();
    m.qetag = o.value(QLatin1String("qetag")).toString();
    return m;
}

QJsonObject MediaMeta::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("file_id")] = fileId;
    o[QStringLiteral("url")]     = url;
    o[QStringLiteral("name")]    = name;
    o[QStringLiteral("size")]    = static_cast<double>(size);
    o[QStringLiteral("mime")]    = mime;
    o[QStringLiteral("qetag")]   = qetag;
    return o;
}

bool MediaMeta::isImage() const
{
    return mime.startsWith(QStringLiteral("image/"));
}

// ---------------------------------------------------------------------------
QString Conversation::key() const
{
    return (isGroup ? QStringLiteral("g:") : QStringLiteral("s:")) + QString::number(peerId);
}
