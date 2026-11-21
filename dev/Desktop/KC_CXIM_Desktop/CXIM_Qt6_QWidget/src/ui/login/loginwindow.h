#ifndef CXIM_LOGINWINDOW_H
#define CXIM_LOGINWINDOW_H

#include "../common/framelesswindow.h"

class ImService;

/**
 * @brief 登录窗口(对照 docs/desktop_real_login_ui_0.png)
 *
 * 视觉规范:
 *  - 无边框卡片窗口, 宽:高 = 2:3(建议 480x720), 圆角 8px;
 *  - 背景为 loginGradTop→loginGradBottom 的浅粉蓝纵向渐变(paintEvent 绘制);
 *  - 顶部居中: 白色圆盘 + 品牌 logo 圆标(可用 Theme 色自绘, 无需图片资源);
 *  - 两个白底圆角输入框(objectName=cardInput): 占位文本"输入账号"/"输入密码"
 *    (密码框 setEchoMode(Password)), 文字居中;
 *  - 输入框下方左右: 圆形勾选(18px, objectName=circleCheck)"自动登录"/"记住密码";
 *  - 主按钮"登录"(objectName=primaryButton, 渐变蓝, 高约 52px);
 *  - 其下居中一行: circleCheck "已阅读并同意" + 两个 linkButton "服务协议"/"隐私政策";
 *  - 底部居中 linkButton "注册账号"。
 * 右上角另有一组小按钮: ≡(菜单占位, 可 no-op)与 ✕(关闭窗口)。
 *
 * 行为:
 *  - 登录前必须勾选协议, 否则提示"请先阅读并同意服务协议和隐私政策";
 *  - 调用 im->account()->login(), 期间按钮文案"登录中..."并禁用;
 *  - 成功: 按勾选状态写 Session::setLoginPreferences, 调 im->startDataPlane(),
 *    emit loginSucceeded()(组装处负责创建 MainWindow 并关闭本窗口);
 *  - 启动时若 remember/autoLogin 且有存量账号密码, 自动发起一次登录;
 *  - 点"注册账号"弹 RegisterDialog(模态)。
 */
class LoginWindow : public FramelessWindow
{
    Q_OBJECT
public:
    explicit LoginWindow(ImService *im, QWidget *parent = nullptr);

signals:
    void loginSucceeded();

protected:
    /// 绘制 loginGradTop→loginGradBottom 纵向渐变 + 8px 圆角卡片背景
    void paintEvent(QPaintEvent *event) override;

private:
    void doLogin(const QString &username, const QString &password);

    ImService *m_im;
    class QLineEdit *m_userEdit = nullptr;
    class QLineEdit *m_passEdit = nullptr;
    class QPushButton *m_loginBtn = nullptr;
    class QCheckBox *m_autoLogin = nullptr;
    class QCheckBox *m_remember = nullptr;
    class QCheckBox *m_agreement = nullptr;
};

#endif // CXIM_LOGINWINDOW_H
