#include "framelesswindow.h"

#include <QEvent>
#include <QMouseEvent>
#include <QShowEvent>
#include <QWindow>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <dwmapi.h>     // DwmSetWindowAttribute: Win11 圆角/阴影(需链接 dwmapi)

// 下面三个取值来自微软文档的 DWMWINDOWATTRIBUTE / DWM_WINDOW_CORNER_PREFERENCE。
// 这里用本地常量而不是头文件里的枚举名, 是因为 DWMWA_WINDOW_CORNER_PREFERENCE
// 只有较新的 Windows SDK 头才定义, 直接引用会让本文件在旧 SDK 上编译失败。
namespace {
constexpr DWORD kAttrCornerPreference = 33;  // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kCornerRound          = 2;   // DWMWCP_ROUND(系统标准圆角, Win11 为 8px)
constexpr DWORD kAttrNcCreation       = 2;   // DWMWA_NCREATION_ATTRIBUTES(打开系统阴影)
} // namespace
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

// ---------------------------------------------------------------------------
// Win11 原生外观: 圆角 + 阴影
//   只在窗口显示时应用(showEvent): 此时 HWND 一定已创建, 既不必在构造期调用
//   winId() 强制提前创建原生窗口, 也能在 hide→show 复用同一实例时重新生效。
// ---------------------------------------------------------------------------
void FramelessWindow::setNativeFrameStyle(bool rounded, bool shadowed)
{
    m_nativeRounded = rounded;
    m_nativeShadowed = shadowed;
}

void FramelessWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    applyNativeFrameStyle();
}

void FramelessWindow::applyNativeFrameStyle()
{
#ifdef Q_OS_WIN
    if (!m_nativeRounded && !m_nativeShadowed)
        return;                             // 未开启的窗口(如登录卡片)完全不受影响

    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd)
        return;

    if (m_nativeRounded) {
        const DWORD pref = kCornerRound;
        // Win10 上该属性不被支持, 返回 E_INVALIDARG 即静默保持直角
        DwmSetWindowAttribute(hwnd, kAttrCornerPreference, &pref, sizeof(pref));
    }
    if (m_nativeShadowed) {
        const BOOL enabled = TRUE;
        // 打开非客户区渲染 → 系统补上原生投影(FramelessWindowHint 下默认没有)
        DwmSetWindowAttribute(hwnd, kAttrNcCreation, &enabled, sizeof(enabled));
    }
#endif
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
