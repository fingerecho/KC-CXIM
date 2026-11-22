#include "conversationlistview.h"

#include <QVBoxLayout>
#include <QListWidget>
#include <QLabel>
#include <QDateTime>
#include <QFontMetrics>

#include "../common/avatarwidget.h"

namespace {
// 会话行内编辑控件: 头像 | 标题+摘要 | 时间+未读
QWidget *makeRowWidget(const QString &title, const QString &preview,
                       quint64 ts, int unread, quint64 seed, QWidget *parent)
{
    auto *row = new QWidget(parent);
    auto *h = new QHBoxLayout(row);
    // 左右 12px 的呼吸空间原先由 QSS 的 ::item{padding} 提供, 但那会连带扣掉行高
    // 导致头像/文字被裁(详见 theme.cpp 中 listPanel::item 的注释), 故移到行布局里。
    h->setContentsMargins(12, 6, 12, 6);
    h->setSpacing(10);

    auto *avatar = new AvatarWidget(row);
    avatar->setAvatar(40, title, seed);
    h->addWidget(avatar);

    auto *v = new QVBoxLayout;
    v->setSpacing(2);
    auto *titleLabel = new QLabel(title, row);
    QFont tf = titleLabel->font();
    tf.setBold(true);
    tf.setPixelSize(14);
    titleLabel->setFont(tf);
    // 摘要用字体度量手动省略号截断(QLabel 自身不会 elide, 之前直接裁切很难看)
    auto *previewLabel = new QLabel(row);
    previewLabel->setStyleSheet(QStringLiteral("color:#8a9099; font-size:12px;"));
    previewLabel->setMaximumWidth(170);
    QFont pf = previewLabel->font();
    pf.setPixelSize(12);
    previewLabel->setText(QFontMetrics(pf).elidedText(preview, Qt::ElideRight, 170));
    v->addWidget(titleLabel);
    v->addWidget(previewLabel);
    h->addLayout(v, 1);

    auto *right = new QVBoxLayout;
    right->setSpacing(2);
    auto *timeLabel = new QLabel(
        ts ? QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(ts)).toString("HH:mm")
           : QString(), row);
    timeLabel->setStyleSheet(QStringLiteral("color:#b0b5bb; font-size:11px;"));
    right->addWidget(timeLabel, 0, Qt::AlignRight);
    if (unread > 0) {
        auto *badge = new QLabel(QString::number(unread > 99 ? 99 : unread), row);
        badge->setAlignment(Qt::AlignCenter);
        badge->setFixedSize(18, 18);
        badge->setStyleSheet(QStringLiteral(
            "background:#f56c6c; color:white; border-radius:9px; font-size:11px;"));
        right->addWidget(badge, 0, Qt::AlignRight);
    }
    h->addLayout(right);
    return row;
}
} // namespace

ConversationListView::ConversationListView(ImService *im, QWidget *parent)
    : QWidget(parent)
    , m_im(im)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(8, 8, 8, 8);
    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("listPanel"));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    v->addWidget(m_list);

    // 会话变化 → 增量刷新(信号可能高频, 直接重建即可, 列表规模有限)
    connect(m_im->chat(), &ChatService::conversationChanged, this,
            [this](const QString &, const Conversation &) { refresh(); });
    connect(m_im->chat(), &ChatService::historyLoaded, this,
            [this](const QString &, const QList<ChatMessage> &, quint64, bool) { refresh(); });

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        const QString key = it->data(Qt::UserRole).toString();
        const bool isGroup = key.startsWith(QLatin1Char('g'));
        const quint64 peer = key.mid(2).toULongLong();
        emit activated(peer, isGroup, m_titleFor ? m_titleFor(peer, isGroup) : key);
    });

    refresh();
}

void ConversationListView::refresh()
{
    const int prevRow = m_list->currentRow();
    m_list->clear();
    const QList<Conversation> convs = m_im->chat()->conversations();
    for (const Conversation &c : convs) {
        auto *item = new QListWidgetItem(m_list);
        item->setData(Qt::UserRole, c.key());
        const QString title = m_titleFor ? m_titleFor(c.peerId, c.isGroup)
                                         : (c.isGroup ? QStringLiteral("群聊") : QStringLiteral("会话"));
        QWidget *w = makeRowWidget(title, c.hasLast ? c.lastMessage.displayPreview() : QString(),
                                   c.hasLast ? c.lastMessage.ts : 0, c.unread, c.peerId, m_list);
        // 固定行高: sizeHint 在全局字体 polish 前算出的高度不足, 文字底部被裁
        item->setSizeHint(QSize(250, 62));
        m_list->setItemWidget(item, w);
    }
    if (prevRow >= 0 && prevRow < m_list->count())
        m_list->setCurrentRow(prevRow);
}

void ConversationListView::selectConversation(quint64 peerId, bool isGroup)
{
    const QString key = Conversation{peerId, isGroup}.key();
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == key) {
            m_list->setCurrentRow(i);
            return;
        }
    }
}
