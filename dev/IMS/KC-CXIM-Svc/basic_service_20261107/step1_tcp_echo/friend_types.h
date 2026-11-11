#pragma once
// ============================================================================
// friend_types.h：好友模块的领域模型与事件序列化
// ----------------------------------------------------------------------------
// 为什么单独放一个头文件？
//   好友功能同时被三个地方使用：
//     1) Db 层：查询/写入好友关系与好友申请；
//     2) HTTP 层：给客户端返回搜索结果、好友列表、申请列表；
//     3) TCP 层：把“收到申请 / 申请被通过或拒绝 / 好友列表变化”实时推给在线用户。
//   把结构体和事件 JSON 的拼装集中在这里，可以避免三处各写一份、字段不一致。
//
// 设计原则：
//   * 数据库里只存“事实”（申请状态、好友关系），事件只是事实变化后的通知；
//   * 推送事件统一用 JSON 字符串放进 Message.content，便于协议扩展；
//   * 所有事件都带 event 字段，客户端可以据此区分同一消息类型下的不同语义。
// ============================================================================

#include <cstdint>
#include <string>

#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace kc {

// 用户搜索接口返回的公开字段。
// 注意：绝不能把 password_hash、token 等敏感字段放进这个结构体，
// 否则很容易在后续序列化时不小心泄露给客户端。
struct UserInfo {
    std::uint64_t id = 0;          // 用户ID（协议中的 userId）
    std::string   username;        // 登录名（全局唯一）
    std::string   nickname;        // 昵称
    std::string   avatar_url;      // 头像地址
};

// 好友列表中的一行：好友公开资料 + 我给他设置的备注。
struct FriendInfo {
    std::uint64_t id = 0;          // 好友 userId
    std::string   username;        // 好友登录名
    std::string   nickname;        // 好友昵称
    std::string   avatar_url;      // 好友头像
    std::string   remark;          // 我对该好友的备注名
    int           status = 1;      // 1=好友，2=拉黑（当前列表只返回 1）
};

// 一条好友申请。
// 关系是单向的：requester_id 发起申请，target_id 收到申请。
// 接受后会在 friends 表写入 (requester,target) 与 (target,requester) 两行。
struct FriendRequestInfo {
    std::uint64_t id = 0;                 // friend_requests.id
    std::uint64_t requester_id = 0;       // 申请人
    std::uint64_t target_id = 0;          // 被申请人
    std::string   requester_username;     // 申请人登录名（展示用）
    std::string   requester_nickname;     // 申请人昵称（展示用）
    std::string   target_username;        // 被申请人登录名（展示用）
    std::string   target_nickname;        // 被申请人昵称（展示用）
    std::string   message;                // 申请附言
    int           status = 0;             // 0=待处理，1=已通过，2=已拒绝，3=已取消
    std::uint64_t created_ts = 0;         // 申请创建时间（毫秒）
};

// ---------------------------------------------------------------------------
// 事件 1：FriendRequest（type=130）
// ---------------------------------------------------------------------------
// 发送方向：申请人 -> 被申请人。
// 触发时机：
//   * 新的好友申请写入数据库后，被申请人在线则立即推送；
//   * 被申请人登录时，服务端把仍处于“待处理”的申请补推一遍（离线补偿）。
// event 取值：
//   "new"     ：刚收到的新申请；
//   "pending" ：登录时补推的历史待处理申请。
// ---------------------------------------------------------------------------
inline std::string serialize_friend_request_event(const FriendRequestInfo& r,
                                                  const char* event) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    w.StartObject();
    w.Key("event");              w.String(event ? event : "new");
    w.Key("request_id");         w.Uint64(r.id);
    w.Key("requester_id");       w.Uint64(r.requester_id);
    w.Key("target_id");          w.Uint64(r.target_id);
    w.Key("requester_username"); w.String(r.requester_username.c_str(), r.requester_username.size());
    w.Key("requester_nickname"); w.String(r.requester_nickname.c_str(), r.requester_nickname.size());
    w.Key("message");            w.String(r.message.c_str(), r.message.size());
    w.Key("status");             w.Int(r.status);
    w.Key("ts");                 w.Uint64(r.created_ts);
    w.EndObject();

    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// 事件 2：FriendRequestResult（type=131）
// ---------------------------------------------------------------------------
// 发送方向：被申请人（处理者） -> 申请人。
// 触发时机：申请被通过或拒绝。
// event 取值："accepted" / "rejected"。
// 如果申请人当时离线，数据库里的 result_notified 会保持 0，
// 等申请人下次 TCP 登录时再补推，避免“离线就永远不知道结果”。
// ---------------------------------------------------------------------------
inline std::string serialize_friend_result_event(const FriendRequestInfo& r,
                                                 const char* event) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    w.StartObject();
    w.Key("event");           w.String(event ? event : "accepted");
    w.Key("request_id");      w.Uint64(r.id);
    w.Key("requester_id");    w.Uint64(r.requester_id);
    w.Key("target_id");       w.Uint64(r.target_id);
    w.Key("target_username"); w.String(r.target_username.c_str(), r.target_username.size());
    w.Key("target_nickname"); w.String(r.target_nickname.c_str(), r.target_nickname.size());
    w.Key("message");         w.String(r.message.c_str(), r.message.size());
    w.Key("status");          w.Int(r.status);
    w.Key("ts");              w.Uint64(r.created_ts);
    w.EndObject();

    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// 事件 3：FriendListChanged（type=132）
// ---------------------------------------------------------------------------
// 发送方向：当前操作者 -> 自己/对方（按操作语义决定）。
// 触发时机：通过申请、删除好友、修改备注、拉黑/取消拉黑。
// action 取值："added" / "deleted" / "updated" / "blocked" / "unblocked"。
// 作用：多端场景下，一端修改好友关系后，其他端可以立即刷新列表，
//       而不需要轮询 GET /api/friends。
// ---------------------------------------------------------------------------
inline std::string serialize_friend_list_changed(const char* action,
                                                 std::uint64_t friend_id,
                                                 const std::string& remark,
                                                 int status) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    w.StartObject();
    w.Key("event");     w.String("friend_list_changed");
    w.Key("action");    w.String(action ? action : "updated");
    w.Key("friend_id"); w.Uint64(friend_id);
    w.Key("remark");    w.String(remark.c_str(), remark.size());
    w.Key("status");    w.Int(status);
    w.EndObject();

    return std::string(sb.GetString(), sb.GetSize());
}

} // namespace kc
