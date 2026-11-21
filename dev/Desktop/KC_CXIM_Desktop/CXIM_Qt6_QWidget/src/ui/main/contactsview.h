#ifndef CXIM_CONTACTSVIEW_H
#define CXIM_CONTACTSVIEW_H

#include <QWidget>

#include "../../service/imservice.h"

/**
 * @brief 通讯录页(对照 desktop_real_add_group_ui_0.png)
 *
 * 布局:
 *  - 顶部: 搜索框(占位"搜索", 回车弹 AddFriendDialog 并带入关键词) + "+"按钮;
 *  - 标签栏: [好友 | 群聊] 两个互斥 tabButton;
 *  - 好友列表行: 头像 | 备注名/昵称 + ID; 双击 → openConversation;
 *    右键菜单: 发消息 / 设置备注 / 删除好友 / 拉黑;
 *    顶部若有待处理申请显示 banner "新朋友 (N)" → FriendRequestsDialog;
 *  - 群列表行: 群图标 | 群名 + "N 人"; 双击 → openConversation(群);
 *    右键菜单: 管理群聊(GroupManageDialog);
 *    若我是群主/管理员且有入群申请显示 banner "入群申请 (N)" → GroupRequestsDialog;
 *  - 底部: 群聊标签下显示 primaryButton "+ 新建群聊" → CreateGroupDialog。
 *
 * 数据: 自持 m_friends/m_groups 缓存, syncAll() 做登录后全量对齐;
 * TCP 事件到达时 MainWindow 调 refreshFriends()/refreshGroups() 定向重拉。
 * 展示名解析(displayNameOf)供会话列表标题回调复用, 保持"名字只有一份口径"。
 */
class ContactsView : public QWidget
{
    Q_OBJECT
public:
    explicit ContactsView(ImService *im, QWidget *parent = nullptr);

    void syncAll();                       // 好友+群+两类申请 全量重拉(TCP 登录后)
    void refreshFriends();
    void refreshGroups();
    QString displayNameOf(quint64 peerId, bool isGroup) const;

signals:
    void openConversation(quint64 peerId, bool isGroup, const QString &title);

private slots:
    void onSearchEntered();
    void onAddClicked();
    void onTabChanged(bool groups);

private:
    void buildUi();
    void renderFriends();       // 用缓存的申请数渲染 banner + 好友列表
    void renderGroups();
    void showFriendMenu(const QPoint &pos);
    void showGroupMenu(const QPoint &pos);
    void setFriendRemark(quint64 id, const QString &current);

    ImService *m_im;

    class QLineEdit *m_search = nullptr;
    class QPushButton *m_addBtn = nullptr;
    class QPushButton *m_tabFriends = nullptr;
    class QPushButton *m_tabGroups = nullptr;
    class QListWidget *m_friendsList = nullptr;
    class QListWidget *m_groupsList = nullptr;
    class QStackedWidget *m_lists = nullptr;
    class QPushButton *m_newGroupBtn = nullptr;

    QList<FriendInfo> m_friends;
    QList<GroupInfo> m_groups;
    int m_pendingFriendReqs = 0;
    int m_pendingGroupReqs = 0;
};

#endif // CXIM_CONTACTSVIEW_H
