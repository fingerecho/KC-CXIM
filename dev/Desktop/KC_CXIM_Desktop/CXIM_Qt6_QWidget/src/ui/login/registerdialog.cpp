/**
 * @file registerdialog.cpp
 * @brief 注册页实现(视觉对照 docs/desktop_real_register_ui_0.png)
 *
 * 与 LoginWindow 同一套卡片语言: 480x720 渐变圆角卡片 + 白底居中输入框,
 * 底部左"取消"(ghost)右"完成"(primary)。
 */
#include "registerdialog.h"

#include "../common/theme.h"
#include "../../service/accountservice.h"
#include "../../service/imservice.h"

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
#include <QVBoxLayout>

namespace {

constexpr int kCardWidth = 320;   // 2/3 缩放(原 480)
constexpr int kCardHeight = 480;  // 2/3 缩放(原 720)

// 本地校验口径(与后端一致): 用户名 ≤64 字节, 密码 6~72 字节
constexpr int kMaxUserBytes = 64;
constexpr int kMinPassBytes = 6;
constexpr int kMaxPassBytes = 72;

const char kInlineError[] = "inlineError";

/// 品牌圆标(与登录页同款, 白盘 + 渐变蓝圆 + 白色链环)
QPixmap brandLogoPixmap(int diameter)
{
    QPixmap pm(diameter, diameter);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal d = diameter;

    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    p.drawEllipse(QRectF(0, 0, d, d));

    const qreal inner = d * 0.54;
    const QRectF ir((d - inner) / 2.0, (d - inner) / 2.0, inner, inner);
    QLinearGradient g(ir.topLeft(), ir.bottomRight());
    g.setColorAt(0.0, Theme::brandTop());
    g.setColorAt(1.0, Theme::brandBottom());
    p.setBrush(g);
    p.drawEllipse(ir);

    p.save();
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

/// 行内提示(text 为空即隐藏)
void showInlineError(QWidget *card, const QString &text, bool asError = true)
{
    auto *label = card->findChild<QLabel *>(QString::fromLatin1(kInlineError));
    if (!label)
        return;
    label->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px;"
                                        " background: transparent; }")
                             .arg(asError ? QStringLiteral("#e04a4a")
                                          : Theme::textSecondary().name()));
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

// 卡片输入框的紧凑样式(与 37px 固定高度匹配)。
// ⚠ 不能用 setStyleSheet("") "交回全局": 全局 QSS 是 15px 字号 + 12px 内边距,
// 在 37px 固定高度里会把文字上下截断(本次 bug 根因), 清错误态时必须恢复本样式。
const char kCardInputQss[] =
    "QLineEdit#cardInput { font-size: 13px; padding: 6px; }";

/// 校验不通过的输入框: 红框(保留紧凑字号/内边距, 含 :focus 态)
void markInvalid(QLineEdit *edit)
{
    if (!edit)
        return;
    edit->setStyleSheet(QStringLiteral(
        "QLineEdit#cardInput, QLineEdit#cardInput:focus"
        " { font-size: 13px; padding: 6px; border: 1px solid #e04a4a; }"));
}

void clearInvalid(QLineEdit *edit)
{
    if (edit)
        edit->setStyleSheet(QString::fromLatin1(kCardInputQss));
}

} // namespace

