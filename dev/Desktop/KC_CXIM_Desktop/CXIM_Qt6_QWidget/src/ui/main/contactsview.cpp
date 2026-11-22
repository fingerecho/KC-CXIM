#include "contactsview.h"

#include <QDebug>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>

#include "../common/avatarwidget.h"
#include "../common/theme.h"
#include "../dialogs/adddialogs.h"
#include "../dialogs/groupdialogs.h"

namespace {
// 通用两行列表项: 图标 | 主标题 + 副标题
QListWidgetItem *makeRow(QListWidget *list, const QString &iconText,
                         const QString &title, const QString &sub,
                         quint64 seed, bool groupAvatar = false)
{
    auto *item = new QListWidgetItem(list);
    auto *row = new QWidget(list);
    auto *h = new QHBoxLayout(row);
    // 左右 12px 原先靠 QSS 的 ::item{padding} 留白, 但那会一并扣掉行高把头像和
    // 文字裁掉(见 theme.cpp 中 listPanel::item 的注释), 所以留白改由行布局承担。
    h->setContentsMargins(12, 8, 12, 8);
    h->setSpacing(12);

    // 左侧三种形态, 优先级: 功能入口字符 > 群头像 > 个人头像
    if (!iconText.isEmpty()) {
        // 功能入口("新朋友"/"入群申请"): 40px 圆形品牌蓝徽标 + 一个表意字符,
        // 与旁边的个人头像同规格, 视觉上是一列而不是混进来的图标。
        // 必须走这个分支而不是 AvatarWidget: 后者取 nameOrId.left(1) 当首字符,
        // 而 emoji(🤝/📨)在 QString 里是代理对, left(1) 只截到前半个码元 → 豆腐块。
        auto *glyph = new QLabel(iconText, row);
        glyph->setFixedSize(40, 40);
        glyph->setAlignment(Qt::AlignCenter);
        glyph->setStyleSheet(QStringLiteral(
            "QLabel { color: white; font-size: 18px; border-radius: 20px;"
            " background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
            " stop:0 %1, stop:1 %2); }")
            .arg(Theme::brandTop().name(), Theme::brandBottom().name()));
        h->addWidget(glyph);
    } else if (groupAvatar) {
        auto *g = new QLabel(QStringLiteral("👥"), row);   // 群图标: 原生 emoji, 免资源
        g->setStyleSheet(QStringLiteral("font-size:26px; color:#5a7fa6;"));
        g->setFixedWidth(40);
        g->setAlignment(Qt::AlignCenter);
        h->addWidget(g);
    } else {
        auto *avatar = new AvatarWidget(row);
        avatar->setAvatar(40, title, seed);
        h->addWidget(avatar);
    }

    auto *v = new QVBoxLayout;
    v->setSpacing(2);
    auto *t = new QLabel(title, row);
    QFont f = t->font();
    // 用像素字号(与全局 QSS 的 14px 同口径): pointSize 在 sizeHint 时机不稳定,
    // 曾导致行高不足文字底部被截断
    f.setPixelSize(14);
    t->setFont(f);
    v->addWidget(t);
    if (!sub.isEmpty()) {
        auto *s = new QLabel(sub, row);
        s->setStyleSheet(QStringLiteral("color:#8a9099; font-size:12px;"));
        v->addWidget(s);
    }
    h->addLayout(v, 1);

    // 固定行高 64px: sizeHint 在全局字体 polish 前计算不可靠, 显式给足高度
    item->setSizeHint(QSize(240, 64));
    list->setItemWidget(item, row);
    return item;
}
} // namespace

ContactsView::ContactsView(ImService *im, QWidget *parent)
    : QWidget(parent)
    , m_im(im)
{
    buildUi();
}

