#include "adddialogs.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QPointer>
#include <QMessageBox>
#include <QFont>

#include "../../service/imservice.h"
#include "../common/avatarwidget.h"
#include "../common/theme.h"

// ===========================================================================
// adddialogs.cpp —— 好友弹窗实现(协议 4.4~4.9)
//  AddFriendDialog      : 搜索用户 → 发申请(4.4/4.7)
//  FriendRequestsDialog : 待处理申请审批(4.6/4.9)
// 弹窗均为 exec() 模态, 异步回调期间事件循环仍在跑, 用 QPointer 兜底生命周期。
// ===========================================================================

namespace {

const QString kGrayTipQss  = QStringLiteral("color:#8a9099; font-size:12px;");
const QString kCardRowQss  = QStringLiteral("QWidget{background:#ffffff;border-radius:8px;}"
                                            "QLabel{background:transparent;}");

/// 递归移除布局内的控件与嵌套布局(控件用 deleteLater, 避免回调重入时悬空)
void clearLayout(QLayout *lay)
{
    if (!lay)
        return;
    while (QLayoutItem *it = lay->takeAt(0)) {
        if (QWidget *w = it->widget())
            w->deleteLater();
        if (QLayout *sub = it->layout()) {
            clearLayout(sub);
            delete sub;
        }
        delete it;
    }
}

/// 居中灰色占位标签(空列表/加载中)
QLabel *centerHint(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet(kGrayTipQss);
    return l;
}

/// 行内小操作按钮(比 Theme::ghostButtonQss 更紧凑, 适配 360~480 宽弹窗)
QPushButton *rowButton(const QString &text, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setObjectName(QStringLiteral("ghostButton"));
    b->setStyleSheet(QStringLiteral(
        "QPushButton { color:%1; background:#eceef1; border:none; border-radius:6px;"
        " padding:4px 14px; }"
        "QPushButton:pressed { background:#dfe2e6; }"
        "QPushButton:disabled { color:#9aa0a8; background:transparent; }")
        .arg(Theme::textPrimary().name()));
    b->setCursor(Qt::PointingHandCursor);
    return b;
}

} // namespace

// ---------------------------------------------------------------------------
// AddFriendDialog(添加好友)
// ---------------------------------------------------------------------------
AddFriendDialog::AddFriendDialog(ImService *im, QWidget *parent)
    : QDialog(parent)
    , m_im(im)
{
    setWindowTitle(QStringLiteral("添加好友"));
    setFixedSize(420, 360);

    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(16, 16, 16, 12);
    v->setSpacing(8);

    // ---- 顶部: 搜索框 + 搜索按钮 ----
    auto *top = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName(QStringLiteral("cardInput"));
    m_search->setPlaceholderText(QStringLiteral("输入用户ID或用户名"));
    m_search->setClearButtonEnabled(true);
    auto *searchBtn = new QPushButton(QStringLiteral("搜索"), this);
    searchBtn->setObjectName(QStringLiteral("primaryButton"));
    searchBtn->setStyleSheet(Theme::primaryButtonQss());
    searchBtn->setFixedHeight(40);
    searchBtn->setMinimumWidth(76);
    top->addWidget(m_search, 1);
    top->addWidget(searchBtn);
    v->addLayout(top);

    // ---- 状态行(错误/进度提示, doSearch 经 findChild 取用) ----
    auto *status = new QLabel(this);
    status->setObjectName(QStringLiteral("statusLabel"));
    status->setStyleSheet(kGrayTipQss);
    status->setText(QStringLiteral("输入关键字后回车或点击搜索"));
    v->addWidget(status);

    // ---- 结果区: 可滚动的动态行 ----
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget(scroll);
    m_resultLayout = new QVBoxLayout(content);
    m_resultLayout->setContentsMargins(0, 0, 4, 0);
    m_resultLayout->setSpacing(6);
    scroll->setWidget(content);
    v->addWidget(scroll, 1);

    auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    closeBtn->setObjectName(QStringLiteral("ghostButton"));
    closeBtn->setStyleSheet(Theme::ghostButtonQss());
    v->addWidget(closeBtn);

    connect(searchBtn, &QPushButton::clicked, this, &AddFriendDialog::doSearch);
    connect(m_search, &QLineEdit::returnPressed, this, &AddFriendDialog::doSearch);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}

