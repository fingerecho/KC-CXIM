#include "framelesswindow.h"

#include <QEvent>
#include <QMouseEvent>
#include <QWindow>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#endif

FramelessWindow::FramelessWindow(QWidget *parent) : QWidget(parent)
{
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    // 透明背景允许子布局做出圆角卡片(登录界面)
    setAttribute(Qt::WA_TranslucentBackground, false);
}

void FramelessWindow::setTitleBar(QWidget *bar)
{
    m_titleBar = bar;
    if (bar)
        bar->installEventFilter(this);
}

bool FramelessWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_titleBar && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            // 原生系统拖动: 返回 false 让后续双击等事件继续正常分发
            if (windowHandle()->startSystemMove())
                return true;
        }
    }
    if (watched == m_titleBar && event->type() == QEvent::MouseButtonDblClick) {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

#ifdef Q_OS_WIN
// 无边框窗口边缘缩放: 交给系统 NC 命中测试, 在客户区边缘模拟 HTLEFT 等结果。
// ⚠ 高 DPI 关键: WM_NCHITTEST 的 lParam 是物理像素, 而 frameGeometry() 是逻辑
// 坐标(本机 225% 缩放)。不换算的话右/下半窗口会被误判成缩放手 → 鼠标变 ↔
// 调整箭头、点击被系统吃掉(这就是登录后主窗口右侧点不动的根因)。
bool FramelessWindow::nativeEvent(const QByteArray &, void *message, qintptr *result)
{
    if (!m_resizable)
        return false;
    MSG *msg = static_cast<MSG *>(message);
    if (msg->message != WM_NCHITTEST)
        return false;

    const qreal dpr = devicePixelRatioF();
    const qreal x = GET_X_LPARAM(msg->lParam) / dpr;
    const qreal y = GET_Y_LPARAM(msg->lParam) / dpr;
    const QRectF g = QRectF(frameGeometry());
    const qreal border = 6.0;

    const bool left = x < g.left() + border;
    const bool right = x >= g.right() - border;
    const bool top = y < g.top() + border;
    const bool bottom = y >= g.bottom() - border;

    if (isMaximized())
        return false;
    if (top && left) *result = HTTOPLEFT;
    else if (top && right) *result = HTTOPRIGHT;
    else if (bottom && left) *result = HTBOTTOMLEFT;
    else if (bottom && right) *result = HTBOTTOMRIGHT;
    else if (left) *result = HTLEFT;
    else if (right) *result = HTRIGHT;
    else if (top) *result = HTTOP;
    else if (bottom) *result = HTBOTTOM;
    else return false;
    return true;
}
#else
bool FramelessWindow::nativeEvent(const QByteArray &, void *, qintptr *)
{
    return false;
}
#endif
