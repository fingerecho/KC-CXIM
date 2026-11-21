#include "groupdialogs.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QListWidgetItem>
#include <QScrollArea>
#include <QMenu>
#include <QMessageBox>
#include <QDialog>
#include <QPointer>
#include <QSet>
#include <QFont>
#include <memory>

#include "../../service/imservice.h"
#include "../common/avatarwidget.h"
#include "../common/theme.h"

// ===========================================================================
// groupdialogs.cpp —— 群相关弹窗实现(协议 4.14)
//  CreateGroupDialog   : 建群(4.14.1)
//  GroupManageDialog   : 群管理, 权限矩阵全部由服务端裁决, UI 仅按 myRole 隐藏入口
//  GroupRequestsDialog : 入群申请审批(4.14.7)
// ===========================================================================

namespace {

const QString kGrayTipQss = QStringLiteral("color:#8a9099; font-size:12px;");

QLabel *centerHint(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet(kGrayTipQss);
    return l;
}

QPushButton *smallButton(const QString &text, QWidget *parent, bool primary)
{
    auto *b = new QPushButton(text, parent);
    if (primary) {
        b->setObjectName(QStringLiteral("primaryButton"));
        b->setStyleSheet(Theme::primaryButtonQss());
    } else {
        b->setObjectName(QStringLiteral("ghostButton"));
        b->setStyleSheet(Theme::ghostButtonQss());
    }
    b->setFixedHeight(36);
    b->setMinimumWidth(84);
    return b;
}

/// 角色徽标文本(4.14 角色表: 1群主 2管理员 3成员)
QString roleBadge(int role)
{
    switch (role) {
    case 1: return QStringLiteral("👑群主");
    case 2: return QStringLiteral("★管理员");
    default: return QString();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// CreateGroupDialog(新建群聊, 协议 4.14.1)
// ---------------------------------------------------------------------------
CreateGroupDialog::CreateGroupDialog(ImService *im, QWidget *parent)
    : QDialog(parent)
    , m_im(im)
{
    setWindowTitle(QStringLiteral("新建群聊"));
    setFixedSize(460, 520);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(16, 16, 16, 12);
    v->setSpacing(8);

    auto *nameCap = new QLabel(QStringLiteral("群名称"), this);
    v->addWidget(nameCap);
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("cardInput"));
    m_name->setPlaceholderText(QStringLiteral("群名称(不超过128字节)"));
    v->addWidget(m_name);

    auto *pickCap = new QLabel(QStringLiteral("选择好友(至少1人)"), this);
    v->addWidget(pickCap);
    m_friendList = new QListWidget(this);
    m_friendList->setObjectName(QStringLiteral("listPanel"));
    m_friendList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    v->addWidget(m_friendList, 1);

    auto *status = new QLabel(this);
    status->setObjectName(QStringLiteral("statusLabel"));
    status->setStyleSheet(kGrayTipQss);
    status->setText(QStringLiteral("好友列表加载中…"));
    v->addWidget(status);

    auto *btnRow = new QHBoxLayout;
    auto *cancelBtn = smallButton(QStringLiteral("取消"), this, false);
    auto *createBtn = smallButton(QStringLiteral("创建"), this, true);
    btnRow->addStretch();
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(createBtn);
    v->addLayout(btnRow);

    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    // 单击行即切换勾选, 与旧版交互一致。
    // ⚠ 双击回弹 bug 修复: 直接点复选框时 Qt 视图层已切换一次状态, 若 itemClicked
    // 再切一次就净效果为零("选中不了")。故在 itemPressed 快照状态, itemClicked 时
    // 仅当视图未自行切换(即点在行文本区)才手动补切。
    connect(m_friendList, &QListWidget::itemPressed, this,
            [](QListWidgetItem *it) {
        it->setData(Qt::UserRole + 1, int(it->checkState()));
    });
    connect(m_friendList, &QListWidget::itemClicked, this, [](QListWidgetItem *it) {
        if (!(it->flags() & Qt::ItemIsUserCheckable))
            return;
        const int before = it->data(Qt::UserRole + 1).toInt();
        if (int(it->checkState()) == before)   // 视图没切(点在行文本) → 手动补切
            it->setCheckState(it->checkState() == Qt::Checked ? Qt::Unchecked
                                                              : Qt::Checked);
    });

    // 拉好友列表: 空列表时禁用创建并提示"需要先添加好友"
    QPointer<CreateGroupDialog> self(this);
    m_im->friends()->listFriends([self, createBtn](bool ok,
                                                   const QList<FriendInfo> &friends,
                                                   const QString &err) {
        if (!self)
            return;
        QLabel *st = self->findChild<QLabel *>(QStringLiteral("statusLabel"));
        if (!ok) {
            if (st) st->setText(err.isEmpty() ? QStringLiteral("好友列表加载失败") : err);
            return;
        }
        if (friends.isEmpty()) {
            createBtn->setEnabled(false);
            if (st) st->setText(QStringLiteral("需要先添加好友"));
            return;
        }
        if (st) st->clear();
        for (const FriendInfo &f : friends) {
            auto *it = new QListWidgetItem(self->m_friendList);
            it->setText(QStringLiteral("%1 (ID:%2)").arg(f.displayName())
                            .arg(f.userId));
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(Qt::Unchecked);
            it->setData(Qt::UserRole, f.userId);
        }
    });

    connect(createBtn, &QPushButton::clicked, this, [this, createBtn]() {
        const QString name = m_name->text().trimmed();
        QLabel *st = findChild<QLabel *>(QStringLiteral("statusLabel"));
        if (name.isEmpty() || name.toUtf8().size() > 128) {   // 客户端预检(4.14.1)
            if (st) st->setText(QStringLiteral("群名称不能为空且不超过128字节"));
            return;
        }
        QList<quint64> ids;
        for (int i = 0; i < m_friendList->count(); ++i) {
            QListWidgetItem *it = m_friendList->item(i);
            if (it->checkState() == Qt::Checked)
                ids.append(it->data(Qt::UserRole).toULongLong());
        }
        if (ids.isEmpty()) {
            if (st) st->setText(QStringLiteral("请至少选择1位好友"));
            return;
        }
        if (st) st->setText(QStringLiteral("创建中…"));
        createBtn->setEnabled(false);

        QPointer<CreateGroupDialog> self(this);
        m_im->groups()->createGroup(name, ids,
                                    [self, createBtn](bool ok,
                                    const GroupService::GroupDetail &d,
                                    const QString &err) {
            if (!self)
                return;
            if (ok) {
                emit self->created(d.group.groupId, d.group.name);
                self->accept();
                return;
            }
            createBtn->setEnabled(true);
            if (QLabel *s = self->findChild<QLabel *>(QStringLiteral("statusLabel")))
                s->setText(err);   // 如 initial members must be your friends
        });
    });
}

// ---------------------------------------------------------------------------
// GroupManageDialog(群管理, 协议 4.14.2~4.14.6)
// ---------------------------------------------------------------------------
GroupManageDialog::GroupManageDialog(ImService *im, quint64 groupId, QWidget *parent)
    : QDialog(parent)
    , m_im(im)
    , m_groupId(groupId)
{
    setWindowTitle(QStringLiteral("群管理"));
    setFixedSize(560, 600);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(16, 16, 16, 12);
    v->setSpacing(8);

    // ---- 头部: 群名 + 元信息 ----
    m_nameLabel = new QLabel(QStringLiteral("加载中…"), this);
    QFont nf = m_nameLabel->font();
    nf.setPointSize(15);
    nf.setBold(true);
    m_nameLabel->setFont(nf);
    v->addWidget(m_nameLabel);

    auto *meta = new QLabel(this);
    meta->setObjectName(QStringLiteral("metaLabel"));
    meta->setStyleSheet(kGrayTipQss);
    v->addWidget(meta);

    m_announcementLabel = new QLabel(this);
    m_announcementLabel->setWordWrap(true);
    m_announcementLabel->setStyleSheet(QStringLiteral(
        "color:%1; background:#f2f4f7; border-radius:6px; padding:6px 10px;")
        .arg(Theme::textPrimary().name()));
    v->addWidget(m_announcementLabel);

    // ---- 编辑区(仅群主/管理员可见, 权限矩阵 4.14) ----
    auto *editPanel = new QWidget(this);
    editPanel->setObjectName(QStringLiteral("editPanel"));
    auto *ev = new QVBoxLayout(editPanel);
    ev->setContentsMargins(0, 0, 0, 0);
    ev->setSpacing(6);
    m_nameEdit = new QLineEdit(editPanel);
    m_nameEdit->setObjectName(QStringLiteral("cardInput"));
    m_nameEdit->setPlaceholderText(QStringLiteral("群名称"));
    m_announcementEdit = new QLineEdit(editPanel);
    m_announcementEdit->setObjectName(QStringLiteral("cardInput"));
    m_announcementEdit->setPlaceholderText(QStringLiteral("群公告(≤512字节)"));
    m_saveBtn = new QPushButton(QStringLiteral("保存"), editPanel);
    m_saveBtn->setObjectName(QStringLiteral("primaryButton"));
    m_saveBtn->setStyleSheet(Theme::primaryButtonQss());
    m_saveBtn->setFixedHeight(34);
    m_saveBtn->setMaximumWidth(96);
    auto *saveRow = new QHBoxLayout;
    saveRow->addStretch();
    saveRow->addWidget(m_saveBtn);
    ev->addWidget(m_nameEdit);
    ev->addWidget(m_announcementEdit);
    ev->addLayout(saveRow);
    v->addWidget(editPanel);

    // ---- 成员表 ----
    auto *memCap = new QLabel(QStringLiteral("群成员"), this);
    v->addWidget(memCap);
    m_memberList = new QListWidget(this);
    m_memberList->setObjectName(QStringLiteral("listPanel"));
    m_memberList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_memberList->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(m_memberList, 1);

    // ---- 底部操作 ----
    auto *btnRow = new QHBoxLayout;
    m_inviteBtn = smallButton(QStringLiteral("邀请成员"), this, false);
    m_quitBtn = smallButton(QStringLiteral("退群"), this, false);
    m_dismissBtn = new QPushButton(QStringLiteral("解散群"), this);
    m_dismissBtn->setFixedHeight(36);
    m_dismissBtn->setMinimumWidth(84);
    m_dismissBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color:#d93025; background:#fdecea; border:none; border-radius:8px;"
        " padding:6px 16px; } QPushButton:pressed { background:#f6d2ce; }"
        " QPushButton:disabled { color:#b9bdc2; background:#eceef1; }"));
    btnRow->addWidget(m_inviteBtn);
    btnRow->addStretch();
    btnRow->addWidget(m_quitBtn);
    btnRow->addWidget(m_dismissBtn);
    v->addLayout(btnRow);

