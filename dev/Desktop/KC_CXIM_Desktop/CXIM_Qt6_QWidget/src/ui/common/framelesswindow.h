#ifndef CXIM_FRAMELESSWINDOW_H
#define CXIM_FRAMELESSWINDOW_H

#include <QWidget>

class QWidget;
class QShowEvent;   // showEvent 形参, 前向声明即可(避免拖进整个 qevent.h)

/**
 * @brief 无边框窗口基类(登录卡片与主窗口共用)
 *
 * 截图中的窗口均为自绘顶栏(蓝色标题栏/卡片右上角按钮), 因此:
 *  - 去边框 + 可透明(登录卡片圆角);
 *  - 标题区拖动使用 Qt 原生 QWindow::startSystemMove(支持系统级吸附/动画);
 *  - 双击标题区切换最大化;
 *  - 可选的 Win11 原生圆角/投影(见 setNativeFrameStyle, 主窗口开启)。
 * 子类只需把"标题栏控件"注册进来即可, 不关心实现。
 */
class FramelessWindow : public QWidget
{
    Q_OBJECT
public:
    explicit FramelessWindow(QWidget *parent = nullptr);

    /// 注册可拖动区域(通常是顶部 header 容器)
    void setTitleBar(QWidget *bar);

    /// 无边框窗口的缩放角(Windows 原生 hit-test 需要自行处理)
    void setResizable(bool on) { m_resizable = on; }

    /**
     * @brief 启用 Windows 11 原生窗口外观: 系统圆角 + 系统阴影
     *
     * 走 DWM 窗口属性, 由系统在**最终窗口表面**上裁角并绘制阴影, 相比
     * "透明背景 + 自绘圆角"(登录页用的那套)的优势:
     *  - 边缘是系统级抗锯齿, 不会有 setMask 的硬边锯齿;
     *  - 完全不经过 Qt 样式表, 贴边的 #headerBar / #railPanel 等控件零改动,
     *    内部控件的圆角(Theme::radius())不受影响;
     *  - 最大化时系统自动收起圆角, 子类无需处理窗口状态切换。
     *
     * 注意事项(故意为之的限制, 不是 bug):
     *  - 圆角半径由系统决定(Win11 即 8px 口径, 随 DPI 等比缩放), 不可自定义;
     *  - 仅 Win11 生效; Win10 与其他平台上 DwmSetWindowAttribute 返回失败,
     *    静默退化为原来的直角, 不提示也不崩溃;
     *  - 不要与 WA_TranslucentBackground 的自绘圆角窗口混用(会双重裁角),
     *    所以默认关闭, 由需要的窗口显式开启。
     *
     * @param rounded  是否开启系统圆角
     * @param shadowed 是否开启系统投影(无边框窗口默认无阴影, 需单独打开)
     */
    void setNativeFrameStyle(bool rounded, bool shadowed = false);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Qt6 Windows 用 qintptr 输出命中结果(64 位下与 qint64 同宽, 但签名必须一致)
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    // 首次/重新显示时应用 DWM 外观(此刻 HWND 必已存在, 避免构造期提前创建原生窗口)
    void showEvent(QShowEvent *event) override;

private:
    void applyNativeFrameStyle();   // 真正的 DwmSetWindowAttribute 调用(非 Windows 为空)

    QWidget *m_titleBar = nullptr;
    bool m_resizable = true;
    bool m_nativeRounded = false;   // 默认关: 登录卡片等自绘圆角窗口不需要
    bool m_nativeShadowed = false;
};

#endif // CXIM_FRAMELESSWINDOW_H
