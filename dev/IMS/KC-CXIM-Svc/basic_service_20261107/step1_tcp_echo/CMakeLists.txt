# 服务器可执行文件。
# 源文件说明：
#   main.cpp            -> 程序入口：读 DBConfig.ini、建连接池、启动事件循环
#   server.cpp          -> 监听端口、accept 新连接
#   session.cpp         -> 单条连接：收包/拆包/解析/业务/发包
#   session_manager.cpp -> 在线用户表 userId -> session
#   db_config.cpp       -> 解析 DBConfig.ini
#   db.cpp              -> 连接池 + DB 工作线程池 + DAO
add_executable(tcp_echo_server
    main.cpp
    server.cpp
    session.cpp
    session_manager.cpp
    db_config.cpp
    db.cpp
    jwt_config.cpp
    token_service.cpp
    http_server.cpp
)

# 把当前目录加入 include 路径，便于 #include "frame.h" 这类同目录头文件。
target_include_directories(tcp_echo_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 加入 MariaDB C++ Connector 头文件目录（由 build_step.sh 通过 -DMariaDB_include 传入）。
target_include_directories(tcp_echo_server PRIVATE ${MariaDB_include})

# 链接线程库。Boost.Asio 在多数平台上依赖线程相关符号。
target_link_libraries(tcp_echo_server PRIVATE Threads::Threads)

# 链接 MariaDB 数据库（库目录由 build_step.sh 通过 -DMariaDB_lib 传入）。
target_link_directories(tcp_echo_server PRIVATE ${MariaDB_lib})
target_link_libraries(tcp_echo_server PRIVATE mariadbcpp)

#链接libbcrypt库
target_link_libraries(tcp_echo_server PRIVATE bcrypt)

#找到并链接OpenSSL库
find_package(OpenSSL REQUIRED)
target_link_libraries(tcp_echo_server PRIVATE
    OpenSSL::Crypto
)

# ---------------------------------------------------------------------------
# Windows：把 Connector 的 DLL 和认证插件复制到 exe 同目录，避免运行时找不到。
# Linux：设置 rpath，运行时才能找到 libmariadbcpp.so。
# ---------------------------------------------------------------------------
if(WIN32 AND MariaDB_lib)
    if(EXISTS "${MariaDB_lib}/mariadbcpp.dll")
        add_custom_command(TARGET tcp_echo_server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MariaDB_lib}/mariadbcpp.dll"
                "$<TARGET_FILE_DIR:tcp_echo_server>")
    endif()
    if(EXISTS "${MariaDB_lib}/plugin")
        add_custom_command(TARGET tcp_echo_server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MariaDB_lib}/plugin"
                "$<TARGET_FILE_DIR:tcp_echo_server>/plugin")
    endif()
elseif(MariaDB_lib)
    set_target_properties(tcp_echo_server PROPERTIES
        BUILD_RPATH "${MariaDB_lib}"
        INSTALL_RPATH "${MariaDB_lib}")
endif()