    // ---- 信号接线 ----
    connect(m_memberList, &QListWidget::customContextMenuRequested,
            this, &GroupManageDialog::showMemberMenu);
    connect(m_inviteBtn, &QPushButton::clicked, this, &GroupManageDialog::inviteMembers);

    connect(m_saveBtn, &QPushButton::clicked, this, [this]() {
        const QString name = m_nameEdit->text().trimmed();
        const QString ann = m_announcementEdit->text();
        if (name.isEmpty() || name.toUtf8().size() > 128) {
            QMessageBox::warning(this, QStringLiteral("保存"),
                                 QStringLiteral("群名称不能为空且不超过128字节"));
            return;
        }
        if (ann.toUtf8().size() > 512) {
            QMessageBox::warning(this, QStringLiteral("保存"),
                                 QStringLiteral("群公告不能超过512字节"));
            return;
        }
        m_saveBtn->setEnabled(false);
        QPointer<GroupManageDialog> self(this);
        // 空串字段服务端视为"不修改"(4.14.3), 故公告清空需去服务端语义, UI 只提交非空改动
        m_im->groups()->updateGroup(m_groupId, name, ann, QString(),
                                    [self](bool ok, const QString &err) {
            if (!self)
                return;
            self->m_saveBtn->setEnabled(true);
            if (!ok) {
                QMessageBox::warning(self.data(), QStringLiteral("保存失败"), err);
                return;
            }
            emit self->groupChanged(self->m_groupId);
            self->reload();
        });
    });