// ---------------------------------------------------------------------------
RegisterDialog::RegisterDialog(ImService *im, QWidget *parent)
    : FramelessWindow(parent)
    , m_im(im)
{
    setWindowTitle(QStringLiteral("铖信 - 注册账号"));
    setFixedSize(kCardWidth, kCardHeight);
    setResizable(false);
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 顶部: ≡(占位) / ✕(关闭 = reject) ----
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
    Q_UNUSED(menuBtn);
    setTitleBar(header);
    root->addWidget(header);

    // ---- 主体 ----
    auto *body = new QWidget(this);
    auto *bv = new QVBoxLayout(body);
    bv->setContentsMargins(29, 3, 29, 17);
    bv->setSpacing(9);

    auto *logo = new QLabel(body);
    logo->setPixmap(brandLogoPixmap(93));
    logo->setAlignment(Qt::AlignHCenter);
    bv->addWidget(logo);
    bv->addSpacing(3);

    auto *title = new QLabel(QStringLiteral("注册账号"), body);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 18px;"
                                        " font-weight: bold; background: transparent; }")
                             .arg(Theme::textPrimary().name()));
    bv->addWidget(title);
    bv->addSpacing(7);

    m_user = cardInput(QStringLiteral("输入用户名"), body, false);
    m_pass = cardInput(QStringLiteral("输入密码"), body, true);
    m_pass2 = cardInput(QStringLiteral("确认密码"), body, true);
    bv->addWidget(m_user);
    bv->addWidget(m_pass);
    bv->addWidget(m_pass2);

    // 输入即清掉上一次的错误状态(红框/提示)
    auto resetError = [this](QLineEdit *edit) {
        clearInvalid(edit);
        showInlineError(this, QString());
    };
    connect(m_user, &QLineEdit::textChanged, this, [resetError, this] { resetError(m_user); });
    connect(m_pass, &QLineEdit::textChanged, this, [resetError, this] { resetError(m_pass); });
    connect(m_pass2, &QLineEdit::textChanged, this, [resetError, this] { resetError(m_pass2); });

    bv->addStretch(1);

    auto *errLabel = new QLabel(body);
    errLabel->setObjectName(QString::fromLatin1(kInlineError));
    errLabel->setAlignment(Qt::AlignCenter);
    errLabel->setWordWrap(true);
    errLabel->hide();
    bv->addWidget(errLabel);

    // ---- 底部: 取消(ghost) / 完成(primary) ----
    auto *btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 4, 0, 0);
    btnRow->setSpacing(11);

    auto *cancelBtn = new QPushButton(QStringLiteral("取消"), body);
    cancelBtn->setObjectName(QStringLiteral("ghostButton"));
    cancelBtn->setStyleSheet(Theme::ghostButtonQss());
    cancelBtn->setFixedHeight(35);
    cancelBtn->setStyleSheet(Theme::ghostButtonQss() + QStringLiteral("QPushButton{font-size:12px;}"));
    cancelBtn->setCursor(Qt::PointingHandCursor);

    m_doneBtn = new QPushButton(QStringLiteral("完成"), body);
    m_doneBtn->setObjectName(QStringLiteral("primaryButton"));
    m_doneBtn->setStyleSheet(Theme::primaryButtonQss());
    m_doneBtn->setFixedHeight(35);
    m_doneBtn->setStyleSheet(Theme::primaryButtonQss() + QStringLiteral("QPushButton{font-size:14px;}"));
    m_doneBtn->setCursor(Qt::PointingHandCursor);

    btnRow->addWidget(cancelBtn, 1);
    btnRow->addWidget(m_doneBtn, 1);
    bv->addLayout(btnRow);

    root->addWidget(body, 1);

    // 取消 = reject(): RegisterDialog 继承 FramelessWindow(非 QDialog),
    // 用 close() 关闭模态卡片即等价于放弃注册
    connect(cancelBtn, &QPushButton::clicked, this, &QWidget::close);
    connect(m_pass2, &QLineEdit::returnPressed, m_doneBtn, &QPushButton::click);

    // ---- 完成: 本地校验 → 服务端注册 ----
    connect(m_doneBtn, &QPushButton::clicked, this, [this] {
        if (!m_im || !m_doneBtn->isEnabled())
            return;

        const QString user = m_user->text().trimmed();
        const QString pass = m_pass->text();
        const QString pass2 = m_pass2->text();
        clearInvalid(m_user);
        clearInvalid(m_pass);
        clearInvalid(m_pass2);
        showInlineError(this, QString());

        // 校验(全部本地完成, 不合法不发请求)
        if (user.isEmpty()) {
            markInvalid(m_user);
            showInlineError(this, QStringLiteral("用户名不能为空"));
            m_user->setFocus();
            return;
        }
        if (user.toUtf8().size() > kMaxUserBytes) {
            markInvalid(m_user);
            showInlineError(this, QStringLiteral("用户名长度不能超过 %1 个字符").arg(kMaxUserBytes));
            m_user->setFocus();
            return;
        }
        const int passBytes = int(pass.toUtf8().size());
        if (passBytes < kMinPassBytes || passBytes > kMaxPassBytes) {
            markInvalid(m_pass);
            showInlineError(this, QStringLiteral("密码长度需为 %1~%2 字节(当前 %3 字节)")
                                    .arg(kMinPassBytes).arg(kMaxPassBytes).arg(passBytes));
            m_pass->setFocus();
            return;
        }
        if (pass != pass2) {                    // 两次不一致: 确认框红框提示
            markInvalid(m_pass2);
            showInlineError(this, QStringLiteral("两次输入的密码不一致"));
            m_pass2->setFocus();
            return;
        }

        m_doneBtn->setEnabled(false);           // 提交期间禁用
        m_doneBtn->setText(QStringLiteral("提交中..."));

        QPointer<RegisterDialog> self(this);    // 回调可能晚于窗口析构
        m_im->account()->registerAccount(user, pass,
                                         [self](bool ok, const QString &error, quint64 userId) {
            if (!self)
                return;
            self->m_doneBtn->setEnabled(true);
            self->m_doneBtn->setText(QStringLiteral("完成"));

            if (!ok) {                          // 失败: 展示服务端 message(如用户名已存在)
                showInlineError(self, error.isEmpty() ? QStringLiteral("注册失败") : error);
                return;
            }

            QMessageBox::information(self, QStringLiteral("注册成功"),
                                     QStringLiteral("注册成功，用户ID: %1").arg(userId));
            self->close();                      // accept(): 回登录页(账号已可登录)
        });
    });
}

// ---------------------------------------------------------------------------
/// 背景: 与登录页同款的浅粉蓝纵向渐变 + 8px 圆角卡片
void RegisterDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setRenderHint(QPainter::Antialiasing);

    QLinearGradient grad(0, 0, 0, height());
    grad.setColorAt(0.0, Theme::loginGradTop());
    grad.setColorAt(1.0, Theme::loginGradBottom());

    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                        Theme::radius(), Theme::radius());
    p.fillPath(card, grad);
    p.setPen(QColor(0, 0, 0, 26));
    p.drawPath(card);
}
