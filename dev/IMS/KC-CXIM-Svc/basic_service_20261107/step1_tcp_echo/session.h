#pragma once
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "frame.h"
#include "message.h"

// ============================================================================
// session：一条客户端连接（一个 socket）的完整生命周期封装
// ----------------------------------------------------------------------------
// 它负责：
//   1) 按"4 字节长度头 + body"的协议不断读消息（解决粘包/半包）；
//   2) 把 body 解析成 Message（JSON）；
//   3) 处理业务：登录鉴权、发消息落库、离线补推、历史分页、回执；
//   4) 用写队列把回复/转发安全地写回 socket；
//   5) 空闲超时踢人：超过 IDLE_TIMEOUT 没收到任何消息就断开；
//   6) 连接断开时从 SessionManager 注销。
//
// 线程模型（数据库接入后）：
//   - io_context 可能由多个线程同时 run()；
//   - 每条连接用独立的 strand_ 串行化自己的读/写/DB 回调，避免多线程数据竞争；
//   - DB worker 线程：执行阻塞 SQL；
//   - DB 完成后通过 boost::asio::post 回到 io_context，再切回本连接的 strand。
//   - deliver() 会把消息 post 到目标连接的 strand 上。
// ============================================================================

namespace kc {

class SessionManager;   // 前向声明，避免头文件循环包含
class Db;               // 数据库访问层
class TokenService;     // JWT 签发/校验服务

class session : public std::enable_shared_from_this<session> {
public:
    static constexpr std::chrono::seconds IDLE_TIMEOUT{60};

    /*************P0-4：连接级过载指标***************/
    // 当前存活的 session 数（构造 +1、析构 -1，天然平衡）。
    // server 用它决定"是否再接一条新连接"。
    static long liveCount();

    // 单连接在途 DB 任务默认上限（TCP_MAX_OUTSTANDING 可覆盖）。
    static constexpr int kMaxOutstandingDefault = 64;
    /*************P0-4：连接级过载指标***************/

    // 构造时接管一条已建立的 TCP 连接，并绑定在线用户表、数据库层与 JWT 服务。
    session(boost::asio::ip::tcp::socket socket, SessionManager& manager,
            Db& db, TokenService& tokens);

    // 析构时回收存活连接计数（P0-4）。用析构而不是 close() 来减，
    // 是为了保证"任何路径都恰好减一次"。
    ~session();

    void start();

    // 供"其他 session"调用：把一条消息投递给本连接（消息路由的落点）。
    void deliver(const Message& msg);

    void close();

private:
    void do_read_header();
    void do_read_body(std::uint32_t body_len);
    void handle_message(const Message& msg);

    void do_login(const Message& msg);      // 登录：校验 JWT（不查库、不比对密码）
    void do_send(const Message& msg);       // 发消息：落库 + 在线投递
    /*************群聊功能：TCP 群消息与群历史入口***************/
    void do_send_group(const Message& msg); // 发群消息：写扩散落库 + 在线成员直推
    void do_history_group(const Message& msg); // 群历史分页（校验群成员身份）
    /*************群聊功能：TCP 群消息与群历史入口***************/
    void do_history(const Message& msg);    // 历史分页
    void do_ack(const Message& msg);        // 投递/已读回执

    /*************P1-3：登录补推一次性取回（原来 3 个入口 5~7 个 DB 任务）***************/
    // 离线消息 + 待处理好友申请 + 好友申请结果 + 待审批入群申请 + 入群结果，
    // 由 Db::fetchLoginSync 在一个 SQL 任务、一条连接上取完，本函数只负责
    // 按顺序把它们编码成下行帧。风暴期每条登录的 DB 任务数从 ~7 降到 1。
    void push_login_sync();
    /*************P1-3：登录补推一次性取回***************/

    void send_message(const Message& msg);  // 组帧后入写队列
    void send_error(const std::string& text, std::uint64_t to);
    void do_write();                        // P1-8：一次 async_write 合并多帧
    void reset_idle_timer();

    // P0-4：本连接在途 DB 任务计数（投递前检查上限，回调里递减）。
    bool   reserveJob();
    void   releaseJob();

    // 真正的关闭逻辑；close() 负责把它切回本连接的 strand 再执行。
    void do_close();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer     idle_timer_;

    /*************并发登录/注册优化：每条连接一个 strand***************/
    // 多线程 run() 同一个 io_context 时，本连接的读/写/DB 回调可能落到不同线程；
    // strand 保证这些回调串行执行，session 内部状态无需额外加锁。
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    /*************并发登录/注册优化：每条连接一个 strand***************/

    SessionManager&               manager_;
    Db&                           db_;
    TokenService&                 tokens_;   // JWT 校验能力（登录时验签，不再查库比对密码）

    char                          header_[frame::HEADER_LEN];
    std::vector<char>             body_;