    connect(m_quitBtn, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, QStringLiteral("退群"),
                                  QStringLiteral("确定退出群聊「%1」吗？")
                                      .arg(m_group.name)) != QMessageBox::Yes)
            return;
        QPointer<GroupManageDialog> self(this);
        m_im->groups()->quitGroup(m_groupId, [self](bool ok, const QString &err) {
            if (!self)
                return;
            if (!ok) {   // 群主会命中 "owner must transfer first"
                QMessageBox::warning(self.data(), QStringLiteral("退群失败"), err);
                return;
            }
            self->accept();
            emit self->groupChanged(self->m_groupId);
        });
    });

    connect(m_dismissBtn, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::question(this, QStringLiteral("解散群"),
                                  QStringLiteral("解散后群与成员关系将被清除，确定解散「%1」吗？")
                                      .arg(m_group.name)) != QMessageBox::Yes)
            return;
        QPointer<GroupManageDialog> self(this);
        m_im->groups()->dismissGroup(m_groupId, [self](bool ok, const QString &err) {
            if (!self)
                return;
            if (!ok) {
                QMessageBox::warning(self.data(), QStringLiteral("解散失败"), err);
                return;
            }
            self->accept();
            emit self->groupChanged(self->m_groupId);
        });
    });

    reload();
}

