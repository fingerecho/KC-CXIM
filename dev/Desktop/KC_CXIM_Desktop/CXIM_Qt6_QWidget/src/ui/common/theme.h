#ifndef CXIM_THEME_H
#define CXIM_THEME_H

#include <QString>
#include <QColor>

/**
 * @file theme.h
 * @brief 全局视觉规范(对照 docs/desktop_real_*_ui_0.png 三张真实 UI 截图)
 *
 * 所有颜色/圆角/字号只在本文件定义一次, 控件通过 objectName + 全局 QSS 应用,
 * 换肤时只需替换本实现(将来可扩展为读取 theme.ini)。
 */
namespace Theme {

// 主色(标题栏/主按钮), 截图中为明亮的天蓝渐变
inline QColor brandTop()     { return QColor(0x5a, 0xb3, 0xf0); }
inline QColor brandBottom()  { return QColor(0x3f, 0x95, 0xdd); }
inline QColor brandAccent()  { return QColor(0x1e, 0x8a, 0xe0); }   // 链接/选中文字

// 登录/注册卡片背景: 上粉下蓝的浅色渐变
inline QColor loginGradTop() { return QColor(0xfd, 0xef, 0xf3); }
inline QColor loginGradBottom() { return QColor(0xe6, 0xf0, 0xfe); }

inline QColor windowBg()     { return QColor(0xf7, 0xf8, 0xfa); }
inline QColor panelBg()      { return QColor(0xff, 0xff, 0xff); }
inline QColor textPrimary()  { return QColor(0x33, 0x37, 0x3d); }
inline QColor textSecondary(){ return QColor(0x8a, 0x90, 0x99); }
inline QColor divider()      { return QColor(0xe4, 0xe7, 0xeb); }

// 会话气泡
inline QColor bubbleIn()     { return QColor(0xf0, 0xf2, 0xf5); }
inline QColor bubbleOut()    { return QColor(0xd6, 0xe9, 0xfb); }

// 列表交互
inline QColor itemHover()    { return QColor(0xee, 0xf4, 0xfa); }
inline QColor itemSelected() { return QColor(0xcf, 0xe6, 0xfb); }

inline int radius()          { return 8; }   // 全局圆角 8px(与截图口径一致)

/// 全局样式表: 按 objectName 定制, 未命中的控件走 Qt Fusion 默认
QString appStyleSheet();

/// 主按钮(登录/完成/发送)用的渐变样式片段
QString primaryButtonQss();
/// 浅色次按钮(取消)
QString ghostButtonQss();

} // namespace Theme

#endif // CXIM_THEME_H
