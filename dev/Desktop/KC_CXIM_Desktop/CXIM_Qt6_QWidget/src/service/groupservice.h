#ifndef CXIM_GROUPSERVICE_H
#define CXIM_GROUPSERVICE_H

#include <QObject>
#include "../net/httpclient.h"
#include "../core/imstypes.h"

/**
 * @file groupservice.h
 * @brief 群聊业务(协议 4.14): 建群 / 列表 / 详情 / 改资料 / 成员管理 / 审批
 *
 * 权限矩阵由服务端强制(4.14 权限表), 客户端只做入口隐藏, 不做二次判断,
 * 保证"服务端是唯一事实源"; 群事件(140~144)同样只作为重新拉取的触发器。
 */
class GroupService : public QObject
{
    Q_OBJECT
public:
    struct GroupDetail {
        GroupInfo group;
        QList<GroupMember> members;
    };

    explicit GroupService(HttpClient *http, QObject *parent = nullptr);

    using SimpleCb = std::function<void(bool ok, const QString &err)>;
    using GroupsCb = std::function<void(bool ok, const QList<GroupInfo> &groups, const QString &err)>;
    using DetailCb = std::function<void(bool ok, const GroupDetail &detail, const QString &err)>;
    using ReqsCb   = std::function<void(bool ok, const QList<GroupRequestInfo> &reqs, const QString &err)>;

    void createGroup(const QString &name, const QList<quint64> &memberIds, DetailCb cb);
    void listGroups(GroupsCb cb);
    void groupDetail(quint64 groupId, DetailCb cb);
    /// 空串字段表示不修改(协议 4.14.3)
    void updateGroup(quint64 groupId, const QString &name, const QString &announcement,
                     const QString &intro, SimpleCb cb);
    void inviteMembers(quint64 groupId, const QList<quint64> &memberIds, DetailCb cb);
    void removeMember(quint64 groupId, quint64 userId, SimpleCb cb);
    void setAdmin(quint64 groupId, quint64 userId, bool admin, SimpleCb cb);
    void transferOwner(quint64 groupId, quint64 toUserId, SimpleCb cb);
    void quitGroup(quint64 groupId, SimpleCb cb);
    void dismissGroup(quint64 groupId, SimpleCb cb);

    void requestJoin(quint64 groupId, const QString &message, SimpleCb cb);
    void listJoinRequests(const QString &direction, ReqsCb cb);
    void acceptJoin(quint64 requestId, SimpleCb cb);
    void rejectJoin(quint64 requestId, SimpleCb cb);

private:
    static QJsonArray idArray(const QList<quint64> &ids);
    HttpClient *m_http;
};

#endif // CXIM_GROUPSERVICE_H
