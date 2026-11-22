#include "chatview.h"
#include "messagebubble.h"

#include "../common/theme.h"
#include "../common/avatarwidget.h"
#include "../dialogs/groupdialogs.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextEdit>
#include <QTimer>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QImage>
#include <QPointer>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <memory>

namespace {

const int kThumbMaxSize   = 240;   // 图片消息缩略图长边上限

/// 气泡最大宽度 = 聊天面板可视宽的 3/4(用户定案), 下限 320 防窄窗过挤
int bubbleMaxWidth(const QWidget *viewport)
{
    const int w = viewport ? viewport->width() : 800;
    return qMax(320, w * 3 / 4);
}

/// 人性化文件大小(文件气泡展示)
QString humanSize(qint64 bytes)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    if (u == 0)
        return QStringLiteral("%1B").arg(bytes);
    return QStringLiteral("%1%2").arg(v, 0, 'f', 1).arg(QLatin1String(units[u]));
}

} // namespace

// ===========================================================================
// MessageRow: 一条消息行(头像/发送者名/气泡/状态), 按契约定义在 .cpp 内
// ===========================================================================
struct ChatView::MessageRow {
    QWidget *row = nullptr;
    AvatarWidget *avatar = nullptr;     // incoming 左侧 32px 头像
    QLabel *nameLabel = nullptr;        // 群聊 incoming 气泡上方发送者名
    BubbleLabel *bubble = nullptr;      // 文本/图片/文件统一承载
    QLabel *statusLabel = nullptr;      // outgoing 右侧灰字状态
    quint64 clientMsgId = 0;            // 与 ChatService::messageStatusChanged 关联
    bool outgoing = false;
};

