# ----------------------------------------------------------------------------
# seed_db：用 MariaDB Connector 执行 db/schema.sql 和测试数据。
# 这样即使验证机上没有 mariadb/mysql 命令行客户端，也能完成验收。
# ----------------------------------------------------------------------------
add_executable(seed_db
    seed_db.cpp
    ${CMAKE_SOURCE_DIR}/step1_tcp_echo/db_config.cpp
)

target_include_directories(seed_db PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/step1_tcp_echo
    ${MariaDB_include}
)

target_link_directories(seed_db PRIVATE ${MariaDB_lib})
target_link_libraries(seed_db PRIVATE mariadbcpp)

# Windows：把 Connector DLL / 认证插件复制到 exe 同目录。
if(WIN32 AND MariaDB_lib)
    if(EXISTS "${MariaDB_lib}/mariadbcpp.dll")
        add_custom_command(TARGET seed_db POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MariaDB_lib}/mariadbcpp.dll"
                "$<TARGET_FILE_DIR:seed_db>")
    endif()
    if(EXISTS "${MariaDB_lib}/plugin")
        add_custom_command(TARGET seed_db POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MariaDB_lib}/plugin"
                "$<TARGET_FILE_DIR:seed_db>/plugin")
    endif()
elseif(MariaDB_lib)
    set_target_properties(seed_db PROPERTIES
        BUILD_RPATH "${MariaDB_lib}"
        INSTALL_RPATH "${MariaDB_lib}")
endif()

# 这里不产出可执行文件，只注册一个自定义目标，方便用户知道怎么跑验证脚本。
add_custom_target(verify_tcp
    COMMAND ${CMAKE_COMMAND} -E echo "Run: bash ${CMAKE_CURRENT_SOURCE_DIR}/verify_tcp.sh"
    COMMENT "Usage: ctest or manually run verify_tcp.sh"
)