void ContactsView::buildUi()
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(16, 12, 16, 12);
    v->setSpacing(10);

    // ---- 顶部: 搜索 + "+" ----
    auto *top = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("searchInput"));
    m_search->setPlaceholderText(QStringLiteral("🔍 搜索"));
    m_addBtn = new QPushButton(QStringLiteral("＋"), this);
    m_addBtn->setObjectName(QStringLiteral("plusButton"));
    m_addBtn->setFixedSize(40, 40);
    top->addWidget(m_search, 1);
    top->addWidget(m_addBtn);
    v->addLayout(top);

    connect(m_search, &QLineEdit::returnPressed, this, &ContactsView::onSearchEntered);
    connect(m_addBtn, &QPushButton::clicked, this, &ContactsView::onAddClicked);

    // ---- 好友 | 群聊 标签 ----
    auto *tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(0, 0, 0, 0);
    auto *tabPanel = new QWidget(this);
    tabPanel->setObjectName(QStringLiteral("tabBar"));
    auto *tabLay = new QHBoxLayout(tabPanel);
    tabLay->setContentsMargins(2, 2, 2, 2);
    m_tabFriends = new QPushButton(QStringLiteral("好友"), tabPanel);
    m_tabGroups = new QPushButton(QStringLiteral("群聊"), tabPanel);
    for (QPushButton *b : {m_tabFriends, m_tabGroups}) {
        b->setObjectName(QStringLiteral("tabButton"));
        b->setCheckable(true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        tabLay->addWidget(b);
    }
    m_tabFriends->setChecked(true);
    tabRow->addWidget(tabPanel);
    v->addLayout(tabRow);

    connect(m_tabFriends, &QPushButton::clicked, this, [this] { onTabChanged(false); });
    connect(m_tabGroups, &QPushButton::clicked, this, [this] { onTabChanged(true); });

    // ---- 列表区 ----
    m_lists = new QStackedWidget(this);
    m_friendsList = new QListWidget(m_lists);
    m_groupsList = new QListWidget(m_lists);
    for (QListWidget *l : {m_friendsList, m_groupsList}) {
        l->setObjectName(QStringLiteral("listPanel"));
        l->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        l->setContextMenuPolicy(Qt::CustomContextMenu);
    }
    m_lists->addWidget(m_friendsList);   // index 0
    m_lists->addWidget(m_groupsList);    // index 1
    v->addWidget(m_lists, 1);

    connect(m_friendsList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *it) {
                const quint64 id = it->data(Qt::UserRole).toULongLong();
                if (!id) return;   // banner 行无 ID
                emit openConversation(id, false, displayNameOf(id, false));
            });
    connect(m_groupsList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *it) {
                const quint64 id = it->data(Qt::UserRole).toULongLong();
                if (!id) return;
                emit openConversation(id, true, displayNameOf(id, true));
            });
    // banner 行(UserRole==0)单击 → 对应申请弹窗(连接只建一次, 避免刷新叠加)
    connect(m_friendsList, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        if (it->data(Qt::UserRole).toULongLong() != 0)
            return;
        auto *dlg = new FriendRequestsDialog(m_im, this);
        connect(dlg, &QObject::destroyed, this, [this] { syncAll(); });
        dlg->exec();
    });
    connect(m_groupsList, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        if (it->data(Qt::UserRole).toULongLong() != 0)
            return;
        auto *dlg = new GroupRequestsDialog(m_im, this);
        connect(dlg, &QObject::destroyed, this, [this] { syncAll(); });
        dlg->exec();
    });
    connect(m_friendsList, &QListWidget::customContextMenuRequested,
            this, &ContactsView::showFriendMenu);
    connect(m_groupsList, &QListWidget::customContextMenuRequested,
            this, &ContactsView::showGroupMenu);

    // ---- 底部: 新建群聊(仅群聊标签显示) ----
    m_newGroupBtn = new QPushButton(QStringLiteral("＋ 新建群聊"), this);
    m_newGroupBtn->setObjectName(QStringLiteral("primaryButton"));
    m_newGroupBtn->setStyleSheet(Theme::primaryButtonQss());
    m_newGroupBtn->setFixedHeight(44);
    m_newGroupBtn->setVisible(false);
    v->addWidget(m_newGroupBtn);
    connect(m_newGroupBtn, &QPushButton::clicked, this, [this] {
        auto *dlg = new CreateGroupDialog(m_im, this);
        connect(dlg, &CreateGroupDialog::created, this,
                [this](quint64, const QString &) { refreshGroups(); });
        dlg->exec();
    });
}