// ===========================================================================
// 构造: chatHeader → announcementBar → messageArea → 输入区(工具栏+输入框+发送)
// ===========================================================================
ChatView::ChatView(ImService *im, QWidget *parent)
    : QWidget(parent)
    , m_im(im)
{
    m_nam = new QNetworkAccessManager(this);   // 媒体下载专用(带 Bearer 换取入口)

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 1. chatHeader: 加粗标题 + 群聊绿点 + 右侧"⋯"管理入口 ----
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("chatHeader"));
    header->setFixedHeight(56);
    auto *hh = new QHBoxLayout(header);
    hh->setContentsMargins(16, 0, 10, 0);
    hh->setSpacing(6);

    m_titleLabel = new QLabel(header);
    m_titleLabel->setObjectName(QStringLiteral("chatTitle"));
    m_onlineDot = new QLabel(QStringLiteral("●"), header);
    m_onlineDot->setStyleSheet(QStringLiteral("color:#2ecc71; font-size:13px;"));
    m_onlineDot->hide();
    m_moreBtn = new QPushButton(QStringLiteral("⋯"), header);
    m_moreBtn->setToolTip(QStringLiteral("群管理"));
    m_moreBtn->setCursor(Qt::PointingHandCursor);
    m_moreBtn->hide();

    hh->addWidget(m_titleLabel);
    hh->addWidget(m_onlineDot);
    hh->addStretch(1);
    hh->addWidget(m_moreBtn);
    root->addWidget(header);

    // ---- 2. announcementBar: 仅群公告非空时显示 ----
    m_announcement = new QLabel(this);
    m_announcement->setObjectName(QStringLiteral("announcementBar"));
    m_announcement->setWordWrap(true);
    m_announcement->setMaximumWidth(620);
    m_announcement->setContentsMargins(16, 8, 16, 2);
    m_announcement->hide();
    root->addWidget(m_announcement, 0, Qt::AlignLeft);

    // ---- 3. messageArea: QScrollArea + 垂直堆叠行, 底部 stretch 让消息贴顶 ----
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("messageArea"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_msgContainer = new QWidget;
    m_msgLayout = new QVBoxLayout(m_msgContainer);
    m_msgLayout->setContentsMargins(14, 12, 14, 12);
    m_msgLayout->setSpacing(10);
    m_msgLayout->addStretch(1);
    m_scroll->setWidget(m_msgContainer);
    root->addWidget(m_scroll, 1);

    // ---- 4. 输入区: 工具栏 + QTextEdit + 右下"发送" ----
    auto *inputPanel = new QWidget(this);
    inputPanel->setObjectName(QStringLiteral("chatInputPanel"));
    inputPanel->setStyleSheet(QStringLiteral(
        "#chatInputPanel { background:%1; border-top:1px solid %2; }")
        .arg(Theme::panelBg().name(), Theme::divider().name()));
    auto *iv = new QVBoxLayout(inputPanel);
    iv->setContentsMargins(14, 6, 14, 12);
    iv->setSpacing(4);

    auto *toolbar = new QWidget(inputPanel);
    toolbar->setObjectName(QStringLiteral("inputToolbar"));
    auto *th = new QHBoxLayout(toolbar);
    th->setContentsMargins(0, 0, 0, 0);
    th->setSpacing(10);
    auto addTool = [toolbar, th](const QString &text, const QString &tip) {
        auto *b = new QToolButton(toolbar);
        b->setText(text);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        th->addWidget(b);
        return b;
    };
    addTool(QStringLiteral("😊"), QStringLiteral("表情(开发中)"));   // 占位: 不接任何行为
    addTool(QStringLiteral("✂"), QStringLiteral("截图(开发中)"));
    QToolButton *fileBtn = addTool(QStringLiteral("📁"), QStringLiteral("发送文件"));
    QToolButton *imgBtn = addTool(QStringLiteral("🖼"), QStringLiteral("发送图片"));
    addTool(QStringLiteral("🎤"), QStringLiteral("语音(开发中)"));
    th->addStretch(1);
    iv->addWidget(toolbar);

    m_input = new QTextEdit(inputPanel);
    m_input->setObjectName(QStringLiteral("inputEdit"));
    m_input->setPlaceholderText(QStringLiteral("按住 Enter 换行，点击发送或按 Enter 发送"));
    m_input->setFixedHeight(96);
    m_input->setAcceptRichText(false);
    iv->addWidget(m_input);

    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(0);
    btnRow->addStretch(1);
    m_sendBtn = new QPushButton(QStringLiteral("发送"), inputPanel);
    m_sendBtn->setObjectName(QStringLiteral("primaryButton"));
    m_sendBtn->setStyleSheet(Theme::primaryButtonQss());
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    m_sendBtn->setFixedSize(88, 34);
    btnRow->addWidget(m_sendBtn);
    iv->addLayout(btnRow);
    root->addWidget(inputPanel);

    // ------------------------------------------------------------------
    // 输入区交互接线
    // ------------------------------------------------------------------
    connect(m_sendBtn, &QPushButton::clicked, this, &ChatView::onSendClicked);
    connect(imgBtn, &QToolButton::clicked, this, &ChatView::onPickImage);
    connect(fileBtn, &QToolButton::clicked, this, &ChatView::onPickFile);

    // Enter 发送 / Shift+Enter 换行: 键事件落在 QTextEdit 的 viewport 上
    auto *keyFilter = new KeyPressFilter([this](QKeyEvent *ev) {
        if (ev->key() != Qt::Key_Return && ev->key() != Qt::Key_Enter)
            return false;
        if (ev->modifiers().testFlag(Qt::ShiftModifier))
            return false;               // Shift+Enter → 默认插入换行
        onSendClicked();
        return true;
    }, this);
    m_input->viewport()->installEventFilter(keyFilter);

    // "⋯" → 群管理弹窗(非模态); "发私信"直接在本面板打开好友会话
    connect(m_moreBtn, &QPushButton::clicked, this, [this] {
        if (m_peerId == 0 || !m_isGroup)
            return;
        auto *dlg = new GroupManageDialog(m_im, m_peerId, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        connect(dlg, &GroupManageDialog::requestOpenFriend, this,
                [this](quint64 userId, const QString &title) {
                    openConversation(userId, false, title);
                });
        dlg->show();
    });

    // ------------------------------------------------------------------
    // ChatService 信号 → 渲染(只消费与当前会话匹配的 key)
    // ------------------------------------------------------------------
    ChatService *chat = m_im->chat();

    auto onNewMessage = [this](const QString &key, const ChatMessage &m) {
        if (m_peerId == 0 || key != currentConvKey())
            return;
        appendMessage(m);
        // 会话打开且窗口活跃 → 立即补已读(协议 5.10, 幂等)
        if (isActiveWindow())
            m_im->chat()->markRead(m_peerId, m_isGroup);
    };
    connect(chat, &ChatService::messageIncoming, this, onNewMessage);
    connect(chat, &ChatService::messageLocalAdded, this, onNewMessage);

    connect(chat, &ChatService::messageStatusChanged, this,
            [this](const QString &key, quint64 clientMsgId,
                   MessageStatus st, const QString &err) {
        if (m_peerId == 0 || key != currentConvKey())
            return;
        updateMessageStatus(clientMsgId, st, err);
    });

    connect(chat, &ChatService::historyLoaded, this,
            [this](const QString &key, const QList<ChatMessage> &, quint64, bool reachedEnd) {
        m_fetchingHistory = false;
        if (m_peerId == 0 || key != currentConvKey())
            return;
        m_noMoreHistory = reachedEnd;
        // 历史分页: 整表重建后按"滚动条增量"粗定位, 保持用户视觉位置不跳
        QScrollBar *sb = m_scroll->verticalScrollBar();
        const int oldVal = sb->value();
        const int oldMax = sb->maximum();
        rebuildFromStore();
        QTimer::singleShot(0, this, [this, oldVal, oldMax] {
            QScrollBar *s = m_scroll->verticalScrollBar();
            s->setValue(oldVal + qMax(0, s->maximum() - oldMax));
        });
    });

    // TCP 业务错误(102) → 面板右上角轻提示, 3s 自动消失
    connect(chat, &ChatService::tcpError, this,
            [this](const QString &reason) { showToast(reason); });

    // 滚到顶部 → 拉更早历史(beforeSeq=最旧 seq, 协议 5.9)
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        if (v > 2 || m_peerId == 0 || m_fetchingHistory || m_noMoreHistory)
            return;
        const QList<ChatMessage> msgs = m_im->chat()->messages(m_peerId, m_isGroup);
        quint64 oldest = 0;
        for (const ChatMessage &m : msgs) {
            if (m.seq > 0) { oldest = m.seq; break; }   // 存储按 seq 升序
        }
        if (oldest == 0)
            return;                                     // 尚无服务端消息, 无更早可拉
        m_fetchingHistory = true;
        m_im->chat()->requestHistory(m_peerId, m_isGroup, oldest, 20);
    });
}

