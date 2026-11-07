# 测试客户端可执行文件（同步阻塞式 socket，用于功能验证）。
add_executable(test_client test_client.cpp)

add_executable(test_client_another test_client_another.cpp)

# 用到 std::thread / sleep_for，需要线程库。
target_link_libraries(test_client PRIVATE Threads::Threads)

target_link_libraries(test_client_another PRIVATE Threads::Threads)
