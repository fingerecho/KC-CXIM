#include "avatarwidget.h"

#include <QPainter>
#include <QPainterPath>
#include <QFont>

namespace {
// 与 UI 主色协调的一组头像底色
const char *kPalette[] = {"#5aa3df", "#6cc4a1", "#e8a25c", "#b58ad6",
                          "#e07a8f", "#5cb8c4", "#8fa85c", "#d68ab0"};
}

AvatarWidget::AvatarWidget(QWidget *parent) : QWidget(parent)
{
    setFixedSize(36, 36);
}

void AvatarWidget::setAvatar(int diameter, const QString &nameOrId, quint64 seed,
                             const QPixmap &pix)
{
    m_diameter = diameter;
    m_seed = seed;
    m_pix = pix;
    // 首字符: 中文取第一个字, 英文取首字母大写
    m_text = nameOrId.isEmpty() ? QStringLiteral("?")
                                : nameOrId.left(1).toUpper();
    setFixedSize(diameter, diameter);
    update();
}

void AvatarWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r(0.5, 0.5, m_diameter - 1.0, m_diameter - 1.0);
    QPainterPath circle;
    circle.addEllipse(r);
    p.setClipPath(circle);

    if (!m_pix.isNull()) {
        p.drawPixmap(r.toRect(), m_pix.scaled(r.toRect().size(),
                        Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        return;
    }

    p.fillPath(circle, QColor(kPalette[m_seed % (sizeof(kPalette) / sizeof(kPalette[0]))]));
    p.setPen(Qt::white);
    QFont f = font();
    f.setPixelSize(int(m_diameter * 0.45));
    f.setBold(true);
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, m_text);
}