// ===========================================================================
// 会话上下文
// ===========================================================================
QString ChatView::currentConvKey() const
{
    return ChatService::convKey(m_peerId, m_isGroup);
}

void ChatView::openConversation(quint64 peerId, bool isGroup, const QString &title)
{
    m_peerId = peerId;
    m_isGroup = isGroup;
    m_title = title;

    m_titleLabel->setText(title);
    m_onlineDot->setVisible(isGroup);
    m_moreBtn->setVisible(isGroup);
    if (!isGroup) {
        // 防御性清理(MainWindow 单聊路径也会调 clearGroupContext)
        m_announcement->hide();
        m_memberNames.clear();
        m_group = GroupInfo();
    }
    m_fetchingHistory = false;
    m_noMoreHistory = false;

    rebuildFromStore();

    // 首次打开该会话 → 拉最近 20 条(before_seq=0)
    if (!m_im->chat()->historyLoadedOnce(peerId, isGroup)) {
        m_fetchingHistory = true;
        m_im->chat()->requestHistory(peerId, isGroup, 0, 20);
    }
    m_im->chat()->markRead(peerId, isGroup);
    QTimer::singleShot(0, this, &ChatView::scrollToBottom);
}

void ChatView::setGroupContext(const GroupInfo &group, const QList<GroupMember> &members)
{
    if (!m_isGroup || m_peerId != group.groupId)
        return;   // 迟到的详情回调: 用户已切走则丢弃

    m_group = group;
    m_memberNames.clear();
    for (const GroupMember &gm : members)
        m_memberNames.insert(gm.userId, gm.displayName());

    if (!group.announcement.isEmpty()) {
        m_announcement->setText(QStringLiteral("公告: %1").arg(group.announcement));
        m_announcement->show();
    } else {
        m_announcement->hide();
    }
    m_onlineDot->setVisible(true);
    m_moreBtn->setVisible(true);
}

