#include "messagebubble.h"

#include <QKeyEvent>
#include <QMouseEvent>

BubbleLabel::BubbleLabel(QWidget *parent)
    : QLabel(parent)
{
}

void BubbleLabel::mouseReleaseEvent(QMouseEvent *e)
{
    // 左键在控件范围内抬起 → 视为点击(媒体气泡 = 另存为入口)
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()))
        emit clicked();
    QLabel::mouseReleaseEvent(e);
}

KeyPressFilter::KeyPressFilter(Handler handler, QObject *parent)
    : QObject(parent)
    , m_handler(std::move(handler))
{
}

bool KeyPressFilter::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && m_handler) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (m_handler(ke))
            return true;    // 已消费(例如 Enter 发送), 不再落到输入框
    }
    return QObject::eventFilter(watched, event);
}
