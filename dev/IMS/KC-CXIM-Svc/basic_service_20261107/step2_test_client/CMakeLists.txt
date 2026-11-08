# 测试客户端可执行文件（同步阻塞式 socket，用于功能验证）。
add_executable(test_client test_client.cpp)
target_include_directories(test_client PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)

add_executable(test_client_another test_client_another.cpp)

# 用到 std::thread / sleep_for，需要线程库。
target_link_libraries(test_client PRIVATE Threads::Threads)

target_link_libraries(test_client_another PRIVATE Threads::Threads)

# ----------------------------------------------------------------------------
# 交互式聊天客户端 A / B：
#   - 用 scanf 从命令行读输入，回车即发送；
#   - 复用了 step1_tcp_echo 里的 message.h / message_codec.h，
#     所以要把该目录加入 include 路径。
# ----------------------------------------------------------------------------
add_executable(test_client_A test_client_A.cpp)
target_include_directories(test_client_A PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)
target_link_libraries(test_client_A PRIVATE Threads::Threads)

add_executable(test_client_B test_client_B.cpp)
target_include_directories(test_client_B PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)
target_link_libraries(test_client_B PRIVATE Threads::Threads)
