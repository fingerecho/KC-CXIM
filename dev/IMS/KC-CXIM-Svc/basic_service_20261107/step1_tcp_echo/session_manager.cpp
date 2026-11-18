#include "session_manager.h"
#include "session.h"   // 需要 session 的完整定义才能 lock()/调用其方法

namespace kc {

// ---------------------------------------------------------------------------
// add：登记一个在线用户。
// 返回旧的连接（若同一账号重复登录），让调用方在"锁外"去关闭旧连接。
// 为什么不在锁内关？因为 close() 里可能又会回调 manager（remove），
// 造成同一把 mutex 重入 -> 死锁。锁的粒度要尽量小。
// ---------------------------------------------------------------------------
SessionManager::SessionPtr
SessionManager::add(UserId uid, const SessionPtr& s) {
    SessionPtr old;

    // lock_guard：作用域开始时加锁，函数返回时自动解锁（RAII，异常安全）。
    std::lock_guard<std::mutex> lk(mutex_);

    auto it = sessions_.find(uid);
    if (it != sessions_.end()) old = it->second.lock();  // 旧连接可能已失效 -> 得到空

    sessions_[uid] = s;   // 覆盖登记（weak_ptr 记录，不增加引用计数）
    return old;           // 锁在这里析构释放，之后调用方才去 close 旧连接
}

// ---------------------------------------------------------------------------
// remove：用户下线时删除登记项，**但要删的是自己**。
// ---------------------------------------------------------------------------
// 为什么必须比对身份（core-dump 修复 ③ 的另一半）：
// 重复登录的顺序是「新连接 add 覆盖这一格 → 再 close 旧连接」，
// 所以旧连接最终一定会走到 remove，而此时表里那一格属于**新连接**。
// 无条件 erase(uid) 会把新连接的登记抹掉：它的 socket 完全正常、事件循环也还在跑，
// 但 SessionManager 已经找不到它 —— 之后所有发给这个用户的路由都判成
// "对方离线"（session.cpp 走 offline 落库分支），实时消息永久失联，
// 只有下次重连才恢复。压测里同账号快速重登、以及"被踢的一方延迟关闭"都会命中。
// owner==nullptr 保留"无条件删"的语义（调用方明确知道自己要做清理时才用）。
void SessionManager::remove(UserId uid, const session* owner) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sessions_.find(uid);
    if (it == sessions_.end()) return;
    if (owner) {
        // 只有这一格仍然是我们时才删；已被新连接覆盖则什么都不做。
        auto live = it->second.lock();
        if (live && live.get() != owner) return;
    }
    sessions_.erase(it);
}

// ---------------------------------------------------------------------------
// find：按 userId 找连接。
// weak_ptr::lock() 的语义：如果对象还活着，返回一个 shared_ptr（引用计数 +1，
// 保证调用期间对象不被销毁）；如果对象已经销毁，返回空 shared_ptr。
// 所以调用方拿到的一定是"本次使用期间有效"的连接。
// ---------------------------------------------------------------------------
SessionManager::SessionPtr SessionManager::find(UserId uid) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sessions_.find(uid);
    if (it == sessions_.end()) return nullptr;
    return it->second.lock();
}

// ---------------------------------------------------------------------------
// size：当前登记的用户数。
// 注意：这里统计的是"表里的条目数"，某个连接若已意外销毁（weak 过期）但仍未
// 调用 remove，会短暂多算一条。正常流程下 close() 一定会 remove，故无影响。
// 若要严格，可遍历并对 lock() 成功的才计数。
// ---------------------------------------------------------------------------
std::size_t SessionManager::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return sessions_.size();
}

// ---------------------------------------------------------------------------
// online_users：返回所有登记的用户 id 的拷贝。
// 返回副本而不是内部容器的引用，避免调用方在锁外遍历时被其他线程修改。
// ---------------------------------------------------------------------------
std::vector<SessionManager::UserId> SessionManager::online_users() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<UserId> ids;
    ids.reserve(sessions_.size());          // 预留容量，避免多次扩容
    for (auto& kv : sessions_) ids.push_back(kv.first);
    return ids;
}

} // namespace kc
