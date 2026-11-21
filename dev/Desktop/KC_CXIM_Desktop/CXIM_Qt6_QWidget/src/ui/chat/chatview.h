#ifndef CXIM_CHATVIEW_H
#define CXIM_CHATVIEW_H

#include <QWidget>

#include "../../service/imservice.h"

/**
 * @brief 聊天面板(对照 docs/desktop_real_group_ui_0.png 右侧区域)
 *
 * 布局(自上而下):
 *  1. chatHeader: 会话标题(群聊带在线点 ●)+ 右侧按钮区(群聊:"…" → GroupManageDialog);
 *  2. announcementBar: 仅群聊且有公告时显示"公告: xxx";
 *  3. messageArea: QScrollArea 内垂直堆叠气泡(incoming 左/白灰, outgoing 右/浅蓝),
 *     群聊 incoming 气泡上方显示发送者名; outgoing 气泡右侧灰色状态标签
 *     [发送中]/[已发送]/[已发送(去重)]/[发送失败: 原因]/[上传中 45%];
 *     图片消息显示缩略图(最大 240x240, 点击另存), 文件消息显示图标+名称+大小(点击另存);
 *  4. inputToolbar: 表情(占位)/图片/文件夹 三个图标按钮 + 下方 QTextEdit 输入区;
 *     右下角 primaryButton"发送"(Ctrl+Enter/Enter 也可发送)。
 *
 * 与功能层的连接(只允许通过 ImService 暴露的 service):
 *  - 发送: chat()->sendText / sendMedia; 媒体先 uploader()->upload 再 sendMedia,
 *    上传进度实时写进气泡状态(见 sendMediaFile 内部流程);
 *  - 渲染: messageLocalAdded/messageIncoming/messageStatusChanged/historyLoaded;
 *  - 历史: 打开会话且未拉过 → requestHistory(peer,isGroup,0,20);
 *    滚动到顶部且还有更早 → requestHistory(beforeSeq=oldest);
 *  - 已读: 打开会话与收到新消息且面板可见 → markRead;
 *  - 媒体下载: 元数据 url 是服务器换取入口(需 Bearer), 用 HttpClient::rawRequest
 *    附 Authorization 头取回字节(302 由 QNAM 自动跟随)。
 *
 * 实现文件: chatview.cpp(本头是跨模块契约, MainWindow 只依赖 openConversation/setGroupInfo)。
 */
class ChatView : public QWidget
{
    Q_OBJECT
public:
    explicit ChatView(ImService *im, QWidget *parent = nullptr);

    /// 打开会话(单聊/群聊通用); title 由调用方给(好友备注/群名)
    void openConversation(quint64 peerId, bool isGroup, const QString &title);
    /// 群聊上下文: 公告条 + "…"管理入口; 单聊调用 clearGroupContext()
    void setGroupContext(const GroupInfo &group, const QList<GroupMember> &members);
    void clearGroupContext();
    /// 当前打开的会话(未打开时 peerId=0)
    quint64 currentPeerId() const { return m_peerId; }
    bool currentIsGroup() const { return m_isGroup; }

private slots:
    void onSendClicked();
    void onPickImage();
    void onPickFile();
    void scrollToBottom();

protected:
    /// 窗口缩放时按"可视宽 3/4"重算气泡最大宽度
    void resizeEvent(QResizeEvent *event) override;

private:
    struct MessageRow;   // 内部气泡封装(定义在 .cpp)
    void rebuildFromStore();
    void appendMessage(const ChatMessage &m);
    void updateMessageStatus(quint64 clientMsgId, MessageStatus st, const QString &err);
    void downloadAndSave(const QString &serverUrl, const QString &suggestName);
    void startMediaUpload(const QString &filePath, int msgType);

    // ---- 实现内部辅助(定义在 chatview.cpp, 不属于跨模块契约) ----
    QString currentConvKey() const;
    MessageRow *buildRowShell(bool outgoing, quint64 senderId, const QString &senderName);
    void insertRow(MessageRow *row);
    void dropRowWidget(class QWidget *rowWidget);
    void showBubbleMedia(MessageRow *row, const ChatMessage &m);
    void mediaGet(const QString &url, std::function<void(int, QByteArray)> cb);
    void showToast(const QString &text);
    static QString statusTextFor(MessageStatus st, const QString &err);

    ImService *m_im;

    // 会话上下文
    quint64 m_peerId = 0;
    bool m_isGroup = false;
    QString m_title;
    GroupInfo m_group;
    QHash<quint64, QString> m_memberNames;   // 群聊: userId → 展示名

    class QLabel *m_titleLabel = nullptr;
    class QPushButton *m_moreBtn = nullptr;
    class QLabel *m_announcement = nullptr;
    class QScrollArea *m_scroll = nullptr;
    class QWidget *m_msgContainer = nullptr;
    class QVBoxLayout *m_msgLayout = nullptr;
    class QTextEdit *m_input = nullptr;
    QList<MessageRow *> m_rows;              // 与 ChatService::messages 顺序一致

    // ---- 实现内部状态(非跨模块契约) ----
    bool m_fetchingHistory = false;          // 历史请求在途(滚动分页节流)
    bool m_noMoreHistory = false;            // 服务端已返回 reachedEnd
    class QNetworkAccessManager *m_nam = nullptr;  // 媒体下载(带 Bearer 的换取入口)
    class QLabel *m_toast = nullptr;         // 轻提示(面板右上角, 3s 自动隐藏)
    class QLabel *m_onlineDot = nullptr;     // 群聊标题后的绿色在线点 "●"
    class QPushButton *m_sendBtn = nullptr;  // 右下角"发送"
};

#endif // CXIM_CHATVIEW_H
