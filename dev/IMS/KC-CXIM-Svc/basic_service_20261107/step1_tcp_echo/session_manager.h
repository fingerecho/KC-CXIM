#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// ============================================================================
// SessionManager：在线用户表（谁连着服务器）
// ----------------------------------------------------------------------------
// 核心是一张哈希表：  userId  ->  session（连接）
// 这样收到"发给 1002"的消息时，就能 O(1) 查到 1002 的那条 TCP 连接。
//
// 为什么值用 weak_ptr 而不是 shared_ptr？
//   - 连接对象 session 由 Boost.Asio 的异步回调（lambda 里的 self）负责保活；
//   - 如果 manager 再用 shared_ptr 持有它，就是"强引用"，即使连接已断，
//     只要 manager 不删就永远不析构，容易泄漏/延长生命周期；
//   - weak_ptr 只"观察"不"拥有"：连接死了它自动变成 expired，lock() 返回空。
// ============================================================================

namespace kc {

class session;   // 前向声明：这里只需要"指针类型"，不用看到 session 的完整定义

class SessionManager {
public:
    using UserId     = std::uint64_t;
    using SessionPtr = std::shared_ptr<session>;

    // 注册登录。若该 userId 已在线，返回旧连接（调用方在锁外关掉它，避免死锁）。
    SessionPtr add(UserId uid, const SessionPtr& s);
    // 注销（连接断开时调用）。
    // 带 owner：**只有当这一格登记的确实是自己时才删**。
    // 重复登录的顺序是"新连接 add 覆盖 → 旧连接 close"，旧连接若无条件按 uid 删，
    // 删掉的是新连接的登记：它还活着却不再可路由，别人发给它的消息全被判成离线。
    // owner 传 nullptr 表示"无条件删"（仅用于清理/测试）。
    void       remove(UserId uid, const session* owner = nullptr);
    // 按 userId 查连接；不在线或连接已销毁则返回 nullptr。
    SessionPtr find(UserId uid) const;
    // 当前表里登记的用户数。
    std::size_t size() const;
    // 所有已登记的用户 id（调试/广播用）。
    std::vector<UserId> online_users() const;

private:
    // mutable：让 const 成员函数（find/size/online_users）也能加锁。
    // 加锁不改变"逻辑状态"，只是保护数据，所以语义上仍是 const 操作。
    mutable std::mutex mutex_;

    // 哈希表：用户 id -> 弱引用连接。
    // unordered_map 平均 O(1) 查找，适合这种高频"按 id 找人"的场景。
    std::unordered_map<UserId, std::weak_ptr<session>> sessions_;
};

} // namespace kc
