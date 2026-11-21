/**
 * @file loginwindow.cpp
 * @brief 登录页实现(视觉对照 docs/desktop_real_login_ui_0.png)
 *
 * 纯代码布局(无 .ui): 渐变圆角卡片 + 白底输入框 + 圆形勾选 + 渐变主按钮,
 * 颜色/圆角全部取自 Theme, 不引入任何外部图片资源(logo 用 QPixmap 自绘)。
 */
#include "loginwindow.h"

#include "registerdialog.h"
#include "../common/theme.h"
#include "../../core/session.h"
#include "../../service/imservice.h"

#include <QCheckBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 卡片尺寸: 宽:高 = 2:3(与截图口径一致)
constexpr int kCardWidth = 320;   // 2/3 缩放(原 480)
constexpr int kCardHeight = 480;  // 2/3 缩放(原 720)

/// 行内错误提示标签的 objectName(不弹窗, 避免打断输入)
const char kInlineError[] = "inlineError";

/// 品牌圆标: 白色圆盘 + 渐变蓝圆 + 白色链环(免图片资源)
QPixmap brandLogoPixmap(int diameter)
{
    QPixmap pm(diameter, diameter);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal d = diameter;

    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);                       // 白色圆盘
    p.drawEllipse(QRectF(0, 0, d, d));

    const qreal inner = d * 0.54;                // 品牌蓝圆
    const QRectF ir((d - inner) / 2.0, (d - inner) / 2.0, inner, inner);
    QLinearGradient g(ir.topLeft(), ir.bottomRight());
    g.setColorAt(0.0, Theme::brandTop());
    g.setColorAt(1.0, Theme::brandBottom());
    p.setBrush(g);
    p.drawEllipse(ir);

    p.save();                                    // 链环: 两个交叠圆角环, 旋转 45°
    p.translate(ir.center());
    p.rotate(-45);
    QPen wp(Qt::white);
    wp.setWidthF(inner * 0.12);
    wp.setCapStyle(Qt::RoundCap);
    wp.setJoinStyle(Qt::RoundJoin);
    p.setPen(wp);
    p.setBrush(Qt::NoBrush);
    const qreal w = inner * 0.42;
    const qreal h = inner * 0.24;
    const qreal off = inner * 0.15;
    p.drawRoundedRect(QRectF(-w / 2 - off, -h / 2, w, h), h / 2, h / 2);
    p.drawRoundedRect(QRectF(-w / 2 + off, -h / 2, w, h), h / 2, h / 2);
    p.restore();

    return pm;
}

/// 卡片右上角的扁平小按钮(≡ / ✕)
QPushButton *cornerButton(const QString &text, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setFixedSize(20, 20);
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: none; color: %1;"
        " font-size: 12px; }"
        "QPushButton:hover { background: rgba(0,0,0,0.07); border-radius: 6px; }")
        .arg(Theme::textPrimary().name()));
    return b;
}

/// 白底圆角输入框(objectName=cardInput, 文字居中)
QLineEdit *cardInput(const QString &placeholder, QWidget *parent, bool secret)
{
    auto *edit = new QLineEdit(parent);
    edit->setObjectName(QStringLiteral("cardInput"));
    edit->setPlaceholderText(placeholder);
    edit->setAlignment(Qt::AlignCenter);
    edit->setFixedHeight(37);
    edit->setStyleSheet(QStringLiteral("QLineEdit#cardInput { font-size: 13px; padding: 6px; }"));
    if (secret)
        edit->setEchoMode(QLineEdit::Password);
    return edit;
}

/// 圆形勾选框(objectName=circleCheck, 样式由全局 QSS 提供)
QCheckBox *circleCheck(const QString &text, QWidget *parent)
{
    auto *cb = new QCheckBox(text, parent);
    cb->setObjectName(QStringLiteral("circleCheck"));
    cb->setCursor(Qt::PointingHandCursor);
    cb->setStyleSheet(QStringLiteral("QCheckBox { font-size: 12px; }"));
    return cb;
}

/// 文字链接按钮(objectName=linkButton)
QPushButton *linkButton(const QString &text, QWidget *parent, int pixelSize = 12)
{
    auto *b = new QPushButton(text, parent);
    b->setObjectName(QStringLiteral("linkButton"));
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("QPushButton#linkButton { font-size: %1px; }")
                         .arg(pixelSize));
    return b;
}

/// 显示/清空行内错误(text 为空即隐藏)
void showInlineError(QWidget *card, const QString &text)
{
    auto *label = card->findChild<QLabel *>(QString::fromLatin1(kInlineError));
    if (!label)
        return;
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

} // namespace