void AddFriendDialog::doSearch()
{
    const QString kw = m_search->text().trimmed();
    auto *status = findChild<QLabel *>(QStringLiteral("statusLabel"));
    if (kw.isEmpty()) {
        if (status) status->setText(QStringLiteral("请输入用户ID或用户名"));
        return;
    }

    clearLayout(m_resultLayout);
    m_resultLayout->addWidget(centerHint(QStringLiteral("搜索中…"), m_resultLayout->parentWidget()));
    if (status) status->clear();

    // 搜索结果每行: 头像 | ID+展示名 | 添加按钮(协议 4.4 → 4.7)
    QPointer<AddFriendDialog> self(this);
    m_im->friends()->searchUsers(kw, [self](bool ok, const QList<UserCard> &users,
                                            const QString &err) {
        if (!self)
            return;
        clearLayout(self->m_resultLayout);
        QLabel *status = self->findChild<QLabel *>(QStringLiteral("statusLabel"));
        QWidget *holder = self->m_resultLayout->parentWidget();

        if (!ok) {
            self->m_resultLayout->addWidget(centerHint(
                err.isEmpty() ? QStringLiteral("搜索失败") : err, holder));
            self->m_resultLayout->addStretch();
            return;
        }
        if (users.isEmpty()) {          // 空结果 → 占位提示
            if (status) status->clear();
            self->m_resultLayout->addWidget(
                centerHint(QStringLiteral("未找到用户"), holder));
            self->m_resultLayout->addStretch();
            return;
        }

        for (const UserCard &u : users) {
            auto *row = new QWidget(holder);
            row->setStyleSheet(kCardRowQss);
            auto *h = new QHBoxLayout(row);
            h->setContentsMargins(10, 6, 10, 6);
            h->setSpacing(10);

            auto *ava = new AvatarWidget(row);
            ava->setAvatar(32, u.displayName(), u.userId);
            auto *info = new QLabel(
                QStringLiteral("ID: %1  %2").arg(u.userId).arg(u.displayName()), row);
            QFont f = info->font();
            f.setPointSize(10);
            info->setFont(f);

            QPushButton *addBtn = rowButton(QStringLiteral("添加"), row);
            h->addWidget(ava);
            h->addWidget(info, 1);
            h->addWidget(addBtn);
            self->m_resultLayout->addWidget(row);

            // 点击添加 → 发好友申请(协议 4.7, 固定附言与旧版一致)
            QPointer<QPushButton> btn(addBtn);
            self->connect(addBtn, &QPushButton::clicked, addBtn,
                          [self, btn, u]() {
                if (!self || !btn)
                    return;
                btn->setEnabled(false);
                btn->setText(QStringLiteral("发送中…"));
                QLabel *st = self->findChild<QLabel *>(QStringLiteral("statusLabel"));
                self->m_im->friends()->sendRequest(
                    u.userId, QStringLiteral("你好，我想加你为好友"),
                    [self, btn, st](bool ok, const QString &e) {
                if (!self)
                    return;
                // 服务端报 already friends 与发送成功同样按"已添加"置灰处理
                if (ok || e.contains(QStringLiteral("already friends"))) {
                    if (btn) {
                        btn->setText(QStringLiteral("已添加"));
                        btn->setFlat(true);
                        btn->setEnabled(false);
                    }
                    if (st)
                        st->setText(ok ? QStringLiteral("申请已发送，等待对方通过")
                                       : QStringLiteral("对方已经是你的好友"));
                } else {
                    if (btn) {
                        btn->setText(QStringLiteral("添加"));
                        btn->setEnabled(true);
                    }
                    if (st) st->setText(e);   // 其他错误: 恢复按钮 + 状态栏展示原因
                }
                });
            });
        }
        self->m_resultLayout->addStretch();
    });
}