void GroupManageDialog::reload()
{
    QPointer<GroupManageDialog> self(this);
    m_im->groups()->groupDetail(m_groupId,
                                [self](bool ok, const GroupService::GroupDetail &d,
                                       const QString &err) {
        if (!self)
            return;
        if (!ok) {   // 4.14.2: 非成员/已解散均会失败, 提示后保持原展示
            QMessageBox::warning(self.data(), QStringLiteral("群管理"),
                                 err.isEmpty() ? QStringLiteral("群信息加载失败") : err);
            return;
        }
        self->m_group = d.group;
        self->m_members = d.members;

        // ---- 头部 ----
        self->m_nameLabel->setText(self->m_group.name);
        const QString roleText = self->m_group.myRole == 1 ? QStringLiteral("群主")
                               : self->m_group.myRole == 2 ? QStringLiteral("管理员")
                               : self->m_group.myRole == 3 ? QStringLiteral("成员")
                                                           : QStringLiteral("非成员");
        if (auto *meta = self->findChild<QLabel *>(QStringLiteral("metaLabel")))
            meta->setText(QStringLiteral("ID: %1 · %2 人 · 我的角色: %3")
                              .arg(self->m_group.groupId)
                              .arg(self->m_group.memberCount)
                              .arg(roleText));
        self->m_announcementLabel->setText(self->m_group.announcement.isEmpty()
            ? QStringLiteral("公告: 暂无")
            : QStringLiteral("公告: %1").arg(self->m_group.announcement));

        // ---- 编辑区: 仅群主/管理员(改群资料权限, 服务端最终裁决) ----
        const bool canManage = self->m_group.myRole == 1 || self->m_group.myRole == 2;
        if (auto *panel = self->findChild<QWidget *>(QStringLiteral("editPanel")))
            panel->setVisible(canManage);
        self->m_nameEdit->setText(self->m_group.name);
        self->m_announcementEdit->setText(self->m_group.announcement);

        // ---- 成员表: 展示名 + ID + 角色徽标 ----
        self->m_memberList->clear();
        for (const GroupMember &mem : self->m_members) {
            auto *item = new QListWidgetItem(self->m_memberList);
            auto *row = new QWidget(self->m_memberList);
            auto *h = new QHBoxLayout(row);
            h->setContentsMargins(8, 4, 8, 4);
            h->setSpacing(8);

            auto *ava = new AvatarWidget(row);
            ava->setAvatar(32, mem.displayName(), mem.userId);
            auto *name = new QLabel(mem.displayName(), row);
            auto *id = new QLabel(QStringLiteral("ID:%1").arg(mem.userId), row);
            id->setStyleSheet(kGrayTipQss);
            auto *badge = new QLabel(roleBadge(mem.role), row);
            badge->setStyleSheet(mem.role == 1
                ? QStringLiteral("color:#c88a15; font-weight:bold;")
                : QStringLiteral("color:%1; font-weight:bold;")
                      .arg(Theme::brandAccent().name()));

            h->addWidget(ava);
            h->addWidget(name);
            h->addWidget(id);
            h->addStretch();
            h->addWidget(badge);

            item->setSizeHint(row->sizeHint());
            item->setData(Qt::UserRole, mem.userId);
            item->setData(Qt::UserRole + 1, mem.role);
            item->setData(Qt::UserRole + 2, mem.displayName());
            self->m_memberList->setItemWidget(item, row);
        }

        // ---- 底部按钮: 邀请(群主/管理员) / 退群(群主禁用+提示) / 解散(仅群主) ----
        self->m_inviteBtn->setVisible(canManage);
        if (self->m_group.myRole == 1) {
            self->m_quitBtn->setEnabled(false);
            self->m_quitBtn->setToolTip(QStringLiteral("群主需先转让群主"));
        } else {
            self->m_quitBtn->setVisible(self->m_group.myRole != 0);
            self->m_quitBtn->setEnabled(true);
            self->m_quitBtn->setToolTip(QString());
        }
        self->m_dismissBtn->setVisible(self->m_group.myRole == 1);
    });
}

