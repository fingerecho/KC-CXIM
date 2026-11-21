#include "mainwindow.h"
#include "conversationlistview.h"
#include "contactsview.h"
#include "../common/theme.h"
#include "../common/avatarwidget.h"
#include "../dialogs/groupdialogs.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QStackedWidget>
#include <QPushButton>
#include <QMessageBox>
#include <QTimer>
#include <QFrame>

MainWindow::MainWindow(ImService *im, QWidget *parent)
    : FramelessWindow(parent)
    , m_im(im)
{
    setWindowTitle(QStringLiteral("铖信"));
    resize(1100, 730);
    setMinimumSize(900, 600);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    root->addWidget(buildHeader());
    root->addWidget(buildPages(), 1);

    wireSignals();
}

// ---------------------------------------------------------------------------
QWidget *MainWindow::buildHeader()
{
    // 顶部蓝色标题栏: logo + 应用名 | 我的头像/用户名/ID | 窗口按钮
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("headerBar"));
    header->setFixedHeight(52);
    auto *h = new QHBoxLayout(header);
    h->setContentsMargins(14, 0, 6, 0);

    auto *logo = new QLabel(QStringLiteral("◉"), header);   // 品牌符号(免资源)
    logo->setStyleSheet(QStringLiteral("color:white; font-size:22px; font-weight:bold;"));
    h->addWidget(logo);
    auto *appName = new QLabel(QStringLiteral(" 铖信"), header);
    h->addWidget(appName);
    h->addSpacing(24);

    auto *me = new AvatarWidget(header);
    me->setAvatar(30, m_im->session()->username(), m_im->session()->userId());
    h->addWidget(me);
    auto *who = new QLabel(QStringLiteral(" %1  %2")
                               .arg(m_im->session()->username())
                               .arg(m_im->session()->userId()), header);
    h->addWidget(who);
    h->addStretch(1);

    auto *minBtn = new QPushButton(QStringLiteral("─"), header);
    auto *maxBtn = new QPushButton(QStringLiteral("◌"), header);
    auto *closeBtn = new QPushButton(QStringLiteral("✕"), header);
    closeBtn->setProperty("winClose", true);
    h->addWidget(minBtn);
    h->addWidget(maxBtn);
    h->addWidget(closeBtn);

    connect(minBtn, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(maxBtn, &QPushButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);

    setTitleBar(header);   // 无边框拖动区
    return header;
}

QWidget *MainWindow::buildPages()
{
    auto *body = new QWidget(this);
    auto *bh = new QHBoxLayout(body);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->setSpacing(0);

    // ---- 左侧图标栏(消息/通讯录) ----
    auto *rail = new QWidget(body);
    rail->setObjectName(QStringLiteral("railPanel"));
    rail->setFixedWidth(64);
    auto *rv = new QVBoxLayout(rail);
    rv->setContentsMargins(10, 16, 10, 10);
    rv->setSpacing(14);

    m_railChat = new QToolButton(rail);
    m_railChat->setObjectName(QStringLiteral("railButton"));
    m_railChat->setText(QStringLiteral("💬"));
    m_railChat->setToolTip(QStringLiteral("消息"));
    m_railChat->setCheckable(true);
    m_railChat->setChecked(true);

    m_railContacts = new QToolButton(rail);
    m_railContacts->setObjectName(QStringLiteral("railButton"));
    m_railContacts->setText(QStringLiteral("👤"));
    m_railContacts->setToolTip(QStringLiteral("通讯录"));
    m_railContacts->setCheckable(true);

    rv->addWidget(m_railChat);
    rv->addWidget(m_railContacts);
    rv->addStretch(1);

    connect(m_railChat, &QToolButton::clicked, this, [this] {
        m_pages->setCurrentIndex(0);
        m_railChat->setChecked(true);
        m_railContacts->setChecked(false);
    });
    connect(m_railContacts, &QToolButton::clicked, this, [this] {
        m_pages->setCurrentIndex(1);
        m_railContacts->setChecked(true);
        m_railChat->setChecked(false);
    });

    // ---- 页面堆栈 ----
    m_pages = new QStackedWidget(body);

    // Page0: 会话列表 + 聊天
    auto *chatPage = new QWidget(m_pages);
    auto *ch = new QHBoxLayout(chatPage);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->setSpacing(0);
    m_convList = new ConversationListView(m_im, chatPage);
    m_convList->setFixedWidth(280);
    m_convList->setTitleResolver([this](quint64 peer, bool isGroup) {
        return m_contacts->displayNameOf(peer, isGroup);
    });
    m_chat = new ChatView(m_im, chatPage);
    ch->addWidget(m_convList);
    ch->addWidget(m_chat, 1);

    auto *splitterLine = new QFrame(chatPage);
    splitterLine->setFrameShape(QFrame::VLine);
    splitterLine->setStyleSheet(QStringLiteral("color:%1;").arg(Theme::divider().name()));
    ch->insertWidget(1, splitterLine);

    // Page1: 通讯录
    m_contacts = new ContactsView(m_im, m_pages);
    m_pages->addWidget(chatPage);
    m_pages->addWidget(m_contacts);

    bh->addWidget(rail);
    bh->addWidget(m_pages, 1);

    connect(m_convList, &ConversationListView::activated,
            this, &MainWindow::openConversation);
    connect(m_contacts, &ContactsView::openConversation,
            this, &MainWindow::openConversation);

    return body;
}