void ChatView::clearGroupContext()
{
    m_group = GroupInfo();
    m_memberNames.clear();
    m_announcement->hide();
    m_onlineDot->hide();
    m_moreBtn->hide();
}

// ===========================================================================
// 气泡行构建与渲染
// ===========================================================================
ChatView::MessageRow *ChatView::buildRowShell(bool outgoing, quint64 senderId,
                                              const QString &senderName)
{
    auto *r = new MessageRow;
    r->outgoing = outgoing;
    r->row = new QWidget(m_msgContainer);
    auto *h = new QHBoxLayout(r->row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);   // 头像与气泡的间距: 8 会让左侧看起来空出一列

    // 气泡: 圆角 8px + 1px 同色系描边(浅灰底在白色聊天区几乎看不出边界, 描边才像气泡)。
    // ⚠ 内边距用 QLabel::setMargin 而不是样式表 padding: QLabel 的尺寸计算不计入样式表
    //    padding, 多行文本最后一行会被裁掉(实测)。BubbleLabel 侧的公式同样按 margin 反推。
    r->bubble = new BubbleLabel(r->row);
    r->bubble->setWordWrap(true);
    r->bubble->setTextFormat(Qt::PlainText);
    // 垂直 Fixed: Minimum/Preferred 会让布局把 widget 拉高, 出现"气泡一大块、文字只占
    // 中间一条"的观感。水平 Maximum = 不超过尺寸上限, 短消息不会撑满整行。
    r->bubble->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    r->bubble->setMargin(8);
    // 不设 setIndent: 聊天气泡不需要首行缩进, 而 BubbleLabel 的宽高公式里没有 indent
    // 这一项, 加上会让"外框宽"和"排版内框宽"两个口径不一致(凭空多一行高)。
    QFont bf = r->bubble->font();
    bf.setPixelSize(14);
    r->bubble->setFont(bf);
    const QColor bg = outgoing ? Theme::bubbleOut() : Theme::bubbleIn();
    const QColor border = outgoing ? QColor(0xb9, 0xdc, 0xf5) : QColor(0xe2, 0xe6, 0xea);
    r->bubble->setStyleSheet(QStringLiteral(
        "QLabel { background:%1; border:1px solid %4; border-radius:%2px; color:%3; }")
        .arg(bg.name()).arg(Theme::radius()).arg(Theme::textPrimary().name()).arg(border.name()));
    // ⚠ 必须放在字体/margin 都设好之后: setMaxBubbleWidth 内部会 refit() 按当前字体与
    //    内边距锁定自身尺寸, 放前面会按默认 13px 字体算出偏小的气泡。
    //    文本是之后才 setText 的, 那里还会再 refit 一次(见 appendMessage)。
    r->bubble->setMaxBubbleWidth(bubbleMaxWidth(m_scroll ? m_scroll->viewport() : nullptr));

    if (!outgoing) {
        // 气泡布局(incoming): 头像 32px | [群聊发送者名] + 气泡, 整体左对齐
        r->avatar = new AvatarWidget(r->row);
        r->avatar->setAvatar(32, senderName, senderId);
        h->addWidget(r->avatar, 0, Qt::AlignTop);

        auto *col = new QVBoxLayout;
        col->setSpacing(2);
        if (m_isGroup) {
            r->nameLabel = new QLabel(senderName, r->row);
            r->nameLabel->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                                            .arg(Theme::textSecondary().name()));
            col->addWidget(r->nameLabel, 0, Qt::AlignLeft);
        }
        // 靠左对齐。可以安全带上 alignment: 之前"带 alignment 会裁掉多行文本"是因为
        // QLabel 的 sizeHint 不含 heightForWidth, 现在 BubbleLabel 两个都重写了,
        // 布局拿到的就是"该宽度换行后的真实尺寸"。
        col->addWidget(r->bubble, 0, Qt::AlignLeft);
        h->addLayout(col, 1);
    } else {
        // 气泡布局(outgoing): 右对齐气泡 + 右侧灰色状态标签
        h->addStretch(1);
        h->addWidget(r->bubble, 0, Qt::AlignRight);
        r->statusLabel = new QLabel(r->row);
        r->statusLabel->setStyleSheet(QStringLiteral("color:%1; font-size:12px;")
                                          .arg(Theme::textSecondary().name()));
        h->addWidget(r->statusLabel, 0, Qt::AlignVCenter);
    }
    return r;
}