void GroupManageDialog::showMemberMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_memberList->itemAt(pos);
    if (!item)
        return;
    const quint64 targetId = item->data(Qt::UserRole).toULongLong();
    const int targetRole = item->data(Qt::UserRole + 1).toInt();
    const QString targetName = item->data(Qt::UserRole + 2).toString();
    if (!targetId)
        return;

    const quint64 myId = m_im->session()->userId();
    const int my = m_group.myRole;

    // ---- 权限矩阵落地(4.14): 仅按角色隐藏入口, 服务端仍是唯一裁决方 ----
    QMenu menu(this);
    QAction *actMsg = menu.addAction(QStringLiteral("发私信"));
    QAction *actAdmin = nullptr;      // 任命/取消管理员: 仅群主, 目标非自己
    QAction *actTransfer = nullptr;   // 转让群主: 仅群主, 目标非自己
    QAction *actRemove = nullptr;     // 移出: 群主可移管理员/成员; 管理员仅移成员(role==3)
    if (my == 1 && targetId != myId) {
        if (targetRole == 3)
            actAdmin = menu.addAction(QStringLiteral("任命为管理员"));
        else if (targetRole == 2)
            actAdmin = menu.addAction(QStringLiteral("取消管理员"));
        actTransfer = menu.addAction(QStringLiteral("转让群主"));
    }
    if (targetId != myId &&
        ((my == 1 && targetRole != 1) || (my == 2 && targetRole == 3)))
        actRemove = menu.addAction(QStringLiteral("移出群聊"));

    QAction *picked = menu.exec(m_memberList->mapToGlobal(pos));
    if (!picked)
        return;

    QPointer<GroupManageDialog> self(this);
    if (picked == actMsg) {
        // 关窗并由主界面打开单聊会话
        emit requestOpenFriend(targetId, targetName);
        accept();
    } else if (picked == actAdmin) {
        const bool toAdmin = (targetRole == 3);
        m_im->groups()->setAdmin(m_groupId, targetId, toAdmin,
                                 [self](bool ok, const QString &err) {
            if (!self)
                return;
            if (!ok) {
                QMessageBox::warning(self.data(), QStringLiteral("操作失败"), err);
                return;
            }
            emit self->groupChanged(self->m_groupId);
            self->reload();
        });
    } else if (picked == actRemove) {
        if (QMessageBox::question(this, QStringLiteral("移出群聊"),
                                  QStringLiteral("确定将 %1 移出群聊吗？").arg(targetName))
            != QMessageBox::Yes)
            return;
        m_im->groups()->removeMember(m_groupId, targetId, [self](bool ok, const QString &err) {
            if (!self)
                return;
            if (!ok) {
                QMessageBox::warning(self.data(), QStringLiteral("操作失败"), err);
                return;
            }
            emit self->groupChanged(self->m_groupId);
            self->reload();
        });
    } else if (picked == actTransfer) {
        if (QMessageBox::question(this, QStringLiteral("转让群主"),
                                  QStringLiteral("确定将群主转让给 %1 吗？转让后你将成为管理员。")
                                      .arg(targetName)) != QMessageBox::Yes)
            return;
        m_im->groups()->transferOwner(m_groupId, targetId,
                                      [self](bool ok, const QString &err) {
            if (!self)
                return;
            if (!ok) {
                QMessageBox::warning(self.data(), QStringLiteral("转让失败"), err);
                return;
            }
            emit self->groupChanged(self->m_groupId);
            self->reload();
        });
    }
}

