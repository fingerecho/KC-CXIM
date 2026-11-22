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
    //   listPanel        会话/好友/群列表(setItemWidget 自定义行 → ::item 不能有 padding)
    //   listPanelText    纯文本+checkbox 的列表(建群选人/邀请成员 → 留白写在 ::item)
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
        // 左侧导航按钮: 现在是纯图标按钮(resources/icons 下的双态 SVG),
        // 所以不再需要 font-size/color(emoji 时代的遗留); padding 8 + iconSize 24
        // = 40x40 的方形热区, 圆角 10px 让它看起来是个"胶囊块"而不是整列高亮。
        // 选中效果有两层: 图标自动切到 QIcon::On(蓝色版) + 下面这层浅蓝底。
        "#railButton { background: transparent; border: none;"
        " padding: 8px; border-radius: 10px; }"
        "#railButton:hover { background: %6; }"
        "#railButton:checked { background: %10; }"

        "QLineEdit#cardInput { background: white; border: none; border-radius: %8px;"
        " padding: 12px; font-size: 15px; color: %1; }"
        "QLineEdit#cardInput:focus { border: 1px solid %7; }"

        "QPushButton#linkButton { background: transparent; border: none;"
        " color: %7; font-size: 15px; }"
        "QPushButton#linkButton:hover { text-decoration: underline; }"

        // 圆形勾选框(登录页"自动登录/记住密码/已阅读并同意"三处共用)
        //   旧版: 18px 圆 + 2px 粗灰边, 选中态几乎整圆实心蓝 → 相对 12px 文字过重。
        //   现版: 12px 圆, 选中态为实心品牌蓝(不画中心白点), 并补齐 hover/disabled 反馈。
        //   字号/间距也统一收在这里(原先散落在 loginwindow.cpp 的控件级 setStyleSheet)。
        "QCheckBox#circleCheck { color: %5; font-size: 12px; spacing: 5px;"
        " background: transparent; }"
        "QCheckBox#circleCheck:hover { color: %1; }"
        "QCheckBox#circleCheck:disabled { color: #bfc4cb; }"
        // indicator 一律 border:0 + padding:0: Qt 样式表在画"带 border 的背景渐变"时,
        // 渐变所用的矩形与 border 参与计算的盒子不一致, 圆心会被推移约 1px(即之前
        // "白圈不居中"的根因)。所以现在不用 border, 未选中态那圈灰环也改由同心渐变画出。
        // 12px 圆配 6px 半径: 环的厚度用 stop 比例(0.85~1.0)控制, 不依赖像素边框。
        "QCheckBox#circleCheck::indicator { width: 12px; height: 12px;"
        " border: 0px; padding: 0px; border-radius: 6px;"
        " background: qradialgradient(cx:0.5, cy:0.5, radius:0.5,"
        " stop:0 white, stop:0.85 white, stop:0.87 #c9ced6, stop:1 #c9ced6); }"
        // hover: 只把外环换成品牌蓝, 内圈仍是白
        "QCheckBox#circleCheck::indicator:hover {"
        " background: qradialgradient(cx:0.5, cy:0.5, radius:0.5,"
        " stop:0 white, stop:0.85 white, stop:0.87 %7, stop:1 %7); }"
        // checked: 实心品牌蓝圆。中心白点按要求去掉 —— 少一层同心渐变,
        // 也就彻底不存在"白圈偏心"的可能, 选中/未选中的差别只在填色。
        "QCheckBox#circleCheck::indicator:checked { background: %7; }"
        "QCheckBox#circleCheck::indicator:checked:hover { background: #1878c8; }"
        "QCheckBox#circleCheck::indicator:disabled {"
        " background: qradialgradient(cx:0.5, cy:0.5, radius:0.5,"
        " stop:0 #f5f6f8, stop:0.85 #f5f6f8, stop:0.87 #e4e7eb, stop:1 #e4e7eb); }"

        "#tabBar { background: #eef0f2; border-radius: %8px; padding: 2px; }"
        "#tabButton { background: transparent; border: none; border-radius: 6px;"
        " padding: 7px 0; color: %5; font-size: 15px; }"
        "#tabButton:checked { background: white; color: %7; font-weight: bold; }"

        "QListWidget#listPanel { background: %9; border: none; outline: 0; }"
        // ⚠ ::item 上**绝对不能写 padding**。这三个列表(会话/好友/群成员)都是用
        //    QListWidget::setItemWidget() 装的自定义行, Qt 会把 item 的 padding 从
        //    setSizeHint() 的高度里扣掉再摆放行 widget:
        //      会话行需 40(头像)+6+6=52, 给了 62-20=42 → 头像上下各裁 5px、文字截断
        //      好友行需 40+8+8=56, 给了 64-20=44 → 同样被裁
        //    行内边距一律由各视图行布局的 setContentsMargins 负责(那里不会被扣),
        //    要改呼吸空间请去改那边的 margins 或 sizeHint, 不要回到这里加 padding。
        "QListWidget#listPanel::item { border: none; border-radius: 6px; }"
        "QListWidget#listPanel::item:hover { background: %6; }"
        "QListWidget#listPanel::item:selected { background: %10; }"

        // listPanelText: 只装"纯文本 + checkbox"的列表(建群选人/邀请成员)。
        // 这类项由 style 自己绘制文字, padding 只挪动文字位置、不会裁内容,
        // 所以留白仍然写在这里; 自定义行(setItemWidget)的列表必须用 listPanel。
        "QListWidget#listPanelText { background: %9; border: none; outline: 0; }"
        "QListWidget#listPanelText::item { padding: 8px 12px; border: none;"
        " border-radius: 6px; }"
        "QListWidget#listPanelText::item:hover { background: %6; }"
        "QListWidget#listPanelText::item:selected { background: %10; }"

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