    /*************P0-4：写队列高水位（内存与带宽预算）***************/
    // 3Mbps 链路下，接收方慢时发送缓冲会一直堆在 write_queue_ 里。
    // 无界队列在 2GB 机器上是直接的 OOM 路径，所以：
    //   * 统计队列字节数，超过 kMaxWriteBytes 就主动断开这条连接，
    //     让客户端重连后按游标/历史接口重新对齐（消息本身已落库，不会丢）；
    //   * 断开比堆积好：堆积最终同样会导致服务整体不可用。
    std::deque<std::string>       write_queue_;
    std::size_t                   write_queue_bytes_ = 0;
    /*************P0-4：写队列高水位 —— 必须与整机内存预算相称***************/
    // 上一版这里是"每连接 4MB"，配合 TCP_MAX_SESSIONS=2000，隐含的最坏内存是
    // 8GB —— 而这台机器只有 2GB，还要和 innodb_buffer_pool_size=384M 的 mariadbd 分。
    // 单连接上限本身没错（对端读得慢时值得缓冲），**错在没有全局预算**：
    // 500 条慢连接各堆 1MB 就是 500MB，压测里"客户端阻塞在 HTTP DELETE、
    // TCP 侧仍在被推事件"恰好就是这种形态。
    // 现在两级一起生效：
    //   * 每连接 512KB：一条连接最多多堆 512KB（约 2000 条消息帧）；
    //   * 进程级 64MB ：所有连接加起来的上限，超过就断开"这次要写入"的那条连接，
    //                  优先保住已经能正常排空 majority 会话。
    // 两个数值都必须配合 do_close() 里的归还逻辑：写队列字节是全局计数，
    // 连接销毁时若不清掉，预算会随连接 churn 单向漂移到"永远满载"。
    static constexpr std::size_t  kMaxWriteBytes = 512 * 1024;         // 512KB/连接
    static constexpr std::size_t  kGlobalWriteBudgetBytes = 64 * 1024 * 1024;  // 64MB 总量
    /*************P0-4：写队列高水位 —— 必须与整机内存预算相称***************/

    /*************P1-8：合并写的一次批量上限***************/
    static constexpr int          kMaxWriteFrames      = 32;        // 最多 32 帧
    static constexpr std::size_t  kMaxWriteBatchBytes  = 64 * 1024; // 或 64KB
    /*************P1-8：合并写的一次批量上限***************/

    /*************core-dump 修复 ①：一次只允许一个 async_write 在途***************/
    // asio/Beast 的前提是"同一条流、同一个方向，同时只有一个异步操作"。
    // 两个 async_write 同时在途 = 两套 op 各自推进同一个 socket 的发送状态，
    // 表现就是随机 core dump（而且只在"多帧成批"时才出现，单帧路径永远碰不到）。
    // 有了这个标志，send_message 的 "队列为空就启动写" 与写回调里的 "还有剩余就继续写"
    // 都不可能叠加出第二个并发写。
    bool                          writing_ = false;
    /*************core-dump 修复 ①：一次只允许一个 async_write 在途***************/

    /*************P1-3：单次登录补推的离线消息条数上限***************/
    // 原来固定 200：100 个用户同时重连 = 最多 2 万条 ≈ 5MB，3Mbps 链路要 13 秒，
    // 登录风暴会顺带把带宽打满。
    // 这里**不是**"越小越省"：超出上限的部分只能等下一次登录再补，
    // 所以调小是拿"补全延迟"换"瞬时带宽"。取 100（原值的一半）是折中：
    //   * 带宽突发从 ~5MB 降到 ~2.5MB；
    //   * 绝大多数用户一次就取完，落在"要等下次登录"区间里的只剩极端深队列用户。
    // 已知限制（必须如实说明）：offlineMore 目前只进日志，没有下行的"续拉"信令，
    // 因为加一个新帧类型会被老客户端的 parse_message 判为非法而断线。
    // 真正的修法是把 ACK 与补推打通（ACK 后若仍有未推完的队列就立刻继续推），
    // 那需要改 updateReceipt 的返回语义，属于后续项。
    static constexpr int          kLoginOfflineBatch   = 100;
    /*************P1-3：单次登录补推的离线消息条数上限***************/

    std::uint64_t                 user_id_   = 0;
    bool                          logged_in_ = false;
    bool                          logging_in_ = false;  // 防止登录请求并发处理
    bool                          closed_    = false;

    /*************P0-4：单连接在途 DB 任务上限***************/
    // 客户端可以连续狂发 Text，每条都会提交一个 DB 任务；上限保护的是
    // Db 的两条队列和整机吞吐，超过时回 Error("server busy") 让它降速。
    int                           outstanding_ = 0;
    std::chrono::seconds          idle_timeout_{IDLE_TIMEOUT};   // 构造时按环境变量覆盖
    /*************P0-4：单连接在途 DB 任务上限***************/
};

} // namespace kc