void GroupManageDialog::inviteMembers()
{
    // ---- 邀请去重: 已在群内的好友置灰不可选(服务端也会幂等跳过, 4.14.4) ----
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("邀请成员"));
    dlg.setFixedSize(380, 440);

    auto *v = new QVBoxLayout(&dlg);
    v->setContentsMargins(14, 14, 14, 10);
    v->setSpacing(8);
    v->addWidget(new QLabel(QStringLiteral("选择要邀请的好友(已在群内的不可选)"), &dlg));
    auto *list = new QListWidget(&dlg);
    list->setObjectName(QStringLiteral("listPanel"));
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    v->addWidget(list, 1);
    auto *st = new QLabel(&dlg);
    st->setStyleSheet(kGrayTipQss);
    st->setText(QStringLiteral("好友列表加载中…"));
    v->addWidget(st);
    auto *btnRow = new QHBoxLayout;
    auto *cancelBtn = smallButton(QStringLiteral("取消"), &dlg, false);
    auto *okBtn = smallButton(QStringLiteral("确定"), &dlg, true);
    okBtn->setEnabled(false);
    btnRow->addStretch();
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    v->addLayout(btnRow);

    // 选中结果放堆上: 列表回调可能在 exec 返回后才触发, 避免悬空引用
    auto chosen = std::make_shared<QList<quint64>>();

    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(okBtn, &QPushButton::clicked, &dlg, [list, st, chosen, &dlg]() {
        chosen->clear();
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem *it = list->item(i);
            if (it->flags() & Qt::ItemIsUserCheckable &&
                it->checkState() == Qt::Checked)
                chosen->append(it->data(Qt::UserRole).toULongLong());
        }
        if (chosen->isEmpty()) {
            st->setText(QStringLiteral("请至少选择1位好友"));
            return;
        }
        dlg.accept();
    });
    // 同建群对话框: 点复选框时视图已切换, 仅行文本点击才手动补切(防双击回弹)
    connect(list, &QListWidget::itemPressed, &dlg, [](QListWidgetItem *it) {
        it->setData(Qt::UserRole + 1, int(it->checkState()));
    });
    connect(list, &QListWidget::itemClicked, &dlg, [](QListWidgetItem *it) {
        if (!(it->flags() & Qt::ItemIsUserCheckable))
            return;
        const int before = it->data(Qt::UserRole + 1).toInt();
        if (int(it->checkState()) == before)
            it->setCheckState(it->checkState() == Qt::Checked ? Qt::Unchecked
                                                              : Qt::Checked);
    });

    QSet<quint64> inGroup;
    for (const GroupMember &m : m_members)
        inGroup.insert(m.userId);

    QPointer<QListWidget> listPtr(list);
    QPointer<QPushButton> okPtr(okBtn);
    QPointer<QLabel> stPtr(st);
    m_im->friends()->listFriends([listPtr, okPtr, stPtr, inGroup](
                                    bool ok, const QList<FriendInfo> &friends,
                                    const QString &err) {
        if (!listPtr)   // 弹窗已关闭
            return;
        if (!ok) {
            if (stPtr) stPtr->setText(err.isEmpty() ? QStringLiteral("加载失败") : err);
            return;
        }
        for (const FriendInfo &f : friends) {
            auto *it = new QListWidgetItem(listPtr);
            const bool already = inGroup.contains(f.userId);
            it->setText(QStringLiteral("%1 (ID:%2)%3").arg(f.displayName())
                            .arg(f.userId)
                            .arg(already ? QStringLiteral(" · 已在群内") : QString()));
            if (already) {
                it->setFlags(Qt::NoItemFlags);   // 置灰
            } else {
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(Qt::Unchecked);
            }
            it->setData(Qt::UserRole, f.userId);
        }
        if (stPtr) stPtr->clear();
        if (okPtr) okPtr->setEnabled(true);
    });

    if (dlg.exec() == QDialog::Accepted && !chosen->isEmpty()) {
        QPointer<GroupManageDialog> self(this);
        m_im->groups()->inviteMembers(m_groupId, *chosen,
                                      [self](bool ok, const GroupService::GroupDetail &,
                                             const QString &err) {
            if (!self)
                return;
            if (!ok) {   // 如 member limit exceeded / all invitees already in group
                QMessageBox::warning(self.data(), QStringLiteral("邀请失败"), err);
                return;
            }
            emit self->groupChanged(self->m_groupId);
            self->reload();
        });
    }
}

