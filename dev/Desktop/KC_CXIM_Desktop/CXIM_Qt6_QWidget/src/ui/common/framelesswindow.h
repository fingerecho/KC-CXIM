#ifndef CXIM_FRAMELESSWINDOW_H
#define CXIM_FRAMELESSWINDOW_H

#include <QWidget>

class QWidget;

/**
 * @brief 无边框窗口基类(登录卡片与主窗口共用)
 *
 * 截图中的窗口均为自绘顶栏(蓝色标题栏/卡片右上角按钮), 因此:
 *  - 去边框 + 可透明(登录卡片圆角);
 *  - 标题区拖动使用 Qt 原生 QWindow::startSystemMove(支持系统级吸附/动画);
 *  - 双击标题区切换最大化。
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

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Qt6 Windows 用 qintptr 输出命中结果(64 位下与 qint64 同宽, 但签名必须一致)
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    QWidget *m_titleBar = nullptr;
    bool m_resizable = true;
};

#endif // CXIM_FRAMELESSWINDOW_H