void ContactsView::onTabChanged(bool groups)
{
    m_tabFriends->setChecked(!groups);
    m_tabGroups->setChecked(groups);
    m_lists->setCurrentIndex(groups ? 1 : 0);
    m_newGroupBtn->setVisible(groups);
}

void ContactsView::onSearchEntered()
{
    const QString kw = m_search->text().trimmed();
    if (kw.isEmpty())
        return;
    auto *dlg = new AddFriendDialog(m_im, this);
    dlg->exec();
}

void ContactsView::onAddClicked()
{
    qDebug("[dbg] onAddClicked enter");
    auto *dlg = new AddFriendDialog(m_im, this);
    qDebug("[dbg] dialog created, exec...");
    dlg->exec();
    qDebug("[dbg] dialog done");
}

// ---------------------------------------------------------------------------
// 全量对齐(TCP 登录后)与定向刷新(事件触发)
// ---------------------------------------------------------------------------
void ContactsView::syncAll()
{
    refreshFriends();
    refreshGroups();
}

void ContactsView::refreshFriends()
{
    // 先取申请计数再渲染列表(顺序回调保证 banner 数字与列表同帧更新)
    m_im->friends()->listRequests(QStringLiteral("incoming"),
                                  [this](bool ok, const QList<FriendRequestInfo> &reqs,
                                         const QString &) {
        if (ok) {
            m_pendingFriendReqs = 0;
            for (const FriendRequestInfo &r : reqs)
                if (r.status == 0)
                    ++m_pendingFriendReqs;
        }
        renderFriends();
    });
}

void ContactsView::renderFriends()
{
    m_im->friends()->listFriends([this](bool ok, const QList<FriendInfo> &list,
                                        const QString &) {
        if (!ok)
            return;
        m_friends = list;
        m_friendsList->clear();

        // 新朋友入口常驻列表顶部(与旧版一致), 待处理数实时体现在副标题
        // 徽标字符用 🤝(握手=结为新友), 不再写成 "🔔 新朋友" 塞进标题:
        // 那样标题带 emoji、左侧头像还会因代理对被截半而显示成方块。
        auto *banner = makeRow(m_friendsList, QStringLiteral("🤝"),
                               QStringLiteral("新朋友"),
                               m_pendingFriendReqs > 0
                                   ? QStringLiteral("%1 条待处理申请").arg(m_pendingFriendReqs)
                                   : QStringLiteral("暂无新申请"),
                               0);
        banner->setData(Qt::UserRole, 0);

        for (const FriendInfo &f : m_friends) {
            auto *item = makeRow(m_friendsList, QString(), f.displayName(),
                                 QStringLiteral("ID:%1").arg(f.userId), f.userId);
            item->setData(Qt::UserRole, f.userId);
        }
    });
}

void ContactsView::refreshGroups()
{
    // 入群申请数先行(仅群主/管理员有数据, 服务端按权限过滤 direction=incoming)
    m_im->groups()->listJoinRequests(QStringLiteral("incoming"),
                                     [this](bool ok, const QList<GroupRequestInfo> &reqs,
                                            const QString &) {
        m_pendingGroupReqs = ok ? reqs.size() : 0;
        renderGroups();
    });
}

void ContactsView::renderGroups()
{
    m_im->groups()->listGroups([this](bool ok, const QList<GroupInfo> &list,
                                      const QString &) {
        if (!ok)
            return;
        m_groups = list;
        m_groupsList->clear();

        if (m_pendingGroupReqs > 0) {
            // 点击处理在 buildUi 的常驻 itemClicked 连接里, 这里只负责渲染
            // 徽标字符用 📨(收到待办申请), 与"新朋友"的 🤝 同一套圆形蓝底规格
            auto *banner = makeRow(m_groupsList, QStringLiteral("📨"),
                                   QStringLiteral("入群申请"),
                                   QStringLiteral("%1 条待审批").arg(m_pendingGroupReqs), 0);
            banner->setData(Qt::UserRole, 0);
        }

        for (const GroupInfo &g : m_groups) {
            auto *item = makeRow(m_groupsList, QString(), g.name,
                                 QStringLiteral("%1 人").arg(g.memberCount),
                                 g.groupId, /*groupAvatar=*/true);
            item->setData(Qt::UserRole, g.groupId);
        }
    });
}