void ChatView::insertRow(MessageRow *row)
{
    // 插到底部 stretch 之前, 保证消息贴顶、空白在下
    m_msgLayout->insertWidget(m_msgLayout->count() - 1, row->row);
    m_rows.append(row);
}

/// 窗口尺寸变化: 气泡最大宽度始终维持"可视宽 3/4"(用户定案的展示比例)
void ChatView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (!m_scroll)
        return;
    const int maxW = bubbleMaxWidth(m_scroll->viewport());
    for (MessageRow *r : m_rows) {
        if (r && r->bubble)
            r->bubble->setMaxBubbleWidth(maxW);   // 内部会 updateGeometry 触发重排
    }
}

void ChatView::dropRowWidget(QWidget *rowWidget)
{
    if (!rowWidget)
        return;
    for (int i = 0; i < m_rows.size(); ++i) {
        MessageRow *r = m_rows[i];
        if (r->row == rowWidget) {
            m_msgLayout->removeWidget(r->row);
            delete r->row;
            delete r;
            m_rows.removeAt(i);
            return;
        }
    }
}

void ChatView::appendMessage(const ChatMessage &m)
{
    QString senderName;
    if (!m.outgoing) {
        // 群聊发送者名走成员缓存, 缺失时退化 "用户<ID>"
        senderName = m_isGroup
            ? m_memberNames.value(m.fromUid, QStringLiteral("用户%1").arg(m.fromUid))
            : m_title;
    }
    MessageRow *r = buildRowShell(m.outgoing, m.fromUid, senderName);
    r->clientMsgId = m.clientMsgId;

    if (m.type == MsgType::Text)
        r->bubble->setText(m.content);
    else
        showBubbleMedia(r, m);

    // 文本落地后必须重算并锁死气泡尺寸(BubbleLabel::refit 的机制说明)
    r->bubble->refit();

    if (r->statusLabel)
        r->statusLabel->setText(statusTextFor(m.status, QString()));

    // 自己发的或本来就在底部附近才跟随滚动, 避免翻历史时被拽走
    QScrollBar *sb = m_scroll->verticalScrollBar();
    const bool follow = m.outgoing || sb->maximum() - sb->value() <= 60;
    insertRow(r);
    if (follow)
        QTimer::singleShot(0, this, &ChatView::scrollToBottom);
}

void ChatView::rebuildFromStore()
{
    // 整表重建(会话消息量级小, 不做 diff): 旧行连同上传占位行一起销毁,
    // 外部回调侧用 QPointer 判空, 不会触碰已销毁控件
    while (!m_rows.isEmpty()) {
        MessageRow *r = m_rows.takeFirst();
        m_msgLayout->removeWidget(r->row);
        delete r->row;
        delete r;
    }
    const QList<ChatMessage> msgs = m_im->chat()->messages(m_peerId, m_isGroup);
    for (const ChatMessage &m : msgs)
        appendMessage(m);
}

void ChatView::updateMessageStatus(quint64 clientMsgId, MessageStatus st, const QString &err)
{
    for (MessageRow *r : m_rows) {
        if (r->outgoing && r->clientMsgId == clientMsgId && r->statusLabel) {
            r->statusLabel->setText(statusTextFor(st, err));
            if (st == MessageStatus::Failed)
                r->row->setToolTip(err);
            return;
        }
    }
}

QString ChatView::statusTextFor(MessageStatus st, const QString &err)
{
    switch (st) {
    case MessageStatus::Sending:   return QStringLiteral("[发送中]");
    case MessageStatus::Sent:      return QStringLiteral("[已发送]");
    case MessageStatus::Duplicate: return QStringLiteral("[已发送(去重)]");
    case MessageStatus::Failed:
        return err.isEmpty() ? QStringLiteral("[发送失败]")
                             : QStringLiteral("[发送失败: %1]").arg(err);
    }
    return QString();
}

