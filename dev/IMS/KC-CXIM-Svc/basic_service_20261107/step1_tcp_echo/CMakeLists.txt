# 服务器可执行文件。
# 源文件说明：
#   main.cpp            -> 程序入口：读 DBConfig.ini、建连接池、启动事件循环
#   server.cpp          -> 监听端口、accept 新连接
#   session.cpp         -> 单条连接：收包/拆包/解析/业务/发包
#   session_manager.cpp -> 在线用户表 userId -> session
#   db_config.cpp       -> 解析 DBConfig.ini
#   db.cpp              -> 连接池 + DB 工作线程池 + DAO
add_executable(cxim-server
    main.cpp
    server.cpp
    session.cpp
    session_manager.cpp
    db_config.cpp
    db.cpp
    jwt_config.cpp
    token_service.cpp
    http_server.cpp
    #/*************图片文件功能（七牛云直传）：新增源文件***************/
    file_types.cpp      # 文件领域模型：uuid/元数据 JSON/白名单/FileRuntime
    qiniu_signer.cpp    # 七牛三类签名的纯本地计算（OpenSSL HMAC/Base64）
    object_store.cpp    # ObjectStorage 抽象 + MockObjectStore + QiniuObjectStore(含管理API客户端)
    #/*************图片文件功能（七牛云直传）：新增源文件***************/
)

# 把当前目录加入 include 路径，便于 #include "frame.h" 这类同目录头文件。
target_include_directories(cxim-server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# /*************core-dump 修复 ⑧：Release 也要留符号与帧指针***************/
# 只加 -g / -fno-omit-frame-pointer，**不动优化级别**（CMAKE_BUILD_TYPE 仍是
# Release，-O3 照旧，压测数据不受影响，见 readme §11.6 第 1 条）。
# 为什么必须加：core 与 backtrace 里只有地址时，等于没有现场。
#   * -g                        → gdb bt / addr2line 能出函数名与行号
#   * -fno-omit-frame-pointer   → 即使 -O3，栈也能一路走回去
# 体积代价：可执行文件变大几 MB，运行期内存几乎不变（调试段不进 RSS）。
# 需要纯 Release 时：-DCXIM_NO_DEBUG_INFO=ON。
if(NOT MSVC AND NOT CXIM_NO_DEBUG_INFO)
    target_compile_options(cxim-server PRIVATE -g -fno-omit-frame-pointer)
endif()

# 加入 MariaDB C++ Connector 头文件目录（由 build_step.sh 通过 -DMariaDB_include 传入）。
target_include_directories(cxim-server PRIVATE ${MariaDB_include})

# 链接线程库。Boost.Asio 在多数平台上依赖线程相关符号。
target_link_libraries(cxim-server PRIVATE Threads::Threads)

# 链接 MariaDB 数据库（库目录由 build_step.sh 通过 -DMariaDB_lib 传入）。
target_link_directories(cxim-server PRIVATE ${MariaDB_lib})
target_link_libraries(cxim-server PRIVATE mariadbcpp)

#链接libbcrypt库
target_link_libraries(cxim-server PRIVATE bcrypt)

# 图片文件功能调试期补强：Windows 崩溃处理器用 dbghelp 做栈符号化（main.cpp），
# 只在 MSVC 下需要；Linux 走 execinfo（无需额外库）。
if(MSVC)
    target_link_libraries(cxim-server PRIVATE dbghelp)
endif()

#找到并链接OpenSSL库
# 图片文件功能（七牛云直传）：OpenSSL::Crypto 供 HMAC/Base64/uuid 随机源；
# OpenSSL::SSL 供管理 API（rs.qbox.me）的 HTTPS 调用（object_store.cpp）。
find_package(OpenSSL REQUIRED)
target_link_libraries(cxim-server PRIVATE
    OpenSSL::Crypto
    OpenSSL::SSL
)

# ---------------------------------------------------------------------------
# Windows：把 Connector 的 DLL 和认证插件复制到 exe 同目录，避免运行时找不到。
# Linux：设置 rpath，运行时才能找到 libmariadbcpp.so。
# ---------------------------------------------------------------------------
if(WIN32 AND MariaDB_lib)
    if(EXISTS "${MariaDB_lib}/mariadbcpp.dll")
        add_custom_command(TARGET cxim-server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MariaDB_lib}/mariadbcpp.dll"
                "$<TARGET_FILE_DIR:cxim-server>")
    endif()
    if(EXISTS "${MariaDB_lib}/plugin")
        add_custom_command(TARGET cxim-server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MariaDB_lib}/plugin"
                "$<TARGET_FILE_DIR:cxim-server>/plugin")
    endif()
elseif(MariaDB_lib)
    set_target_properties(cxim-server PROPERTIES
        BUILD_RPATH "${MariaDB_lib}"
        INSTALL_RPATH "${MariaDB_lib}")
endif()
