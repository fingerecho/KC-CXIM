#ifndef CXIM_CONVERSATIONLISTVIEW_H
#define CXIM_CONVERSATIONLISTVIEW_H

#include <QWidget>

#include "../../service/imservice.h"

/**
 * @brief 会话列表(消息页左列): 头像 | 标题+最近一条摘要 | 时间+未读红点
 *
 * 数据源是 ChatService 的本地会话存储(协议: 消息事实以服务端为准,
 * 列表只是其视图)。标题来自 MainWindow 注入的 titleFor() 回调 ——
 * 本控件不认识好友/群模型, 只认识 (peerId,isGroup) 键, 保持低耦合。
 */
class ConversationListView : public QWidget
{
    Q_OBJECT
public:
    using TitleFn = std::function<QString(quint64 peerId, bool isGroup)>;

    explicit ConversationListView(ImService *im, QWidget *parent = nullptr);
    void setTitleResolver(TitleFn fn) { m_titleFor = std::move(fn); }

    void refresh();                       // 从 ChatService 重建列表
    void selectConversation(quint64 peerId, bool isGroup);

signals:
    void activated(quint64 peerId, bool isGroup, const QString &title);

private:
    ImService *m_im;
    TitleFn m_titleFor;
    class QListWidget *m_list = nullptr;
};

#endif // CXIM_CONVERSATIONLISTVIEW_H