// ===========================================================================
// 媒体消息: 渲染(缩略图/文件行) + 带鉴权下载 + 另存为
// ===========================================================================
void ChatView::showBubbleMedia(MessageRow *row, const ChatMessage &m)
{
    const QJsonObject meta = m.mediaMeta();
    const QString url = meta.value(QStringLiteral("url")).toString();
    const QString name = meta.value(QStringLiteral("name")).toString();
    const qint64 size = static_cast<qint64>(meta.value(QStringLiteral("size")).toDouble());
    const bool isImage = (m.type == MsgType::Image);

    // 发送方本地 content 只有 {"file_id":...}(服务端重建权威元数据, 协议 5.8),
    // 拿不到 url/name 时只显示占位文本
    if (url.isEmpty()) {
        row->bubble->setText(isImage ? QStringLiteral("🖼 图片消息")
                                     : QStringLiteral("📄 文件消息"));
        return;
    }

    // 媒体气泡可点击 = 另存为
    row->bubble->setCursor(Qt::PointingHandCursor);
    connect(row->bubble, &BubbleLabel::clicked, this, [this, url, name] {
        downloadAndSave(url, name);
    });

    if (!isImage) {
        row->bubble->setText(QStringLiteral("📄 %1 (%2)  点击下载").arg(name, humanSize(size)));
        return;
    }

    // 图片: 先拉缩略图(≤240x240); 行可能在下载完成前被切会话销毁 → QPointer 守护
    row->bubble->setText(QStringLiteral("🖼 图片加载中…"));
    QPointer<BubbleLabel> guard(row->bubble);
    mediaGet(url, [guard, name](int status, const QByteArray &body) {
        if (!guard)
            return;
        QImage img;
        img.loadFromData(body);
        const QPixmap pix = QPixmap::fromImage(img);
        if (status != 200 || pix.isNull()) {
            guard->setText(QStringLiteral("🖼 %1 (点击另存)").arg(name));
            guard->refit();                 // 异步落地: 外层那次 refit 早跑完了
            return;
        }
        guard->setText(QString());
        guard->setPixmap(pix.scaled(kThumbMaxSize, kThumbMaxSize,
                                    Qt::KeepAspectRatio, Qt::SmoothTransformation));
        guard->refit();                     // 缩略图尺寸由 pixmap 决定, 需重新锁定
    });
}

void ChatView::mediaGet(const QString &url, std::function<void(int, QByteArray)> cb)
{
    // 媒体下载鉴权: 消息里的 url 恒为服务器换取入口(GET 302 → 签名直链,
    // QNetworkAccessManager 默认自动跟随重定向), 必须带 Bearer
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Authorization",
                     QStringLiteral("Bearer %1")
                         .arg(m_im->session()->accessToken()).toUtf8());
    QNetworkReply *rep = m_nam->get(req);
    connect(rep, &QNetworkReply::finished, this, [rep, cb] {
        const int http = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = rep->readAll();
        const int status = http ? http
                                : (rep->error() != QNetworkReply::NoError ? -1 : 200);
        rep->deleteLater();
        if (cb)
            cb(status, body);
    });
}

void ChatView::downloadAndSave(const QString &serverUrl, const QString &suggestName)
{
    showToast(QStringLiteral("正在下载 %1…").arg(suggestName));
    mediaGet(serverUrl, [this, suggestName](int status, const QByteArray &body) {
        if (status != 200 || body.isEmpty()) {
            showToast(QStringLiteral("下载失败 (HTTP %1)").arg(status));
            return;
        }
        const QString fn = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存文件"), suggestName);
        if (fn.isEmpty())
            return;
        QFile f(fn);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(body);
            f.close();
            showToast(QStringLiteral("已保存: %1").arg(QFileInfo(fn).fileName()));
        } else {
            showToast(QStringLiteral("保存失败: %1").arg(f.errorString()));
        }
    });
}

// ===========================================================================
// 发送: 文本 / 图片 / 文件(上传占位 → sendMedia 接力真实气泡)
// ===========================================================================
void ChatView::onSendClicked()
{
    if (m_peerId == 0) {
        showToast(QStringLiteral("请先打开一个会话"));
        return;
    }
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty())
        return;
    // 渲染统一走 messageLocalAdded 回调, 此处只管发起与清空输入
    m_im->chat()->sendText(m_peerId, m_isGroup, text);
    m_input->clear();
}

