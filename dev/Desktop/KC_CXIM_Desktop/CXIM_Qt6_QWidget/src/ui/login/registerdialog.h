#ifndef CXIM_REGISTERDIALOG_H
#define CXIM_REGISTERDIALOG_H

#include "../common/framelesswindow.h"

class ImService;

/**
 * @brief 注册窗口(对照 docs/desktop_real_register_ui_0.png)
 *
 * 视觉规范(与 LoginWindow 同一套卡片语言):
 *  - 无边框卡片, 2:3 比例, 圆角 8px, 同款渐变背景;
 *  - 顶部 logo 圆标 + 标题"注册账号"(粗体深色大字);
 *  - 三个白底圆角输入框: "输入用户名"/"输入密码"/"确认密码"(文字居中);
 *  - 底部左右两个按钮: "取消"(ghostButton 浅灰)与"完成"(primaryButton 渐变蓝);
 *  - 右上角 ≡ / ✕ 小按钮(✕ = reject)。
 *
 * 行为:
 *  - 本地校验: 用户名非空且 ≤64; 密码 6~72; 两次密码一致(不一致红框提示);
 *  - 调 im->account()->registerAccount(); 期间"完成"禁用显示"提交中...";
 *  - 成功: QMessageBox 提示"注册成功, 用户ID: N", accept() 回登录页;
 *  - 失败: 展示服务端 message(如用户名已存在)。
 */
class RegisterDialog : public FramelessWindow
{
    Q_OBJECT
public:
    explicit RegisterDialog(ImService *im, QWidget *parent = nullptr);

protected:
    /// 与登录页同款的渐变圆角卡片背景
    void paintEvent(QPaintEvent *event) override;

private:
    ImService *m_im;
    class QLineEdit *m_user = nullptr;
    class QLineEdit *m_pass = nullptr;
    class QLineEdit *m_pass2 = nullptr;
    class QPushButton *m_doneBtn = nullptr;
};

#endif // CXIM_REGISTERDIALOG_H
