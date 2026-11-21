#include "theme.h"

#include <QLinearGradient>

namespace Theme {

QString primaryButtonQss()
{
    return QStringLiteral(
        "QPushButton { color: white; font-size: 17px; font-weight: bold;"
        " border: none; border-radius: %1px;"
        " background: qlineargradient(x1:0, y1:0, x2:1, y2:0,"
        " stop:0 %2, stop:1 %3); }"
        "QPushButton:pressed { background: %4; }"
        "QPushButton:disabled { color: #e0e0e0; background: #b9dcf5; }")
        .arg(radius())
        .arg(brandTop().name(), brandBottom().name(), brandBottom().darker(115).name());
}

QString ghostButtonQss()
{
    return QStringLiteral(
        "QPushButton { color: %1; background: #eceef1; border: none;"
        " border-radius: %2px; font-size: 15px; padding: 8px 24px; }"
        "QPushButton:pressed { background: #dfe2e6; }")
        .arg(textPrimary().name()).arg(radius());
}

QString appStyleSheet()
{
    // objectName 约定:
    //   headerBar        主窗口顶部蓝色标题栏
    //   railButton       左侧导航图标按钮(可 checkable, 选中高亮)
    //   cardInput        白底圆角输入框(账号/密码/搜索)
    //   linkButton       文字链接按钮(注册账号/协议)
    //   circleCheck      圆形勾选框(自动登录/记住密码/同意协议)
    //   tabButton        好友|群聊 切换标签
    //   listPanel        会话/好友/群列表
    //   announcementBar  群公告条
    //   primaryButton /  ghostButton 由各自 setStyleSheet 用上面的片段
    return QStringLiteral(
        "QWidget { font-family: 'Microsoft YaHei UI', 'Segoe UI', sans-serif;"
        " font-size: 14px; color: %1; }"

        "#headerBar { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        " stop:0 %2, stop:1 %3); }"
        "#headerBar QLabel { color: white; font-size: 16px; }"
        "#headerBar QPushButton { background: transparent; border: none; color: white;"
        " font-size: 14px; padding: 4px 10px; }"
        "#headerBar QPushButton:hover { background: rgba(255,255,255,0.18); }"
        "#headerBar QPushButton[winClose=\"true\"]:hover { background: #e81123; }"

        "#railPanel { background: #fbfbfc; border-right: 1px solid %4; }"
        "#railButton { background: transparent; border: none; font-size: 22px;"
        " color: %5; padding: 10px; border-radius: 6px; }"
        "#railButton:hover { background: %6; }"
        "#railButton:checked { color: %7; background: %6; }"

        "QLineEdit#cardInput { background: white; border: none; border-radius: %8px;"
        " padding: 12px; font-size: 15px; color: %1; }"
        "QLineEdit#cardInput:focus { border: 1px solid %7; }"

        "QPushButton#linkButton { background: transparent; border: none;"
        " color: %7; font-size: 15px; }"
        "QPushButton#linkButton:hover { text-decoration: underline; }"

        "QCheckBox#circleCheck::indicator { width: 18px; height: 18px;"
        " border-radius: 9px; border: 2px solid #b7bcc3; background: white; }"
        "QCheckBox#circleCheck::indicator:checked { border: 2px solid %7;"
        " background: qradialgradient(cx:0.5, cy:0.5, radius:0.45,"
        " stop:0 %7, stop:0.99 %7, stop:1 white); }"
        "QCheckBox#circleCheck { color: %5; }"

        "#tabBar { background: #eef0f2; border-radius: %8px; padding: 2px; }"
        "#tabButton { background: transparent; border: none; border-radius: 6px;"
        " padding: 7px 0; color: %5; font-size: 15px; }"
        "#tabButton:checked { background: white; color: %7; font-weight: bold; }"

        "QListWidget#listPanel { background: %9; border: none; outline: 0; }"
        "QListWidget#listPanel::item { padding: 10px 12px; border-radius: 6px; }"
        "QListWidget#listPanel::item:hover { background: %6; }"
        "QListWidget#listPanel::item:selected { background: %10; }"

        "#searchInput { background: #eef0f2; border: none; border-radius: 6px;"
        " padding: 8px 12px; color: %1; }"
        "#plusButton { background: #eef0f2; border: none; border-radius: 6px;"
        " font-size: 18px; color: %5; }"
        "#plusButton:hover { background: #e2e5e9; }"

        "#announcementBar { background: #f2f4f7; border-radius: 6px; padding: 6px 10px;"
        " color: %5; }"

        "#chatHeader { background: white; border-bottom: 1px solid %4; }"
        "#chatHeader QLabel#chatTitle { font-size: 17px; font-weight: bold; color: %1; }"
        "#chatHeader QPushButton { background: transparent; border: none;"
        " font-size: 16px; color: %5; padding: 4px 8px; border-radius: 4px; }"
        "#chatHeader QPushButton:hover { background: %6; }"

        "#messageArea { background: white; border: none; }"
        "#inputToolbar QToolButton { background: transparent; border: none;"
        " font-size: 18px; color: %5; padding: 4px; }"
        "#inputToolbar QToolButton:hover { color: %7; }"
        "QTextEdit#inputEdit { border: none; background: white; font-size: 14px; }"

        "#statusBadge { color: %5; font-size: 12px; }"
    )
    .arg(textPrimary().name(),
         brandTop().lighter(105).name(),
         brandBottom().name(),
         divider().name(),
         textSecondary().name(),
         itemHover().name(),
         brandAccent().name(),
         QString::number(radius()),
         windowBg().name(),
         itemSelected().name());
}

} // namespace Theme