void ChatView::onPickImage()
{
    if (m_peerId == 0) {
        showToast(QStringLiteral("请先打开一个会话"));
        return;
    }
    const QString fn = QFileDialog::getOpenFileName(
        this, QStringLiteral("发送图片"), QString(),
        QStringLiteral("图片文件 (*.jpg *.jpeg *.png *.gif *.webp *.bmp)"));
    if (!fn.isEmpty())
        startMediaUpload(fn, MsgType::Image);
}

void ChatView::onPickFile()
{
    if (m_peerId == 0) {
        showToast(QStringLiteral("请先打开一个会话"));
        return;
    }
    const QString fn = QFileDialog::getOpenFileName(
        this, QStringLiteral("发送文件"), QString(),
        QStringLiteral("常用文件 (*.pdf *.txt *.md *.doc *.docx *.xls *.xlsx "
                       "*.ppt *.pptx *.zip *.rar *.7z *.mp3 *.mp4 *.wav)"));
    if (!fn.isEmpty())
        startMediaUpload(fn, MsgType::File);
}

void ChatView::startMediaUpload(const QString &filePath, int msgType)
{
    const QFileInfo info(filePath);
    // 发送目标在上传开始时就冻结: 中途切会话仍发往原对端, 只放弃旧行 UI 更新
    const quint64 peer = m_peerId;
    const bool isGroup = m_isGroup;

    // 上传中占位行(还不是 ChatMessage, 完成后由真实气泡替换)
    MessageRow *r = buildRowShell(true, m_im->session()->userId(), m_title);
    r->bubble->setText((msgType == MsgType::Image ? QStringLiteral("🖼 ")
                                                  : QStringLiteral("📄 "))
                       + info.fileName());
    r->bubble->refit();     // 占位行不走 appendMessage, 要自己锁一次尺寸
    r->statusLabel->setText(QStringLiteral("上传中 0%"));
    insertRow(r);
    QTimer::singleShot(0, this, &ChatView::scrollToBottom);

    struct Ctx {
        QPointer<QWidget> rowWidget;   // 切会话重建后自动置空, 回调不再触碰 UI
        QPointer<QLabel> status;
    };
    auto ctx = std::make_shared<Ctx>();
    ctx->rowWidget = r->row;
    ctx->status = r->statusLabel;

    m_im->uploader()->upload(
        filePath,
        [ctx](qint64 sent, qint64 total) {
            if (!ctx->status)
                return;
            const int pct = total > 0 ? static_cast<int>(sent * 100 / total) : 0;
            ctx->status->setText(QStringLiteral("上传中 %1%").arg(pct));
        },
        [this, ctx, peer, isGroup, msgType](const FileUploader::Result &res) {
            if (res.ok) {
                // 撤掉占位行 → sendMedia 触发 messageLocalAdded 追加真实气泡
                dropRowWidget(ctx->rowWidget.data());
                m_im->chat()->sendMedia(peer, isGroup, msgType,
                                         res.meta.fileId, res.meta.name);
            } else if (ctx->status) {
                ctx->status->setText(QStringLiteral("上传失败: %1").arg(res.error));
            }
        });
}

// ===========================================================================
// 滚动
// ===========================================================================
void ChatView::scrollToBottom()
{
    QScrollBar *sb = m_scroll->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void ChatView::showToast(const QString &text)
{
    // 轻提示: 面板右上角深色小条, 3s 自动隐藏(不弹模态框打断操作)
    if (!m_toast) {
        m_toast = new QLabel(this);
        m_toast->setStyleSheet(QStringLiteral(
            "QLabel { background:%1; color:white; border-radius:6px;"
            " padding:6px 12px; font-size:12px; }")
            .arg(Theme::textPrimary().name()));
        m_toast->setWordWrap(true);
        m_toast->setMaximumWidth(320);
    }
    m_toast->setText(text);
    m_toast->adjustSize();
    m_toast->move(width() - m_toast->width() - 16, 66);
    m_toast->show();
    m_toast->raise();
    QTimer::singleShot(3000, m_toast, &QWidget::hide);
}
