# 服务器可执行文件。
# 源文件说明：
#   main.cpp            -> 程序入口，创建 io_context 并启动事件循环
#   server.cpp          -> 监听端口、accept 新连接
#   session.cpp         -> 单条连接：收包/拆包/解析/业务/发包
#   session_manager.cpp -> 在线用户表 userId -> session
add_executable(tcp_echo_server main.cpp server.cpp session.cpp session_manager.cpp)

# 把当前目录加入 include 路径，便于 #include "frame.h" 这类同目录头文件。
target_include_directories(tcp_echo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 链接线程库。Boost.Asio 在多数平台上依赖线程相关符号。
target_link_libraries(tcp_echo_server PRIVATE Threads::Threads)