QString ContactsView::displayNameOf(quint64 peerId, bool isGroup) const
{
    if (isGroup) {
        for (const GroupInfo &g : m_groups)
            if (g.groupId == peerId)
                return g.name;
        return QStringLiteral("群%1").arg(peerId);
    }
    for (const FriendInfo &f : m_friends)
        if (f.userId == peerId)
            return f.displayName();
    return QStringLiteral("用户%1").arg(peerId);
}

// ---------------------------------------------------------------------------
// 右键菜单: 好友操作(协议 4.9~4.13, 权限与副作用由服务端裁决)
// ---------------------------------------------------------------------------
void ContactsView::showFriendMenu(const QPoint &pos)
{
    auto *item = m_friendsList->itemAt(pos);
    const quint64 id = item ? item->data(Qt::UserRole).toULongLong() : 0;
    if (!id)
        return;

    QMenu menu(this);
    QAction *msg = menu.addAction(QStringLiteral("发消息"));
    QAction *remark = menu.addAction(QStringLiteral("设置备注"));
    QAction *del = menu.addAction(QStringLiteral("删除好友"));
    QAction *block = menu.addAction(QStringLiteral("加入黑名单"));
    QAction *picked = menu.exec(m_friendsList->mapToGlobal(pos));
    if (!picked)
        return;

    if (picked == msg) {
        emit openConversation(id, false, displayNameOf(id, false));
    } else if (picked == remark) {
        setFriendRemark(id, QString());
    } else if (picked == del) {
        if (QMessageBox::question(this, QStringLiteral("删除好友"),
                                  QStringLiteral("确定删除好友 %1 吗？").arg(displayNameOf(id, false)))
            == QMessageBox::Yes) {
            m_im->friends()->removeFriend(id, [this](bool ok, const QString &err) {
                if (!ok) QMessageBox::warning(this, QStringLiteral("删除失败"), err);
                refreshFriends();   // 132 事件也会触发, 这里双保险
            });
        }
    } else if (picked == block) {
        if (QMessageBox::question(this, QStringLiteral("加入黑名单"),
                                  QStringLiteral("拉黑后好友关系将被删除，确定吗？"))
            == QMessageBox::Yes) {
            m_im->friends()->blockFriend(id, [this](bool ok, const QString &err) {
                if (!ok) QMessageBox::warning(this, QStringLiteral("操作失败"), err);
                refreshFriends();
            });
        }
    }
}

void ContactsView::setFriendRemark(quint64 id, const QString &)
{
    bool okInput = false;
    const QString text = QInputDialog::getText(this, QStringLiteral("设置备注"),
                                               QStringLiteral("备注(≤64 字节):"),
                                               QLineEdit::Normal, QString(), &okInput);
    if (!okInput)
        return;
    m_im->friends()->setRemark(id, text, [this](bool ok, const QString &err) {
        if (!ok) QMessageBox::warning(this, QStringLiteral("设置备注"), err);
        refreshFriends();
    });
}

void ContactsView::showGroupMenu(const QPoint &pos)
{
    auto *item = m_groupsList->itemAt(pos);
    const quint64 id = item ? item->data(Qt::UserRole).toULongLong() : 0;
    if (!id)
        return;

    QMenu menu(this);
    QAction *manage = menu.addAction(QStringLiteral("管理群聊"));
    QAction *open = menu.addAction(QStringLiteral("进入群聊"));
    QAction *picked = menu.exec(m_groupsList->mapToGlobal(pos));
    if (picked == manage) {
        auto *dlg = new GroupManageDialog(m_im, id, this);
        connect(dlg, &GroupManageDialog::groupChanged, this,
                [this](quint64) { refreshGroups(); });
        dlg->exec();
    } else if (picked == open) {
        emit openConversation(id, true, displayNameOf(id, true));
    }
}
