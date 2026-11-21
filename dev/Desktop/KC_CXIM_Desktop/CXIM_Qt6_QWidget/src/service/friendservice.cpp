#include "friendservice.h"

FriendService::FriendService(HttpClient *http, QObject *parent)
    : QObject(parent)
    , m_http(http)
{
}

void FriendService::searchUsers(const QString &keyword, UsersCb cb)
{
    if (keyword.trimmed().isEmpty()) { cb(false, {}, QStringLiteral("missing keyword")); return; }
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("keyword"), keyword.trimmed());
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("20"));
    m_http->get(QStringLiteral("/api/users/search"), q, [cb](const ApiResult &res) {
        if (!res.ok) { cb(false, {}, res.message); return; }
        cb(true, parseJsonArray<UserCard>(res.objectData().value(QStringLiteral("users"))),
           QString());
    });
}

void FriendService::listFriends(FriendsCb cb)
{
    m_http->get(QStringLiteral("/api/friends"), QUrlQuery(), [cb](const ApiResult &res) {
        if (!res.ok) { cb(false, {}, res.message); return; }
        cb(true, parseJsonArray<FriendInfo>(res.objectData().value(QStringLiteral("friends"))),
           QString());
    });
}

void FriendService::listRequests(const QString &direction, ReqsCb cb)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("direction"),
                   direction.isEmpty() ? QStringLiteral("incoming") : direction);
    m_http->get(QStringLiteral("/api/friends/requests"), q, [cb](const ApiResult &res) {
        if (!res.ok) { cb(false, {}, res.message); return; }
        cb(true, parseJsonArray<FriendRequestInfo>(
                     res.objectData().value(QStringLiteral("requests"))), QString());
    });
}

void FriendService::sendRequest(quint64 toUserId, const QString &message, SimpleCb cb)
{
    QJsonObject body;
    body[QStringLiteral("toUserId")] = static_cast<double>(toUserId);
    body[QStringLiteral("message")] = message.left(85);   // 服务端限 255 字节, 中文按 3 字节保守截断
    m_http->post(QStringLiteral("/api/friends/requests"), body,
                 [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void FriendService::sendRequestsBatch(const QList<quint64> &toUserIds,
                                      const QString &message, SimpleCb cb)
{
    if (toUserIds.isEmpty() || toUserIds.size() > 200) {   // 协议 4.8 上限
        cb(false, QStringLiteral("too many friendIds"));
        return;
    }
    QJsonArray arr;
    for (quint64 id : toUserIds)
        arr.append(static_cast<double>(id));
    QJsonObject body;
    body[QStringLiteral("toUserIds")] = arr;
    body[QStringLiteral("message")] = message;
    m_http->post(QStringLiteral("/api/friends/requests/batch"), body,
                 [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void FriendService::acceptRequest(quint64 requestId, SimpleCb cb)
{
    m_http->post(QStringLiteral("/api/friends/requests/%1/accept").arg(requestId),
                 QJsonObject(), [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void FriendService::rejectRequest(quint64 requestId, SimpleCb cb)
{
    m_http->post(QStringLiteral("/api/friends/requests/%1/reject").arg(requestId),
                 QJsonObject(), [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void FriendService::setRemark(quint64 friendId, const QString &remark, SimpleCb cb)
{
    if (remark.toUtf8().size() > 64) { cb(false, QStringLiteral("remark too long")); return; }
    QJsonObject body;
    body[QStringLiteral("remark")] = remark;
    m_http->patch(QStringLiteral("/api/friends/%1").arg(friendId), body,
                  [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void FriendService::removeFriend(quint64 friendId, SimpleCb cb)
{
    m_http->del(QStringLiteral("/api/friends/%1").arg(friendId),
                [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void FriendService::blockFriend(quint64 friendId, SimpleCb cb)
{
    m_http->post(QStringLiteral("/api/friends/%1/block").arg(friendId),
                 QJsonObject(), [cb](const ApiResult &res) { cb(res.ok, res.message); });
}

void FriendService::unblockFriend(quint64 friendId, SimpleCb cb)
{
    m_http->post(QStringLiteral("/api/friends/%1/unblock").arg(friendId),
                 QJsonObject(), [cb](const ApiResult &res) { cb(res.ok, res.message); });
}
