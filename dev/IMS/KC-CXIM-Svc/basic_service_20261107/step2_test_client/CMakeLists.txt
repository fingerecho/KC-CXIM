# 测试客户端可执行文件（同步阻塞式 socket，用于功能验证）。
add_executable(test_client test_client.cpp)
target_include_directories(test_client PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)

add_executable(test_client_another test_client_another.cpp)

# ----------------------------------------------------------------------------
# 好友模块验收客户端（BE-FRIEND-08）：
#   同时使用 HTTP（好友业务）与 TCP（实时事件），覆盖搜索、申请、通过/拒绝、
#   好友列表、备注、删除、拉黑等完整链路。
# ----------------------------------------------------------------------------
add_executable(test_friend_client test_friend_client.cpp)
target_include_directories(test_friend_client PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)

# ----------------------------------------------------------------------------
# 好友全链路并发压测客户端：
#   每个线程循环执行 注册 -> TCP 登录 -> 发申请 -> 通过 -> 发消息
#   -> 查列表 -> 删除，最后统计成功次数与耗时。
# ----------------------------------------------------------------------------
add_executable(test_stress_01 test_stress_01.cpp)
target_include_directories(test_stress_01 PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)

# ----------------------------------------------------------------------------
# 好友全链路并发压测客户端：
#   iter/s 反映完整链路吞吐，tcp/http 分位数分别反映两条链路的延迟分布。
# ----------------------------------------------------------------------------
add_executable(test_stress_02 test_stress_02.cpp)
target_include_directories(test_stress_02 PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)

# 用到 std::thread / sleep_for，需要线程库。
target_link_libraries(test_client PRIVATE Threads::Threads)

target_link_libraries(test_client_another PRIVATE Threads::Threads)
target_link_libraries(test_friend_client PRIVATE Threads::Threads)
target_link_libraries(test_stress_01 PRIVATE Threads::Threads)
target_link_libraries(test_stress_02 PRIVATE Threads::Threads)

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
