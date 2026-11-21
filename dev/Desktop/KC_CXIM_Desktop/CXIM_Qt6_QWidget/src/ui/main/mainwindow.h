#ifndef CXIM_MAINWINDOW_H
#define CXIM_MAINWINDOW_H

#include "../common/framelesswindow.h"

#include "../../service/imservice.h"
#include "../chat/chatview.h"

class ConversationListView;
class ContactsView;

/**
 * @brief 主窗口(对照 desktop_real_group_ui_0.png / desktop_real_add_group_ui_0.png)
 *
 * 布局:
 *  ┌ headerBar: logo+铖信 | 我的头像 用户名 ID | 最小化/最大化/关闭 ┐
 *  ├ railPanel: 💬消息  👤通讯录 (两个 checkable 图标按钮)          │
 *  ├ Page0 消息: ConversationListView(280px) + ChatView            │
 *  └ Page1 通讯录: ContactsView(搜索/+/好友|群聊/列表/新建群聊)     │
 *
 * 职责边界(UI 组装层):
 *  - 只做"页面切换 + 信号转发 + 全量同步编排", 不含任何协议细节;
 *  - TCP 登录成功(tcpLoggedIn)后触发 ContactsView::syncAll() 全量对齐;
 *  - 好友/群事件(132/140~144)→ 定向重拉对应列表(事件只是"变了"的通知);
 *  - 打开群会话时拉群详情注入 ChatView(公告/成员名/管理入口);
 *  - sessionExpired → 弹提示并 emit logoutRequested()(由 main 切回登录页)。
 */
class MainWindow : public FramelessWindow
{
    Q_OBJECT
public:
    explicit MainWindow(ImService *im, QWidget *parent = nullptr);

signals:
    void logoutRequested();

private slots:
    void openConversation(quint64 peerId, bool isGroup, const QString &title);
    void onSessionExpired();

private:
    QWidget *buildHeader();     // 顶部标题栏(返回给 root 布局)
    QWidget *buildPages();      // rail + 消息页/通讯录页
    void wireSignals();

    ImService *m_im;

    // 页面
    ConversationListView *m_convList = nullptr;
    ChatView *m_chat = nullptr;
    ContactsView *m_contacts = nullptr;
    class QStackedWidget *m_pages = nullptr;
    class QToolButton *m_railChat = nullptr;
    class QToolButton *m_railContacts = nullptr;
};

#endif // CXIM_MAINWINDOW_H
