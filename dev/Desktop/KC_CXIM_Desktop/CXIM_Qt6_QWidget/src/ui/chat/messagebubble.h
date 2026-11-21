#ifndef CXIM_MESSAGEBUBBLE_H
#define CXIM_MESSAGEBUBBLE_H

#include <QLabel>
#include <QObject>

#include <functional>

class QKeyEvent;
class QMouseEvent;

/**
 * @file messagebubble.h
 * @brief 聊天气泡辅助控件(ChatView 内部实现细节, 不对外暴露)
 *
 * BubbleLabel: 文本/图片/文件统一用一种气泡控件承载;
 * 媒体消息把光标设为 pointer 并点击触发"另存为", 因此需要可点击的 QLabel。
 * (QLabel 本身没有 clicked 信号, 这里用鼠标释放事件补上。)
 *
 * KeyPressFilter: QTextEdit 的按键过滤器。Qt 中键事件落在滚动区域的
 * viewport 上, 不能直接在 QTextEdit 子类里改 chatview.h 契约, 所以用
 * 独立 QObject 事件过滤器实现 "Enter 发送 / Shift+Enter 换行"。
 */
class BubbleLabel : public QLabel
{
    Q_OBJECT
public:
    explicit BubbleLabel(QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *e) override;
};

class KeyPressFilter : public QObject
{
    Q_OBJECT
public:
    /// handler 返回 true 表示事件已消费(不再传递给 QTextEdit)
    using Handler = std::function<bool(QKeyEvent *)>;

    explicit KeyPressFilter(Handler handler, QObject *parent = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Handler m_handler;
};

#endif // CXIM_MESSAGEBUBBLE_H
