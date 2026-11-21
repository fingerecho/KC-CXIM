#ifndef CXIM_GROUPDIALOGS_H
#define CXIM_GROUPDIALOGS_H

#include <QDialog>

#include "../../core/imstypes.h"

class ImService;

/**
 * @file groupdialogs.h
 * @brief 群相关弹窗契约(实现见 groupdialogs.cpp)
 *
 * CreateGroupDialog(对照 desktop_real_add_group_ui_0.png 的"新建群聊"入口):
 *  - 群名输入(≤128 字节) + 好友多选列表(复选框行: 头像/备注名/ID);
 *  - "创建" → groups()->createGroup(name, checkedIds);
 *    成功 accept() 并 emit created(groupId, name); 失败展示服务端 message
 *    (如 "initial members must be your friends")。
 *
 * GroupManageDialog(协议 4.14 权限矩阵全部由服务端裁决, UI 仅按 myRole 隐藏入口):
 *  - 打开时 groupDetail(groupId) 拉最新群资料 + 成员表;
 *  - 上半: 群名/公告/简介展示; 群主或管理员可编辑群名与公告(保存 → updateGroup);
 *  - 成员表: 头像/展示名/ID/角色徽标(群主👑/管理员★); 行右键菜单按权限给出:
 *      任命/取消管理员(仅群主) | 移出成员(群主/管理员, 仅对 role==3) |
 *      转让群主(仅群主, 目标非自己) | 发私信(关窗并由外部打开会话);
 *  - 底部按钮: 邀请成员(仅群主/管理员, 弹好友多选, 已在群内的置灰) |
 *      退群(非群主; 群主点击提示"请先转让群主") | 解散(仅群主, 二次确认);
 *  - 任何变更后 emit groupChanged(groupId) 并 reload(), 主窗口据此刷新列表。
 *
 * GroupRequestsDialog(协议 4.14.7):
 *  - listJoinRequests("incoming") 待我审批的入群申请;
 *  - 行: 申请人 + 群名 + 附言 + 通过/拒绝; 处理后行消失;
 *  - 通过时 emit groupChanged(groupId) 便于主界面刷新成员数。
 */
class CreateGroupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit CreateGroupDialog(ImService *im, QWidget *parent = nullptr);

signals:
    void created(quint64 groupId, const QString &name);

private:
    ImService *m_im;
    class QLineEdit *m_name = nullptr;
    class QListWidget *m_friendList = nullptr;
};

class GroupManageDialog : public QDialog
{
    Q_OBJECT
public:
    GroupManageDialog(ImService *im, quint64 groupId, QWidget *parent = nullptr);

signals:
    void groupChanged(quint64 groupId);
    void requestOpenFriend(quint64 userId, const QString &title);   // 成员行"发私信"

private:
    void reload();
    void showMemberMenu(const QPoint &pos);
    void inviteMembers();

    ImService *m_im;
    quint64 m_groupId = 0;
    GroupInfo m_group;
    QList<GroupMember> m_members;

    class QLabel *m_nameLabel = nullptr;
    class QLabel *m_announcementLabel = nullptr;
    class QLineEdit *m_nameEdit = nullptr;
    class QLineEdit *m_announcementEdit = nullptr;
    class QPushButton *m_saveBtn = nullptr;
    class QListWidget *m_memberList = nullptr;
    class QPushButton *m_inviteBtn = nullptr;
    class QPushButton *m_quitBtn = nullptr;
    class QPushButton *m_dismissBtn = nullptr;
};

class GroupRequestsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit GroupRequestsDialog(ImService *im, QWidget *parent = nullptr);

signals:
    void groupChanged(quint64 groupId);

private:
    void reload();
    ImService *m_im;
    class QVBoxLayout *m_listLayout = nullptr;
};

#endif // CXIM_GROUPDIALOGS_H
