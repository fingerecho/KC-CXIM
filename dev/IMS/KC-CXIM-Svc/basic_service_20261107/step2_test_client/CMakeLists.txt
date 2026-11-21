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

# ----------------------------------------------------------------------------
# 好友全生命周期分阶段压测：
#   注册 -> TCP 登录 -> 对 N 个好友发申请 -> N 个好友通过
#   -> 互相发消息 -> 互相查列表 -> 互相删好友。
# ----------------------------------------------------------------------------
add_executable(test_stress_03 test_stress_03.cpp)
target_include_directories(test_stress_03 PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)

# 用到 std::thread / sleep_for，需要线程库。
target_link_libraries(test_client PRIVATE Threads::Threads)

target_link_libraries(test_client_another PRIVATE Threads::Threads)
target_link_libraries(test_friend_client PRIVATE Threads::Threads)
target_link_libraries(test_stress_01 PRIVATE Threads::Threads)
target_link_libraries(test_stress_02 PRIVATE Threads::Threads)
target_link_libraries(test_stress_03 PRIVATE Threads::Threads)

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

# ----------------------------------------------------------------------------
# 发消息关系校验（BE-SEND-01）验收客户端：
#   陌生人 / 被对方拉黑 / 自己拉黑对方 三种场景下 TCP 发消息应被 Error 拒绝，
#   好友场景则正常 SendAck + 实时投递。
#   复用 step1_tcp_echo 的 message.h / message_codec.h，需要加入 include 路径。
# ----------------------------------------------------------------------------
add_executable(test_send_msg_to_blockFriendAndBlackFriend test_send_msg_to_blockFriendAndBlackFriend.cpp)
target_include_directories(test_send_msg_to_blockFriendAndBlackFriend PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)
target_link_libraries(test_send_msg_to_blockFriendAndBlackFriend PRIVATE Threads::Threads)

# ----------------------------------------------------------------------------
# /*************群聊功能：群聊模块验收客户端（BE-GROUP-01~08）***************/
#   同时使用 HTTP（建群/邀请/权限/资料）与 TCP（群消息实时投递/离线补推/群事件），
#   复用 step1_tcp_echo 的 message.h / message_codec.h。
# ----------------------------------------------------------------------------
add_executable(test_group_client test_group_client.cpp)
target_include_directories(test_group_client PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)
target_link_libraries(test_group_client PRIVATE Threads::Threads)

# ----------------------------------------------------------------------------
# /*************图片文件功能（七牛云直传）：验收客户端 + 签名单测***************/
#   test_file_client  ：mock 模式全链路验收（凭证→直传→commit→单聊/群聊→
#                       302 下载→离线补推→负向用例→删除），需要 qiniu_signer/
#                       file_types 的实现（服务端同源，避免"测试自己编一份"）。
#   test_qiniu_signer ：三类签名的独立单测（RFC 官方向量 + Python 参考实现黄金样例，
#                       纯本地计算，无网络/无数据库/无服务器依赖）。
# ----------------------------------------------------------------------------
add_executable(test_file_client
    test_file_client.cpp
)
target_include_directories(test_file_client PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)
target_link_libraries(test_file_client PRIVATE Threads::Threads)

# 文件票据/存储链路 loopback 压测（排障工具）：进程内高速重复
# upload-token尾段→verifyTicket→mockPut/stat→下载签名→入口签名→元数据往返，
# Debug 版开 _CRTDBG_CHECK_ALWAYS 抓堆损坏（服务器随机静默死亡的取证手段）。
add_executable(test_file_loopback
    test_file_loopback.cpp
    ${CMAKE_SOURCE_DIR}/step1_tcp_echo/qiniu_signer.cpp
    ${CMAKE_SOURCE_DIR}/step1_tcp_echo/file_types.cpp
    ${CMAKE_SOURCE_DIR}/step1_tcp_echo/object_store.cpp
)
target_include_directories(test_file_loopback PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)
target_link_libraries(test_file_loopback PRIVATE Threads::Threads OpenSSL::Crypto OpenSSL::SSL)

add_executable(test_qiniu_signer
    test_qiniu_signer.cpp
    ${CMAKE_SOURCE_DIR}/step1_tcp_echo/qiniu_signer.cpp
)
target_include_directories(test_qiniu_signer PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)
# 注意：OpenSSL::Crypto 是 step1_tcp_echo 里 find_package 出来的**局部**
# imported target，兄弟目录不可见——这里必须自己 find_package 一次
# （CMake 会直接复用已找到的结果，不会重复探测）。
find_package(OpenSSL REQUIRED)
target_link_libraries(test_qiniu_signer PRIVATE Threads::Threads OpenSSL::Crypto)
# /*************图片文件功能（七牛云直传）：验收客户端 + 签名单测***************/

# ----------------------------------------------------------------------------
# /*************图片文件功能（七牛云**真实**直传）：真机验收客户端***************/
#   test_Qiniu_file_client：QINIU_ENABLED=1 下的真机链路——凭证签发后向
#   uploadUrl(七牛 up_host) 做真实 multipart/form-data 表单直传（数据面经
#   Boost.Beast + OpenSSL TLS 出网，不落 IM 服务器），再 commit（服务器向
#   rs.qbox.me stat 核验）、单聊/群聊 type=2/4、302 跟随真实私有签名直链并逐
#   字节核对下载完整性、删除。需真实七牛账号，配合 verify_Qiniu_file.sh 使用。
#   用到 beast::ssl_stream / asio::ssl，必须链 OpenSSL::SSL + OpenSSL::Crypto。
# ----------------------------------------------------------------------------
add_executable(test_Qiniu_file_client test_Qiniu_file_client.cpp)
target_include_directories(test_Qiniu_file_client PRIVATE ${CMAKE_SOURCE_DIR}/step1_tcp_echo)
target_link_libraries(test_Qiniu_file_client PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
# /*************图片文件功能（七牛云真实直传）：真机验收客户端***************/