// ---------------------------------------------------------------------------
// GroupRequestsDialog(入群申请审批, 协议 4.14.7)
// ---------------------------------------------------------------------------
GroupRequestsDialog::GroupRequestsDialog(ImService *im, QWidget *parent)
    : QDialog(parent)
    , m_im(im)
{
    setWindowTitle(QStringLiteral("入群申请"));
    setFixedSize(480, 420);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(16, 16, 16, 12);
    v->setSpacing(8);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scroll);
    m_listLayout = new QVBoxLayout(content);
    m_listLayout->setContentsMargins(0, 0, 4, 0);
    m_listLayout->setSpacing(6);
    scroll->setWidget(content);
    v->addWidget(scroll, 1);

    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    closeBtn->setObjectName(QStringLiteral("ghostButton"));
    closeBtn->setStyleSheet(Theme::ghostButtonQss());
    v->addWidget(closeBtn);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);

    reload();
}

void GroupRequestsDialog::reload()
{
    while (QLayoutItem *it = m_listLayout->takeAt(0)) {
        if (QWidget *w = it->widget())
            w->deleteLater();
        delete it;
    }
    m_listLayout->addWidget(centerHint(QStringLiteral("加载中…"),
                                       m_listLayout->parentWidget()));

    QPointer<GroupRequestsDialog> self(this);
    m_im->groups()->listJoinRequests(QStringLiteral("incoming"),
                                     [self](bool ok, const QList<GroupRequestInfo> &reqs,
                                            const QString &err) {
        if (!self)
            return;
        while (QLayoutItem *it = self->m_listLayout->takeAt(0)) {
            if (QWidget *w = it->widget())
                w->deleteLater();
            delete it;
        }
        QWidget *holder = self->m_listLayout->parentWidget();

        QList<GroupRequestInfo> pending;   // 仅待处理(status==0)可审批
        if (ok) {
            for (const GroupRequestInfo &r : reqs)
                if (r.status == 0)
                    pending.append(r);
        }
        if (!ok && pending.isEmpty() && reqs.isEmpty()) {
            self->m_listLayout->addWidget(centerHint(
                err.isEmpty() ? QStringLiteral("加载失败") : err, holder));
            self->m_listLayout->addStretch();
            return;
        }
        if (pending.isEmpty()) {
            self->m_listLayout->addWidget(
                centerHint(QStringLiteral("暂无待审批申请"), holder));
            self->m_listLayout->addStretch();
            return;
        }

        for (const GroupRequestInfo &r : pending) {
            auto *row = new QWidget(holder);
            row->setStyleSheet(QStringLiteral(
                "QWidget{background:#ffffff;border-radius:8px;} QLabel{background:transparent;}"));
            auto *h = new QHBoxLayout(row);
            h->setContentsMargins(10, 8, 10, 8);
            h->setSpacing(10);

            auto *ava = new AvatarWidget(row);
            ava->setAvatar(36, r.requesterName(), r.requesterId);

            auto *col = new QVBoxLayout;
            col->setSpacing(2);
            auto *name = new QLabel(
                QStringLiteral("%1  (ID:%2)").arg(r.requesterName()).arg(r.requesterId), row);
            QFont f = name->font();
            f.setPointSize(10);
            f.setBold(true);
            name->setFont(f);
            auto *sub = new QLabel(
                QStringLiteral("群:%1 · 附言: %2")
                    .arg(r.groupName, r.message.isEmpty() ? QStringLiteral("（无）")
                                                          : r.message), row);
            sub->setStyleSheet(kGrayTipQss);
            sub->setWordWrap(true);
            col->addWidget(name);
            col->addWidget(sub);

            auto *passBtn = new QPushButton(QStringLiteral("通过"), row);
            passBtn->setObjectName(QStringLiteral("ghostButton"));
            passBtn->setStyleSheet(QStringLiteral(
                "QPushButton { color:%1; background:#eceef1; border:none; border-radius:6px;"
                " padding:4px 14px; } QPushButton:pressed { background:#dfe2e6; }"
                " QPushButton:disabled { color:#9aa0a8; background:transparent; }")
                .arg(Theme::textPrimary().name()));
            auto *rejBtn = new QPushButton(QStringLiteral("拒绝"), row);
            rejBtn->setStyleSheet(passBtn->styleSheet());
            rejBtn->setObjectName(passBtn->objectName());
            h->addWidget(ava);
            h->addLayout(col, 1);
            h->addWidget(passBtn);
            h->addWidget(rejBtn);
            self->m_listLayout->addWidget(row);

            // 通过 → acceptJoin: reload 后行消失, emit groupChanged 供主界面刷新成员数
            QPointer<QPushButton> pass(passBtn);
            self->connect(passBtn, &QPushButton::clicked, passBtn, [self, pass, r]() {
                if (!self || !pass)
                    return;
                pass->setEnabled(false);
                self->m_im->groups()->acceptJoin(r.requestId, [self, r](bool ok,
                                                                         const QString &err) {
                    if (!self)
                        return;
                    if (!ok) {   // 如 request not found or already handled
                        QMessageBox::warning(self.data(), QStringLiteral("审批失败"), err);
                        self->reload();
                        return;
                    }
                    self->reload();
                    emit self->groupChanged(r.groupId);
                });
            });
            QPointer<QPushButton> rej(rejBtn);
            self->connect(rejBtn, &QPushButton::clicked, rejBtn, [self, rej, r]() {
                if (!self || !rej)
                    return;
                rej->setEnabled(false);
                self->m_im->groups()->rejectJoin(r.requestId, [self](bool ok,
                                                                      const QString &err) {
                    if (!self)
                        return;
                    if (!ok) {
                        QMessageBox::warning(self.data(), QStringLiteral("审批失败"), err);
                        self->reload();
                        return;
                    }
                    self->reload();
                });
            });
        }
        self->m_listLayout->addStretch();
    });
}