// ---------------------------------------------------------------------------
LoginWindow::LoginWindow(ImService *im, QWidget *parent)
    : FramelessWindow(parent)
    , m_im(im)
{
    setWindowTitle(QStringLiteral("铖信 - 登录"));
    setFixedSize(kCardWidth, kCardHeight);
    setResizable(false);                        // 固定尺寸卡片不允许拖边缩放
    // 圆角卡片需要逐像素透明背景(基类默认为不透明)
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 顶部: 右上角 ≡(占位) / ✕(关闭), 整行同时作为拖动标题区 ----
    auto *header = new QWidget(this);
    header->setFixedHeight(31);
    auto *hh = new QHBoxLayout(header);
    hh->setContentsMargins(0, 7, 8, 0);
    hh->setSpacing(2);
    hh->addStretch(1);
    auto *menuBtn = cornerButton(QStringLiteral("≡"), header);
    auto *closeBtn = cornerButton(QStringLiteral("✕"), header);
    hh->addWidget(menuBtn);
    hh->addWidget(closeBtn);
    connect(closeBtn, &QPushButton::clicked, this, &QWidget::close);
    Q_UNUSED(menuBtn);                          // ≡: 菜单占位, 暂不接任何动作
    setTitleBar(header);
    root->addWidget(header);

    // ---- 主体 ----
    auto *body = new QWidget(this);
    auto *bv = new QVBoxLayout(body);
    bv->setContentsMargins(29, 3, 29, 17);
    bv->setSpacing(8);

    auto *logo = new QLabel(body);
    logo->setPixmap(brandLogoPixmap(93));
    logo->setAlignment(Qt::AlignHCenter);
    bv->addWidget(logo);
    bv->addSpacing(9);

    m_userEdit = cardInput(QStringLiteral("输入账号"), body, false);
    m_passEdit = cardInput(QStringLiteral("输入密码"), body, true);
    bv->addWidget(m_userEdit);
    bv->addWidget(m_passEdit);

    // 自动登录(左) / 记住密码(右)
    auto *optRow = new QHBoxLayout;
    optRow->setContentsMargins(4, 0, 4, 0);
    m_autoLogin = circleCheck(QStringLiteral("自动登录"), body);
    m_remember = circleCheck(QStringLiteral("记住密码"), body);
    optRow->addWidget(m_autoLogin);
    optRow->addStretch(1);
    optRow->addWidget(m_remember);
    bv->addLayout(optRow);

    // 自动登录依赖记住密码(Session::setLoginPreferences 同口径)
    connect(m_remember, &QCheckBox::toggled, this, [this](bool on) {
        if (!on)
            m_autoLogin->setChecked(false);
    });
    connect(m_autoLogin, &QCheckBox::toggled, this, [this](bool on) {
        if (on)
            m_remember->setChecked(true);
    });

    bv->addStretch(1);

    m_loginBtn = new QPushButton(QStringLiteral("登录"), body);
    m_loginBtn->setObjectName(QStringLiteral("primaryButton"));
    m_loginBtn->setStyleSheet(Theme::primaryButtonQss());
    m_loginBtn->setFixedHeight(35);
    m_loginBtn->setStyleSheet(Theme::primaryButtonQss() + QStringLiteral("QPushButton{font-size:14px;}"));
    m_loginBtn->setCursor(Qt::PointingHandCursor);
    bv->addWidget(m_loginBtn);

    // 行内错误提示(登录失败/未勾选协议)
    auto *errLabel = new QLabel(body);
    errLabel->setObjectName(QString::fromLatin1(kInlineError));
    errLabel->setAlignment(Qt::AlignCenter);
    errLabel->setWordWrap(true);
    errLabel->setStyleSheet(QStringLiteral(
        "QLabel { color: #e04a4a; font-size: 11px; background: transparent; }"));
    errLabel->hide();
    bv->addWidget(errLabel);

    // 协议行: circleCheck + 两个 linkButton
    auto *agreeRow = new QHBoxLayout;
    agreeRow->setContentsMargins(0, 0, 0, 0);
    agreeRow->addStretch(1);
    m_agreement = circleCheck(QStringLiteral("已阅读并同意"), body);
    auto *termBtn = linkButton(QStringLiteral("服务协议"), body);
    auto *andLabel = new QLabel(QStringLiteral(" 和 "), body);
    andLabel->setStyleSheet(QStringLiteral("color:%1; font-size:12px; background:transparent;")
                                .arg(Theme::textSecondary().name()));
    auto *privacyBtn = linkButton(QStringLiteral("隐私政策"), body);
    agreeRow->addWidget(m_agreement);
    agreeRow->addWidget(termBtn);
    agreeRow->addWidget(andLabel);
    agreeRow->addWidget(privacyBtn);
    agreeRow->addStretch(1);
    bv->addLayout(agreeRow);

    // 协议正文暂无本地文本, 先用占位弹窗
    connect(termBtn, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, QStringLiteral("服务协议"),
                                 QStringLiteral("服务协议正文将在官网提供, 此处为占位说明。"));
    });
    connect(privacyBtn, &QPushButton::clicked, this, [this] {
        QMessageBox::information(this, QStringLiteral("隐私政策"),
                                 QStringLiteral("隐私政策正文将在官网提供, 此处为占位说明。"));
    });

    bv->addSpacing(13);

    // 底部居中: 注册账号
    auto *regRow = new QHBoxLayout;
    regRow->setContentsMargins(0, 0, 0, 0);
    regRow->addStretch(1);
    auto *regBtn = linkButton(QStringLiteral("注册账号"), body, 13);
    regRow->addWidget(regBtn);
    regRow->addStretch(1);
    bv->addLayout(regRow);

    root->addWidget(body, 1);

    // ---- 交互 ----
    connect(m_loginBtn, &QPushButton::clicked, this, [this] {
        // 登录流程: 先校验协议与输入, 再走网络
        if (!m_agreement->isChecked()) {
            showInlineError(this, QStringLiteral("请先阅读并同意服务协议和隐私政策"));
            m_agreement->setFocus();
            return;
        }
        const QString user = m_userEdit->text().trimmed();
        const QString pass = m_passEdit->text();
        if (user.isEmpty() || pass.isEmpty()) {
            showInlineError(this, QStringLiteral("请输入账号和密码"));
            (user.isEmpty() ? m_userEdit : m_passEdit)->setFocus();
            return;
        }
        showInlineError(this, QString());
        doLogin(user, pass);
    });

    // 回车即登录
    connect(m_userEdit, &QLineEdit::returnPressed, this, [this] {
        m_passEdit->setFocus();
    });
    connect(m_passEdit, &QLineEdit::returnPressed, m_loginBtn, &QPushButton::click);

    // 注册窗口: 应用级模态(RegisterDialog 基于 FramelessWindow, 关闭即回登录页)
    connect(regBtn, &QPushButton::clicked, this, [this] {
        auto *dlg = new RegisterDialog(m_im, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowModality(Qt::ApplicationModal);
        dlg->move(geometry().center() - QPoint(dlg->width() / 2, dlg->height() / 2));
        dlg->show();
    });

    // ---- 回填本地偏好(QSettings 持久化) ----
    if (m_im) {
        Session *s = m_im->session();
        m_remember->setChecked(s->rememberPassword());
        m_autoLogin->setChecked(s->autoLogin());
        if (s->rememberPassword()) {
            m_userEdit->setText(s->savedUsername());
            m_passEdit->setText(s->savedPassword());
        }

        // 自动登录: 上次同时勾了"自动登录+记住密码"且本地存有账号密码, 开机直接发起
        if (s->autoLogin() && s->rememberPassword()
            && !s->savedUsername().isEmpty() && !s->savedPassword().isEmpty()) {
            const QString user = s->savedUsername();
            const QString pass = s->savedPassword();
            QTimer::singleShot(0, this, [this, user, pass] { doLogin(user, pass); });
        }
    }
}

