cmake_minimum_required(VERSION 3.16)

project(CXIM_Qt6_QWidget VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# .qrc 资源清单自动交给 rcc 编译进二进制(资源放在 resources/, 清单见 resources/resources.qrc)
set(CMAKE_AUTORCC ON)

# 仅使用 Qt 原生模块: Widgets(UI) + Network(HTTP/TCP/直传) + Svg(解码 :/icons 下的 SVG 图标)
#   Svg 不可省: 左侧导航按钮的双态图标是 SVG, 没有它 QIcon 无法解码(表现为按钮无图标);
#   链接它还有一个作用 —— windeployqt 才会把 svgicon 插件与 Qt6Svg.dll 一起打进发布包。
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Svg)

# 分层源码: core(数据/会话) → net(协议) → service(业务) → ui(界面)
# 非 UI 三层单独成集, 供 GUI 与无头冒烟测试共用(证明功能层与 UI 完全解耦)
file(GLOB_RECURSE IM_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/*.cpp)
file(GLOB_RECURSE IM_UI_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp)
# 冒烟测试只参与非 UI 头文件(避免 AUTOMOC 把 UI 头拖进无 Widgets 的目标)
file(GLOB_RECURSE IM_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/service/*.h)

qt_add_executable(CXIM_Qt6_QWidget
    main.cpp
    ${IM_CORE_SOURCES}
    ${IM_UI_SOURCES}
    ${IM_HEADERS}
    # 图片/图标等资源清单(rcc 编译进 exe, 运行时以 ":/icons/..." 访问)。
    # 只挂到 GUI 目标: cxim_smoke / cxim_responder 是无头工具, 不含 UI 也不需要资源。
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.qrc
)
# Qt6::Svg: 运行时解码 :/icons/*.svg(左侧导航双态图标), 并让 windeployqt 带上 svgicon 插件
target_link_libraries(CXIM_Qt6_QWidget PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
if(WIN32)
    # user32: 无边框窗口 nativeEvent 的边缘缩放命中测试
    # dwmapi: DwmSetWindowAttribute → Win11 整窗圆角(8px)与系统投影
    target_link_libraries(CXIM_Qt6_QWidget PRIVATE user32 dwmapi)
endif()
set_target_properties(CXIM_Qt6_QWidget PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

# ---------------------------------------------------------------------------
# 无头协议冒烟测试: 登录 → TCP 登录 → 好友列表 → 发单聊文本 → SendAck → 拉历史
#   运行: cxim_smoke [username password [friendId]]
#   配置: 与 GUI 共用 exe 同级的 CXIM_Qt6_config.ini
# ---------------------------------------------------------------------------
qt_add_executable(cxim_smoke
    tools/smoke/main.cpp
    ${IM_CORE_SOURCES}
    ${IM_HEADERS}
)
target_link_libraries(cxim_smoke PRIVATE Qt6::Network)
set_target_properties(cxim_smoke PROPERTIES WIN32_EXECUTABLE FALSE)

# 对端应答器(供 GUI 自动化互测使用): 需要 Gui 生成回传 PNG
qt_add_executable(cxim_responder
    tools/responder/main.cpp
    ${IM_CORE_SOURCES}
    ${IM_HEADERS}
)
target_link_libraries(cxim_responder PRIVATE Qt6::Network Qt6::Gui)
set_target_properties(cxim_responder PROPERTIES WIN32_EXECUTABLE FALSE)

# 服务端地址配置随 exe 发布(AppConfig 从 applicationDirPath 读取)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/CXIM_Qt6_config.ini
               ${CMAKE_CURRENT_BINARY_DIR}/CXIM_Qt6_config.ini COPYONLY)

install(TARGETS CXIM_Qt6_QWidget cxim_smoke RUNTIME DESTINATION bin)
