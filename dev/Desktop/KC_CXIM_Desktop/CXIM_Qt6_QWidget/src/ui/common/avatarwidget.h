#ifndef CXIM_AVATARWIDGET_H
#define CXIM_AVATARWIDGET_H

#include <QWidget>
#include <QPixmap>

/**
 * @brief 圆形头像控件
 *
 * 有图(URL 已下载或本地路径)则圆形裁切显示; 无图时用名字首字符 + 按 userId
 * 哈希取色的纯色圆底, 保证任何账号都有稳定可辨识的头像(无需外部资源文件)。
 */
class AvatarWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AvatarWidget(QWidget *parent = nullptr);

    void setAvatar(int diameter, const QString &nameOrId, quint64 seed,
                   const QPixmap &pix = QPixmap());

protected:
    void paintEvent(QPaintEvent *) override;

private:
    int m_diameter = 36;
    QString m_text;
    quint64 m_seed = 0;
    QPixmap m_pix;
};

#endif // CXIM_AVATARWIDGET_H
