#ifndef CXIM_FRIENDSERVICE_H
#define CXIM_FRIENDSERVICE_H

#include <QObject>
#include "../net/httpclient.h"
#include "../core/imstypes.h"

/**
 * @file friendservice.h
 * @brief 好友业务(协议 4.4~4.13): 搜索 / 列表 / 申请 / 备注 / 删除 / 拉黑
 *
 * 全部为无状态薄封装: 参数校验 + 路径拼装 + 响应转业务模型;
 * 好友关系的实时变化由 TCP 事件(130/131/132)驱动 ChatService 触发
 * refreshFriends() 全量对齐(协议 5.15: 事件不承载事实)。
 */
class FriendService : public QObject
{
    Q_OBJECT
public:
    explicit FriendService(HttpClient *http, QObject *parent = nullptr);

    using UsersCb   = std::function<void(bool ok, const QList<UserCard> &users, const QString &err)>;
    using FriendsCb = std::function<void(bool ok, const QList<FriendInfo> &friends, const QString &err)>;
    using ReqsCb    = std::function<void(bool ok, const QList<FriendRequestInfo> &reqs, const QString &err)>;
    using SimpleCb  = std::function<void(bool ok, const QString &err)>;

    void searchUsers(const QString &keyword, UsersCb cb);            // GET /api/users/search
    void listFriends(FriendsCb cb);                                   // GET /api/friends
    /// direction: incoming(默认,收到的) / outgoing / all
    void listRequests(const QString &direction, ReqsCb cb);
    void sendRequest(quint64 toUserId, const QString &message, SimpleCb cb);
    void sendRequestsBatch(const QList<quint64> &toUserIds, const QString &message, SimpleCb cb);
    void acceptRequest(quint64 requestId, SimpleCb cb);
    void rejectRequest(quint64 requestId, SimpleCb cb);
    void setRemark(quint64 friendId, const QString &remark, SimpleCb cb);   // 客户端预检 ≤64 字节
    void removeFriend(quint64 friendId, SimpleCb cb);
    void blockFriend(quint64 friendId, SimpleCb cb);
    void unblockFriend(quint64 friendId, SimpleCb cb);

private:
    HttpClient *m_http;
};

#endif // CXIM_FRIENDSERVICE_H
