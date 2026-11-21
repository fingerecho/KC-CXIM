cmake_minimum_required(VERSION 3.16)

project(CXIM_Qt6_QWidget VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# 仅使用 Qt 原生模块: Widgets(UI) + Network(HTTP/TCP/直传)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

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
)
target_link_libraries(CXIM_Qt6_QWidget PRIVATE Qt6::Widgets Qt6::Network)
if(WIN32)
    target_link_libraries(CXIM_Qt6_QWidget PRIVATE user32)   # 无边框窗口 nativeEvent
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
