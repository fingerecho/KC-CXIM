#include "groupservice.h"

GroupService::GroupService(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

QJsonArray GroupService::idArray(const QList<quint64> &ids)
{
    QJsonArray arr;
    for (quint64 id : ids)
        arr.append(static_cast<double>(id));
    return arr;
}

void GroupService::createGroup(const QString &name, const QList<quint64> &memberIds, DetailCb cb)
{
    if (name.trimmed().isEmpty() || name.toUtf8().size() > 128 || memberIds.isEmpty()) {
        cb(false, {}, QStringLiteral("invalid group name or empty members"));
        return;
    }
    QJsonObject body;
    body[QStringLiteral("name")] = name.trimmed();
    body[QStringLiteral("memberIds")] = idArray(memberIds);
    m_http->post(QStringLiteral("/api/groups"), body, [cb](const ApiResult &res) {
        if (!res.ok) { cb(false, {}, res.message); return; }
        GroupDetail d;
        d.group = GroupInfo::fromJson(res.objectData().value(QStringLiteral("group")).toObject());
        cb(true, d, QString());
    });
}

void GroupService::listGroups(GroupsCb cb)
{
    m_http->get(QStringLiteral("/api/groups"), QUrlQuery(), [cb](const ApiResult &res) {
        if (!res.ok) { cb(false, {}, res.message); return; }
        cb(true, parseJsonArray<GroupInfo>(
                     res.objectData().value(QStringLiteral("groups"))), QString());
    });
}

void GroupService::groupDetail(quint64 groupId, DetailCb cb)
{
    m_http->get(QStringLiteral("/api/groups/%1").arg(groupId), QUrlQuery(),
                [cb](const ApiResult &res) {
        if (!res.ok) { cb(false, {}, res.message); return; }
        const QJsonObject d = res.objectData();
        GroupDetail detail;
        detail.group = GroupInfo::fromJson(d.value(QStringLiteral("group")).toObject());
        detail.members = parseJsonArray<GroupMember>(d.value(QStringLiteral("members")));
        cb(true, detail, QString());
    });
}

void GroupService::updateGroup(quint64 groupId, const QString &name,
                               const QString &announcement, const QString &intro,
                               SimpleCb cb)
{
    QJsonObject body;
    body[QStringLiteral("name")] = name;
    body[QStringLiteral("announcement")] = announcement;
    body[QStringLiteral("intro")] = intro;
    body[QStringLiteral("avatarUrl")] = QString();
    m_http->patch(QStringLiteral("/api/groups/%1").arg(groupId), body,
                  [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void GroupService::inviteMembers(quint64 groupId, const QList<quint64> &memberIds, DetailCb cb)
{
    QJsonObject body;
    body[QStringLiteral("memberIds")] = idArray(memberIds);
    m_http->post(QStringLiteral("/api/groups/%1/members").arg(groupId), body,
                 [cb](const ApiResult &res) {
        if (!res.ok) { cb(false, {}, res.message); return; }
        const QJsonObject d = res.objectData();
        GroupDetail detail;
        detail.group = GroupInfo::fromJson(d.value(QStringLiteral("group")).toObject());
        detail.members = parseJsonArray<GroupMember>(d.value(QStringLiteral("members")));
        cb(true, detail, QString());
    });
}

void GroupService::removeMember(quint64 groupId, quint64 userId, SimpleCb cb)
{
    m_http->del(QStringLiteral("/api/groups/%1/members/%2").arg(groupId).arg(userId),
                [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void GroupService::setAdmin(quint64 groupId, quint64 userId, bool admin, SimpleCb cb)
{
    QJsonObject body;
    body[QStringLiteral("userId")] = static_cast<double>(userId);
    body[QStringLiteral("admin")] = admin;
    m_http->post(QStringLiteral("/api/groups/%1/admins").arg(groupId), body,
                 [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void GroupService::transferOwner(quint64 groupId, quint64 toUserId, SimpleCb cb)
{
    QJsonObject body;
    body[QStringLiteral("toUserId")] = static_cast<double>(toUserId);
    m_http->post(QStringLiteral("/api/groups/%1/transfer").arg(groupId), body,
                 [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void GroupService::quitGroup(quint64 groupId, SimpleCb cb)
{
    m_http->post(QStringLiteral("/api/groups/%1/quit").arg(groupId), QJsonObject(),
                 [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void GroupService::dismissGroup(quint64 groupId, SimpleCb cb)
{
    m_http->post(QStringLiteral("/api/groups/%1/dismiss").arg(groupId), QJsonObject(),
                 [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void GroupService::requestJoin(quint64 groupId, const QString &message, SimpleCb cb)
{
    QJsonObject body;
    body[QStringLiteral("groupId")] = static_cast<double>(groupId);
    body[QStringLiteral("message")] = message;
    m_http->post(QStringLiteral("/api/groups/requests"), body,
                 [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void GroupService::listJoinRequests(const QString &direction, ReqsCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("direction"),
                   direction.isEmpty() ? QStringLiteral("incoming") : direction);
    m_http->get(QStringLiteral("/api/groups/requests"), q, [cb](const ApiResult &res) {
        if (!res.ok) { cb(false, {}, res.message); return; }
        cb(true, parseJsonArray<GroupRequestInfo>(
                     res.objectData().value(QStringLiteral("requests"))), QString());
    });
}

void GroupService::acceptJoin(quint64 requestId, SimpleCb cb)
{
    m_http->post(QStringLiteral("/api/groups/requests/%1/accept").arg(requestId),
                 QJsonObject(), [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void GroupService::rejectJoin(quint64 requestId, SimpleCb cb)
{
    m_http->post(QStringLiteral("/api/groups/requests/%1/reject").arg(requestId),
                 QJsonObject(), [cb](const ApiResult &res) { cb(res.ok, res.message); });
}