// ---------------------------------------------------------------------------
// FriendRequestsDialog(新朋友, 协议 4.6/4.9)
// ---------------------------------------------------------------------------
FriendRequestsDialog::FriendRequestsDialog(ImService *im, QWidget *parent)
    : QDialog(parent)
    , m_im(im)
{
    setWindowTitle(QStringLiteral("新朋友"));
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

void FriendRequestsDialog::reload()
{
    clearLayout(m_listLayout);
    m_listLayout->addWidget(centerHint(QStringLiteral("加载中…"),
                                       m_listLayout->parentWidget()));

    QPointer<FriendRequestsDialog> self(this);
    m_im->friends()->listRequests(QStringLiteral("incoming"),
                                  [self](bool ok, const QList<FriendRequestInfo> &reqs,
                                         const QString &err) {
        if (!self)
            return;
        clearLayout(self->m_listLayout);
        QWidget *holder = self->m_listLayout->parentWidget();

        QList<FriendRequestInfo> pending;   // 只展示待处理(status==0)的申请
        if (ok) {
            for (const FriendRequestInfo &r : reqs)
                if (r.status == 0)
                    pending.append(r);
        }

        if (!ok && reqs.isEmpty()) {
            self->m_listLayout->addWidget(centerHint(
                err.isEmpty() ? QStringLiteral("加载失败") : err, holder));
            self->m_listLayout->addStretch();
            return;
        }
        if (pending.isEmpty()) {
            self->m_listLayout->addWidget(
                centerHint(QStringLiteral("暂无新的好友申请"), holder));
            self->m_listLayout->addStretch();
            return;
        }

        for (const FriendRequestInfo &r : pending) {
            auto *row = new QWidget(holder);
            row->setStyleSheet(kCardRowQss);
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
            auto *msg = new QLabel(r.message.isEmpty()
                                       ? QStringLiteral("附言: （无）")
                                       : QStringLiteral("附言: %1").arg(r.message), row);
            msg->setStyleSheet(kGrayTipQss);
            msg->setWordWrap(true);
            col->addWidget(name);
            col->addWidget(msg);

            auto *passBtn = rowButton(QStringLiteral("通过"), row);
            auto *rejBtn  = rowButton(QStringLiteral("拒绝"), row);
            h->addWidget(ava);
            h->addLayout(col, 1);
            h->addWidget(passBtn);
            h->addWidget(rejBtn);
            self->m_listLayout->addWidget(row);

            // 通过 → acceptRequest, 行内变"已添加"(132 事件负责主界面全量刷新)
            QPointer<QPushButton> pass(passBtn), rej(rejBtn);
            self->connect(passBtn, &QPushButton::clicked, passBtn, [self, pass, rej, r]() {
                if (!self || !pass)
                    return;
                pass->setEnabled(false);
                self->m_im->friends()->acceptRequest(r.requestId,
                                                     [self, pass, rej](bool ok,
                                                                        const QString &e) {
                    if (!self)
                        return;
                    if (!ok) {
                        if (pass) { pass->setEnabled(true); }
                        QMessageBox::warning(self.data(), QStringLiteral("操作失败"), e);
                        return;
                    }
                    if (pass) {
                        pass->setText(QStringLiteral("已添加"));
                        pass->setFlat(true);
                        pass->setEnabled(false);
                    }
                    if (rej) rej->setVisible(false);
                });
            });
            // 拒绝 → rejectRequest 后整行消失(reload 重新拉取)
            self->connect(rejBtn, &QPushButton::clicked, rejBtn, [self, r]() {
                if (!self)
                    return;
                self->m_im->friends()->rejectRequest(r.requestId, [self](bool ok,
                                                                         const QString &e) {
                    if (!self)
                        return;
                    if (!ok) {
                        QMessageBox::warning(self.data(), QStringLiteral("操作失败"), e);
                        return;
                    }
                    self->reload();
                });
            });
        }
        self->m_listLayout->addStretch();
    });
}