// ---------------------------------------------------------------------------
/// 发起登录: 只做网络流程与状态恢复, 协议/空值校验由调用方(按钮点击)负责,
/// 这样自动登录路径可以绕过协议勾选直接复用本函数。
void LoginWindow::doLogin(const QString &username, const QString &password)
{
    if (!m_im || !m_loginBtn)
        return;
    if (!m_loginBtn->isEnabled())               // 请求进行中, 忽略重复触发
        return;

    showInlineError(this, QString());
    m_loginBtn->setEnabled(false);              // 期间禁用 + 文案"登录中..."
    m_loginBtn->setText(QStringLiteral("登录中..."));

    QPointer<LoginWindow> self(this);           // 回调可能晚于窗口析构(退出时)
    m_im->account()->login(username, password,
                           [self, username, password](bool ok, const QString &error) {
        if (!self)
            return;
        self->m_loginBtn->setEnabled(true);
        self->m_loginBtn->setText(QStringLiteral("登录"));

        if (!ok) {                              // 失败: 行内提示服务端原因
            showInlineError(self, error.isEmpty() ? QStringLiteral("登录失败") : error);
            return;
        }

        // 成功: 只有勾选"记住密码"才把密码落盘, 否则存空串
        const bool remember = self->m_remember->isChecked();
        self->m_im->session()->setLoginPreferences(
            remember, self->m_autoLogin->isChecked(),
            username, remember ? password : QString());

        // 拉起 TCP 数据面, 登录页的去留由组装处(main)接管
        self->m_im->startDataPlane();
        emit self->loginSucceeded();
    });
}

// ---------------------------------------------------------------------------
/// 背景: loginGradTop→loginGradBottom 纵向渐变 + 8px 圆角(透明区域保持透明)
void LoginWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);        // 圆角外区域必须清空
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::Antialiasing);

    QLinearGradient grad(0, 0, 0, height());
    grad.setColorAt(0.0, Theme::loginGradTop());
    grad.setColorAt(1.0, Theme::loginGradBottom());

    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                        Theme::radius(), Theme::radius());
    p.fillPath(card, grad);
    p.setPen(QColor(0, 0, 0, 26));              // 极淡描边, 浅桌面上区分边界
    p.drawPath(card);
}