// ---------------------------------------------------------------------------
void MainWindow::openConversation(quint64 peerId, bool isGroup, const QString &title)
{
    m_pages->setCurrentIndex(0);
    m_railChat->setChecked(true);
    m_railContacts->setChecked(false);

    m_chat->openConversation(peerId, isGroup, title);
    if (isGroup) {
        // 群上下文(公告/成员名/管理入口): 打开时拉详情, 失败则退化为普通标题
        m_im->groups()->groupDetail(peerId, [this, peerId](bool ok,
                const GroupService::GroupDetail &d, const QString &) {
            if (ok && m_chat->currentPeerId() == peerId && m_chat->currentIsGroup())
                m_chat->setGroupContext(d.group, d.members);
        });
    } else {
        m_chat->clearGroupContext();
    }
    m_convList->selectConversation(peerId, isGroup);
}

void MainWindow::wireSignals()
{
    ChatService *chat = m_im->chat();

    // TCP(重)登录成功 → 全量对齐(协议 5.15: 事件不持久化, HTTP 全量是事实源)
    connect(chat, &ChatService::tcpLoggedIn, m_contacts, &ContactsView::syncAll);

    // 好友事件: 132=列表变化(不持久化) → 重拉; 130=新申请 → 红点/banner
    connect(chat, &ChatService::friendListChangedEvent, m_contacts,
            [this](const QJsonObject &) { m_contacts->refreshFriends(); });
    connect(chat, &ChatService::friendRequestEvent, m_contacts,
            [this](const QJsonObject &) { m_contacts->refreshFriends(); });
    connect(chat, &ChatService::friendRequestResultEvent, m_contacts,
            [this](const QJsonObject &) { m_contacts->refreshFriends(); });

    // 群事件 → 重拉群列表; 当前正打开的群受影响时刷新群上下文
    auto refreshGroupContext = [this](quint64 groupId) {
        if (m_chat->currentPeerId() == groupId && m_chat->currentIsGroup())
            m_im->groups()->groupDetail(groupId, [this, groupId](bool ok,
                    const GroupService::GroupDetail &d, const QString &) {
                if (ok && m_chat->currentPeerId() == groupId)
                    m_chat->setGroupContext(d.group, d.members);
            });
    };
    connect(chat, &ChatService::groupMemberChangedEvent, m_contacts,
            [this, refreshGroupContext](const QJsonObject &p) {
                m_contacts->refreshGroups();
                refreshGroupContext(quint64(p.value(QStringLiteral("group_id")).toDouble()));
            });
    connect(chat, &ChatService::groupInfoChangedEvent, m_contacts,
            [this, refreshGroupContext](const QJsonObject &p) {
                m_contacts->refreshGroups();
                refreshGroupContext(quint64(p.value(QStringLiteral("group_id")).toDouble()));
            });
    connect(chat, &ChatService::groupDismissedEvent, m_contacts,
            [this](const QJsonObject &p) {
                const quint64 gid = quint64(p.value(QStringLiteral("group_id")).toDouble());
                m_im->chat()->clearConversation(gid, true);
                m_contacts->refreshGroups();
            });
    connect(chat, &ChatService::groupRequestEvent, m_contacts,
            [this](const QJsonObject &) { m_contacts->syncAll(); });

    // 凭证失效 → 回登录页(main 负责窗口切换)
    connect(m_im, &ImService::sessionExpired, this, &MainWindow::onSessionExpired);

    // 首次进入: 立即做一次全量对齐(不等 TCP, HTTP 数据先行渲染)
    QTimer::singleShot(0, m_contacts, &ContactsView::syncAll);
}

void MainWindow::onSessionExpired()
{
    QMessageBox::warning(this, QStringLiteral("登录已过期"),
                         QStringLiteral("登录已过期，请重新登录"));
    emit logoutRequested();
}
