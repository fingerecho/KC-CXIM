#include "http_server.h"                       // 本模块声明

#include "db.h"                                // Db：把重活投递到 worker 线程
#include "friend_types.h"                      // 好友事件 JSON 序列化
#include "group_types.h"                       // /*************群聊功能：群领域模型与事件序列化***************/
#include "session.h"                           // SessionManager::find 返回的 session::deliver
#include "session_manager.h"                   // 按 userId 查找在线 TCP 连接
#include "token_service.h"                     // TokenService：HTTP 接口鉴权

#include <boost/beast/core.hpp>                // Beast 核心：flat_buffer / error_code 等
#include <boost/beast/http.hpp>                // Beast HTTP：request/response/async_read/write
#include <boost/beast/version.hpp>             // Beast 版本号（用于 Server 头）

#include "rapidjson/document.h"                // 解析请求 JSON
#include "rapidjson/stringbuffer.h"            // 生成响应 JSON 的缓冲区
#include "rapidjson/writer.h"                  // 把 JSON 写进缓冲区

#include <cctype>                              // isdigit / isxdigit
#include <algorithm>                           // /*************群聊功能：std::sort/std::unique 事件扇出去重***************/
#include <chrono>                              // 事件时间戳
#include <iostream>                            // 打印监听/错误日志
#include <memory>                              // std::shared_ptr 管理连接生命周期
#include <string>                              // std::string
#include <vector>                              // 列表类响应

namespace kc {

namespace beast = boost::beast;                // 简写 Beast 命名空间
namespace http  = beast::http;                 // 简写 HTTP 命名空间
namespace net   = boost::asio;                // 简写 Asio 命名空间
using tcp       = boost::asio::ip::tcp;       // TCP 类型简写

namespace {

// ============================================================================
// 通用 JSON 响应构造
// ----------------------------------------------------------------------------
// 统一响应格式：{"code":0/1,"message":"...","data":{...}}
//   code=0 成功，code!=0 失败；HTTP 状态码再表达语义（400/401/404/405）。
// 这样客户端既能用 HTTP 状态码做粗粒度处理，也能用 code 做业务分支。
// ============================================================================

std::string make_error(const std::string& message) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(1);
    w.Key("message"); w.String(message.c_str(), message.size());
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_ok() {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_register_ok(std::uint64_t userId) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("userId");  w.Uint64(userId);
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_login_ok(std::uint64_t userId,
                          const std::string& username,
                          const std::string& token) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("userId");   w.Uint64(userId);
    w.Key("username"); w.String(username.c_str(), username.size());
    w.Key("token");    w.String(token.c_str(), token.size());
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

// ---- 好友模块：列表/单个对象的响应 -----------------------------------------
// 只输出公开字段；数据库里的 password_hash / status 等不会出现在搜索接口。
std::string make_users_ok(const std::vector<UserInfo>& users) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("users");   w.StartArray();
    for (const auto& u : users) {
        w.StartObject();
        w.Key("userId");    w.Uint64(u.id);
        w.Key("username");  w.String(u.username.c_str(), u.username.size());
        w.Key("nickname");  w.String(u.nickname.c_str(), u.nickname.size());
        w.Key("avatarUrl"); w.String(u.avatar_url.c_str(), u.avatar_url.size());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_friends_ok(const std::vector<FriendInfo>& friends) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("friends"); w.StartArray();
    for (const auto& f : friends) {
        w.StartObject();
        w.Key("userId");    w.Uint64(f.id);
        w.Key("username");  w.String(f.username.c_str(), f.username.size());
        w.Key("nickname");  w.String(f.nickname.c_str(), f.nickname.size());
        w.Key("avatarUrl"); w.String(f.avatar_url.c_str(), f.avatar_url.size());
        w.Key("remark");    w.String(f.remark.c_str(), f.remark.size());
        w.Key("status");    w.Int(f.status);
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

void write_request_object(rapidjson::Writer<rapidjson::StringBuffer>& w,
                          const FriendRequestInfo& r) {
    w.StartObject();
    w.Key("requestId");         w.Uint64(r.id);
    w.Key("requesterId");       w.Uint64(r.requester_id);
    w.Key("targetId");          w.Uint64(r.target_id);
    w.Key("requesterUsername"); w.String(r.requester_username.c_str(), r.requester_username.size());
    w.Key("requesterNickname"); w.String(r.requester_nickname.c_str(), r.requester_nickname.size());
    w.Key("targetUsername");    w.String(r.target_username.c_str(), r.target_username.size());
    w.Key("targetNickname");    w.String(r.target_nickname.c_str(), r.target_nickname.size());
    w.Key("message");           w.String(r.message.c_str(), r.message.size());
    w.Key("status");            w.Int(r.status);
    w.Key("createdTs");         w.Uint64(r.created_ts);
    w.EndObject();
}

std::string make_request_ok(const FriendRequestInfo& r) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("request");
    write_request_object(w, r);
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_requests_ok(const std::vector<FriendRequestInfo>& reqs) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("requests"); w.StartArray();
    for (const auto& r : reqs) write_request_object(w, r);
    w.EndArray();
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

/*************群聊功能：群相关响应构造（沿用 {code,message,data} 统一结构）***************/
std::string make_group_ok(const GroupInfo& g) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("group");
    write_group_json(w, g);
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_groups_ok(const std::vector<GroupInfo>& groups) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("groups");  w.StartArray();
    for (const auto& g : groups) write_group_json(w, g);
    w.EndArray();
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_group_detail_ok(const GroupInfo& g,
                                 const std::vector<GroupMemberInfo>& members) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("group");
    write_group_json(w, g);
    w.Key("members"); w.StartArray();
    for (const auto& m : members) write_member_json(w, m);
    w.EndArray();
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_group_request_ok(const GroupRequestInfo& r) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("request");
    write_request_json(w, r);
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_group_requests_ok(const std::vector<GroupRequestInfo>& reqs) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("requests"); w.StartArray();
    for (const auto& r : reqs) write_request_json(w, r);
    w.EndArray();
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}
/*************群聊功能：群相关响应构造结束***************/

// 当前毫秒时间戳：只用于推送事件的展示时间，权威时间仍在数据库。
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// 轻量 URL 解码：搜索关键字里可能有中文、空格等，需要先解码再交给 SQL。
// 只处理 %XX 与 '+'，对普通 ASCII 原样保留，足够覆盖本项目接口。
// ---------------------------------------------------------------------------
std::string url_decode(const std::string& in) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < in.size()) {
            const int hi = hex(in[i + 1]);
            const int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(c);
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// 从查询串里取某个参数，例如 "keyword=abc&limit=20"。
std::string query_value(const std::string& query, const std::string& key) {
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const std::size_t amp = query.find('&', pos);
        const std::string kv = query.substr(pos, amp == std::string::npos
                                                        ? std::string::npos
                                                        : amp - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            return url_decode(kv.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return {};
}

// 解析形如 /api/friends/{id}/block 的路径。
// 返回 false 表示路径不匹配或 id 非法；这样路由可以安全地继续尝试下一个规则。
bool parse_id_between(const std::string& path,
                      const std::string& prefix,
                      const std::string& suffix,
                      std::uint64_t& out) {
    if (path.size() <= prefix.size() + suffix.size()) return false;
    if (path.compare(0, prefix.size(), prefix) != 0) return false;

    const std::size_t end = path.size() - suffix.size();
    if (!suffix.empty() && path.compare(end, suffix.size(), suffix) != 0) return false;

    const std::string mid = path.substr(prefix.size(), end - prefix.size());
    if (mid.empty()) return false;
    for (char c : mid) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    try {
        out = std::stoull(mid);
    } catch (...) {
        return false;
    }
    return out != 0;
}

// 解析形如 /api/groups/{groupId}/members/{userId} 的路径。
// /*************群聊功能：双 ID 路径解析（移出成员用）***************/
bool parse_two_ids(const std::string& path,
                   const std::string& prefix,
                   const std::string& middle,
                   std::uint64_t& id1,
                   std::uint64_t& id2) {
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    const std::size_t mid = path.find(middle, prefix.size());
    if (mid == std::string::npos) return false;

    const std::string s1 = path.substr(prefix.size(), mid - prefix.size());
    const std::string s2 = path.substr(mid + middle.size());
    if (s1.empty() || s2.empty()) return false;
    auto all_digits = [](const std::string& s) {
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return true;
    };
    if (!all_digits(s1) || !all_digits(s2)) return false;
    try {
        id1 = std::stoull(s1);
        id2 = std::stoull(s2);
    } catch (...) {
        return false;
    }
    return id1 != 0 && id2 != 0;
}
/*************群聊功能：双 ID 路径解析***************/

// RapidJSON 的数字可能是 Uint64，也可能是 Int64（取决于字面量），统一取值。
bool json_uint64(const rapidjson::Value& v, std::uint64_t& out) {    if (v.IsUint64()) {
        out = v.GetUint64();
        return true;
    }
    if (v.IsInt64() && v.GetInt64() >= 0) {
        out = static_cast<std::uint64_t>(v.GetInt64());
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// http_session：一条 HTTP 短连接
// ----------------------------------------------------------------------------
// 路由分两类：
//   * 公开接口：/api/register、/api/login；
//   * 需要 JWT 的接口：搜索用户、好友申请、好友列表、好友维护。
// 所有耗时操作（SQL、bcrypt）都交给 Db 的 worker 线程；
// 本类只在 io_context 线程里解析 JSON、入队、回写响应和推送好友事件。
// ---------------------------------------------------------------------------
class http_session : public std::enable_shared_from_this<http_session> {
public:
    http_session(tcp::socket socket, Db& db, TokenService& tokens,
                 SessionManager& sessions)
        : socket_(std::move(socket)),
          db_(db),
          tokens_(tokens),
          sessions_(sessions) {}

    void run() { do_read(); }

private:
    void do_read() {
        auto self = shared_from_this();
        http::async_read(socket_, buffer_, req_,
            [self](beast::error_code ec, std::size_t /*bytes*/) {
                if (ec) return;
                self->handle_request();
            });
    }

    // 路由入口：先用路径 + 方法选择处理函数，再做鉴权/参数校验。
    void handle_request() {
        std::string full(req_.target());
        std::string path = full;
        std::string query;
        const auto qpos = full.find('?');
        if (qpos != std::string::npos) {
            path  = full.substr(0, qpos);
            query = full.substr(qpos + 1);
        }

        const auto method = req_.method();

        // ---- 公开接口 ------------------------------------------------------
        if (path == "/api/register") {
            if (method != http::verb::post) return method_not_allowed();
            std::string username, password;
            if (!parse_credentials(username, password)) return;
            do_register(username, password);
            return;
        }
        if (path == "/api/login") {
            if (method != http::verb::post) return method_not_allowed();
            std::string username, password;
            if (!parse_credentials(username, password)) return;
            do_login(username, password);
            return;
        }

        // ---- 以下接口统一要求 Bearer JWT --------------------------------
        std::uint64_t uid = 0;
        std::string username;
        if (!authenticate(uid, username)) return;

        // BE-FRIEND-01：搜索用户
        if (path == "/api/users/search") {
            if (method != http::verb::get) return method_not_allowed();
            do_search(uid, query);
            return;
        }

        // BE-FRIEND-05：好友列表
        if (path == "/api/friends") {
            if (method != http::verb::get) return method_not_allowed();
            do_list_friends(uid);
            return;
        }

        // BE-FRIEND-03/05：申请列表与创建申请
        if (path == "/api/friends/requests") {
            if (method == http::verb::get) {
                do_list_requests(uid, query);
                return;
            }
            if (method == http::verb::post) {
                do_create_request(uid);
                return;
            }
            return method_not_allowed();
        }

        // BE-FRIEND-04：通过 / 拒绝申请
        std::uint64_t id = 0;
        if (parse_id_between(path, "/api/friends/requests/", "/accept", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_handle_request(uid, id, true);
            return;
        }
        if (parse_id_between(path, "/api/friends/requests/", "/reject", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_handle_request(uid, id, false);
            return;
        }

        // BE-FRIEND-05：拉黑 / 取消拉黑
        if (parse_id_between(path, "/api/friends/", "/block", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_block_friend(uid, id, true);
            return;
        }
        if (parse_id_between(path, "/api/friends/", "/unblock", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_block_friend(uid, id, false);
            return;
        }

        // BE-FRIEND-05：修改备注 / 删除好友
        if (parse_id_between(path, "/api/friends/", "", id)) {
            if (method == http::verb::patch) {
                do_update_remark(uid, id);
                return;
            }
            if (method == http::verb::delete_) {
                do_delete_friend(uid, id);
                return;
            }
            return method_not_allowed();
        }

        // ====================================================================
        // /*************群聊功能：群聊路由（BE-GROUP-01~08）***************/
        // ====================================================================
        // 路由顺序很关键：/api/groups/requests 必须先于 /api/groups/{id} 匹配，
        // 否则 "requests" 会被 parse_id_between 当作非法 id（它有数字校验，
        // 其实不会误判，但把具体路径放前面更清晰，和好友模块保持一致）。
        if (path == "/api/groups") {
            if (method == http::verb::get)  { do_list_groups(uid);            return; }
            if (method == http::verb::post) { do_create_group(uid);           return; }
            return method_not_allowed();
        }
        if (path == "/api/groups/requests") {
            if (method == http::verb::post) { do_create_group_request(uid);   return; }
            if (method == http::verb::get)  { do_list_group_requests(uid, query); return; }
            return method_not_allowed();
        }
        // 审批入群申请（通过 / 拒绝）
        if (parse_id_between(path, "/api/groups/requests/", "/accept", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_handle_group_request(uid, id, true);
            return;
        }
        if (parse_id_between(path, "/api/groups/requests/", "/reject", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_handle_group_request(uid, id, false);
            return;
        }
        // 移出成员：DELETE /api/groups/{groupId}/members/{userId}
        {
            std::uint64_t gid = 0, member = 0;
            if (parse_two_ids(path, "/api/groups/", "/members/", gid, member)) {
                if (method == http::verb::delete_) { do_remove_member(uid, gid, member); return; }
                return method_not_allowed();
            }
        }
        // 邀请成员：POST /api/groups/{groupId}/members
        if (parse_id_between(path, "/api/groups/", "/members", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_invite_members(uid, id);
            return;
        }
        // 转让群主
        if (parse_id_between(path, "/api/groups/", "/transfer", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_transfer_group(uid, id);
            return;
        }
        // 设置/取消管理员：POST /api/groups/{groupId}/admins
        if (parse_id_between(path, "/api/groups/", "/admins", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_set_admin(uid, id);
            return;
        }
        // 解散群
        if (parse_id_between(path, "/api/groups/", "/dismiss", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_dismiss_group(uid, id);
            return;
        }
        // 退出群
        if (parse_id_between(path, "/api/groups/", "/quit", id)) {
            if (method != http::verb::post) return method_not_allowed();
            do_quit_group(uid, id);
            return;
        }
        // 群详情 / 修改群资料
        if (parse_id_between(path, "/api/groups/", "", id)) {
            if (method == http::verb::get)   { do_group_detail(uid, id);   return; }
            if (method == http::verb::patch) { do_update_group(uid, id);   return; }
            return method_not_allowed();
        }
        // /*************群聊功能：群聊路由结束***************/

        send_json(http::status::not_found, make_error("not found"));
    }

    void method_not_allowed() {
        send_json(http::status::method_not_allowed, make_error("method not allowed"));
    }

    // 从 Authorization: Bearer <JWT> 中取出 token 并验签。
    // 身份只信任 JWT 的 sub（userId），绝不信任请求体里自报的 userId。
    bool authenticate(std::uint64_t& uid, std::string& username) {
        const auto it = req_.find(http::field::authorization);
        if (it == req_.end()) {
            send_json(http::status::unauthorized, make_error("missing authorization"));
            return false;
        }

        const std::string value(it->value());
        const std::string prefix = "Bearer ";
        if (value.compare(0, prefix.size(), prefix) != 0) {
            send_json(http::status::unauthorized, make_error("expected Bearer token"));
            return false;
        }

        if (!tokens_.verify(value.substr(prefix.size()), uid, username)) {
            send_json(http::status::unauthorized, make_error("invalid or expired token"));
            return false;
        }
        return true;
    }

    // 解析请求体为 JSON 对象；失败时已经回 400。
    bool read_json(rapidjson::Document& doc) {
        const std::string& body = req_.body();
        doc.Parse(body.data(), body.size());
        if (doc.HasParseError() || !doc.IsObject()) {
            send_json(http::status::bad_request, make_error("invalid json"));
            return false;
        }
        return true;
    }

    // 注册/登录共用的账号密码解析与基础校验。
    bool parse_credentials(std::string& username, std::string& password) {
        rapidjson::Document doc;
        if (!read_json(doc)) return false;

        if (!doc.HasMember("username") || !doc["username"].IsString() ||
            !doc.HasMember("password") || !doc["password"].IsString()) {
            send_json(http::status::bad_request,
                      make_error("missing username/password"));
            return false;
        }

        username = doc["username"].GetString();
        password = doc["password"].GetString();

        if (username.empty() || username.size() > 64) {
            send_json(http::status::bad_request, make_error("invalid username"));
            return false;
        }
        if (password.size() < 6 || password.size() > 72) {
            send_json(http::status::bad_request, make_error("invalid password"));
            return false;
        }
        return true;
    }

    // ---- 注册 / 登录 ------------------------------------------------------
    void do_register(const std::string& username, const std::string& password) {
        auto self = shared_from_this();
        db_.registerUser(username, password,
            [self](bool ok, const std::string& err, std::uint64_t userId) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->send_json(http::status::ok, make_register_ok(userId));
            });
    }

    void do_login(const std::string& username, const std::string& password) {
        auto self = shared_from_this();
        db_.loginUser(username, password,
            [self](bool ok, const std::string& err,
                   std::uint64_t userId, const std::string& name) {
                if (!ok) {
                    self->send_json(http::status::unauthorized, make_error(err));
                    return;
                }
                const std::string token = self->tokens_.sign(userId, name);
                self->send_json(http::status::ok, make_login_ok(userId, name, token));
            });
    }

    // ---- BE-FRIEND-01：搜索用户 ------------------------------------------
    void do_search(std::uint64_t uid, const std::string& query) {
        const std::string keyword = query_value(query, "keyword");
        if (keyword.empty()) {
            send_json(http::status::bad_request, make_error("missing keyword"));
            return;
        }

        int limit = 20;
        const std::string limitText = query_value(query, "limit");
        if (!limitText.empty()) {
            try { limit = std::stoi(limitText); } catch (...) { limit = 20; }
        }

        auto self = shared_from_this();
        db_.searchUsers(uid, keyword, limit,
            [self](bool ok, const std::string& err, std::vector<UserInfo> users) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->send_json(http::status::ok, make_users_ok(users));
            });
    }

    // ---- BE-FRIEND-03：创建好友申请 --------------------------------------
    void do_create_request(std::uint64_t uid) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;

        if (!doc.HasMember("toUserId")) {
            send_json(http::status::bad_request, make_error("missing toUserId"));
            return;
        }
        std::uint64_t targetId = 0;
        if (!json_uint64(doc["toUserId"], targetId) || targetId == 0) {
            send_json(http::status::bad_request, make_error("invalid toUserId"));
            return;
        }

        std::string message;
        if (doc.HasMember("message") && doc["message"].IsString())
            message = doc["message"].GetString();
        if (message.size() > 255) {
            send_json(http::status::bad_request, make_error("message too long"));
            return;
        }

        auto self = shared_from_this();
        db_.createFriendRequest(uid, targetId, message,
            [self, uid, targetId](bool ok, const std::string& err,
                                  FriendRequestInfo r) {
                if (!ok) {
                    // 业务拒绝（已是好友、重复申请、被拉黑等）统一 400。
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }

                // 实时推给被申请人；对方离线则不推，等他登录时由
                // session::push_friend_events() 从 friend_requests 补推。
                self->push_event(targetId, MsgType::FriendRequest, uid, r.id,
                                 serialize_friend_request_event(r, "new"));

                self->send_json(http::status::ok, make_request_ok(r));
            });
    }

    // ---- BE-FRIEND-04：通过 / 拒绝申请 -----------------------------------
    void do_handle_request(std::uint64_t uid, std::uint64_t requestId, bool accept) {
        auto self = shared_from_this();
        db_.handleFriendRequest(uid, requestId, accept,
            [self, uid, accept](bool ok, const std::string& err,
                                FriendRequestInfo r) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }

                // 结果推给申请人。申请人离线时 result_notified 仍为 0，
                // 由登录补偿逻辑补推，不会丢。
                const bool delivered = self->push_event(
                    r.requester_id, MsgType::FriendRequestResult, uid, r.id,
                    serialize_friend_result_event(r, accept ? "accepted" : "rejected"));

                // 申请人此刻在线且已实时推送，就把这条结果标记为“已通知”，
                // 避免他下次登录时收到重复的 accepted/rejected 事件。
                if (delivered) {
                    self->db_.markRequestResultNotified(r.id,
                        [](bool ok, const std::string& err) {
                            if (!ok)
                                std::cerr << "[friend] mark result notified failed: "
                                          << err << std::endl;
                        });
                }

                if (accept) {
                    // 双方好友列表都发生变化，各推一条列表变更事件。
                    self->push_event(r.requester_id, MsgType::FriendListChanged, uid, 0,
                                     serialize_friend_list_changed("added", uid, "", 1));
                    self->push_event(uid, MsgType::FriendListChanged, uid, 0,
                                     serialize_friend_list_changed(
                                         "added", r.requester_id, "", 1));
                }

                self->send_json(http::status::ok, make_request_ok(r));
            });
    }

    // ---- BE-FRIEND-05：申请列表 ------------------------------------------
    void do_list_requests(std::uint64_t uid, const std::string& query) {
        std::string direction = query_value(query, "direction");
        if (direction.empty()) direction = "incoming";
        if (direction != "incoming" && direction != "outgoing" && direction != "all") {
            send_json(http::status::bad_request, make_error("invalid direction"));
            return;
        }

        auto self = shared_from_this();
        db_.listFriendRequests(uid, direction,
            [self](bool ok, const std::string& err,
                   std::vector<FriendRequestInfo> reqs) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->send_json(http::status::ok, make_requests_ok(reqs));
            });
    }

    // ---- BE-FRIEND-05：好友列表 ------------------------------------------
    void do_list_friends(std::uint64_t uid) {
        auto self = shared_from_this();
        db_.listFriends(uid,
            [self](bool ok, const std::string& err, std::vector<FriendInfo> friends) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->send_json(http::status::ok, make_friends_ok(friends));
            });
    }

    // ---- BE-FRIEND-05：修改备注 ------------------------------------------
    void do_update_remark(std::uint64_t uid, std::uint64_t friendId) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;

        if (!doc.HasMember("remark") || !doc["remark"].IsString()) {
            send_json(http::status::bad_request, make_error("missing remark"));
            return;
        }
        std::string remark = doc["remark"].GetString();
        if (remark.size() > 64) {
            send_json(http::status::bad_request, make_error("remark too long"));
            return;
        }

        auto self = shared_from_this();
        db_.updateFriendRemark(uid, friendId, remark,
            [self, uid, friendId, remark](bool ok, const std::string& err) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->push_event(uid, MsgType::FriendListChanged, uid, 0,
                                 serialize_friend_list_changed("updated", friendId, remark, 1));
                self->send_json(http::status::ok, make_ok());
            });
    }

    // ---- BE-FRIEND-05：删除好友 ------------------------------------------
    void do_delete_friend(std::uint64_t uid, std::uint64_t friendId) {
        auto self = shared_from_this();
        db_.deleteFriend(uid, friendId,
            [self, uid, friendId](bool ok, const std::string& err) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // 双方都推：一端删除后，另一端的好友列表也应立即刷新。
                self->push_event(uid, MsgType::FriendListChanged, uid, 0,
                                 serialize_friend_list_changed("deleted", friendId, "", 0));
                self->push_event(friendId, MsgType::FriendListChanged, uid, 0,
                                 serialize_friend_list_changed("deleted", uid, "", 0));
                self->send_json(http::status::ok, make_ok());
            });
    }

    // ---- BE-FRIEND-05：拉黑 / 取消拉黑 -----------------------------------
    void do_block_friend(std::uint64_t uid, std::uint64_t friendId, bool blocked) {
        auto self = shared_from_this();
        db_.setFriendBlocked(uid, friendId, blocked,
            [self, uid, friendId, blocked](bool ok, const std::string& err) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->push_event(uid, MsgType::FriendListChanged, uid, 0,
                                 serialize_friend_list_changed(
                                     blocked ? "blocked" : "unblocked",
                                     friendId, "", blocked ? 2 : 0));
                self->send_json(http::status::ok, make_ok());
            });
    }

    // ====================================================================
    // /*************群聊功能：HTTP 处理器（BE-GROUP-01~08）***************/
    // ====================================================================
    // 约定与好友模块一致：
    //   * 所有“写事实”成功后，通过 SessionManager 把群事件实时推给在线成员；
    //   * 离线成员不重推事件，登录后靠 GET /api/groups 对齐（成员/资料类），
    //     入群申请与结果则由 session::push_group_events() 做数据库补偿推送；
    //   * 参数错误/权限不足/业务拒绝统一返回 400，鉴权失败由 authenticate 返回 401。
    // ====================================================================

    // 从请求体解析成员 id 数组：{"memberIds":[1,2,3]}
    bool parse_id_array(const rapidjson::Value& arr, std::vector<std::uint64_t>& out) {
        if (!arr.IsArray()) return false;
        for (const auto& v : arr.GetArray()) {
            std::uint64_t id = 0;
            if (!json_uint64(v, id) || id == 0) return false;
            out.push_back(id);
        }
        return true;
    }

    // BE-GROUP-02：建群
    void do_create_group(std::uint64_t uid) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;
        if (!doc.HasMember("name") || !doc["name"].IsString()) {
            send_json(http::status::bad_request, make_error("missing name"));
            return;
        }
        std::string name = doc["name"].GetString();
        if (name.empty() || name.size() > 128) {
            send_json(http::status::bad_request, make_error("invalid name"));
            return;
        }
        std::vector<std::uint64_t> members;
        if (!doc.HasMember("memberIds") || !parse_id_array(doc["memberIds"], members)
            || members.empty()) {
            send_json(http::status::bad_request, make_error("invalid memberIds"));
            return;
        }

        auto self = shared_from_this();
        db_.createGroup(uid, name, members,
            [self, uid](bool ok, const std::string& err, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // 给除创建者外的每个“加入者”广播一条 added 事件（全群可见）。
                const std::uint64_t gid = res.group.id;
                auto targets = member_ids(res.members, uid);
                for (const auto& m : res.members) {
                    if (m.user_id == uid) continue;   // 创建者自己不用收 added
                    self->push_event_many(targets, MsgType::GroupMemberChanged, uid, 0,
                        serialize_group_member_changed("added", gid, m.user_id, m.role, uid));
                }
                self->send_json(http::status::ok, make_group_ok(res.group));
            });
    }

    // BE-GROUP-05：我的群列表
    void do_list_groups(std::uint64_t uid) {
        auto self = shared_from_this();
        db_.listMyGroups(uid,
            [self](bool ok, const std::string& err, std::vector<GroupInfo> groups) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->send_json(http::status::ok, make_groups_ok(groups));
            });
    }

    // BE-GROUP-05：群详情 + 成员
    void do_group_detail(std::uint64_t uid, std::uint64_t groupId) {
        auto self = shared_from_this();
        db_.getGroupDetail(groupId, uid,
            [self](bool ok, const std::string& err, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->send_json(http::status::ok, make_group_detail_ok(res.group, res.members));
            });
    }

    // BE-GROUP-05：修改群资料（仅群主/管理员）
    void do_update_group(std::uint64_t uid, std::uint64_t groupId) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;
        auto getStr = [&](const char* k) -> std::string {
            if (doc.HasMember(k) && doc[k].IsString()) return doc[k].GetString();
            return {};
        };
        std::string name         = getStr("name");
        std::string announcement = getStr("announcement");
        std::string intro        = getStr("intro");
        std::string avatar       = getStr("avatarUrl");
        if (name.empty() && announcement.empty() && intro.empty() && avatar.empty()) {
            send_json(http::status::bad_request, make_error("no fields to update"));
            return;
        }

        auto self = shared_from_this();
        db_.updateGroupInfo(groupId, uid, name, announcement, intro, avatar,
            [self, uid, groupId](bool ok, const std::string& err, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // 全群广播资料变更（含操作者自己，方便多端刷新）。
                self->push_event_many(member_ids(res.members), MsgType::GroupInfoChanged,
                                      uid, 0,
                                      serialize_group_info_changed(groupId, uid, res.group.name));
                self->send_json(http::status::ok, make_group_ok(res.group));
            });
    }

    // BE-GROUP-02：解散群（仅群主）
    void do_dismiss_group(std::uint64_t uid, std::uint64_t groupId) {
        auto self = shared_from_this();
        db_.dismissGroup(groupId, uid,
            [self, uid, groupId](bool ok, const std::string& err, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // 解散前 res.members 就是全部接收者（此刻成员行已被清空，只能靠返回值）。
                self->push_event_many(member_ids(res.members), MsgType::GroupDismissed,
                                      uid, 0,
                                      serialize_group_dismissed(groupId, uid, res.group.name));
                self->send_json(http::status::ok, make_ok());
            });
    }

    // BE-GROUP-02：退群
    void do_quit_group(std::uint64_t uid, std::uint64_t groupId) {
        auto self = shared_from_this();
        db_.quitGroup(groupId, uid,
            [self, uid, groupId](bool ok, const std::string& err, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // res.members 是“退群后剩余成员”。
                self->push_event_many(member_ids(res.members), MsgType::GroupMemberChanged,
                                      uid, 0,
                                      serialize_group_member_changed("quit", groupId, uid, 0, uid));
                self->send_json(http::status::ok, make_ok());
            });
    }

    // BE-GROUP-02：转让群主
    void do_transfer_group(std::uint64_t uid, std::uint64_t groupId) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;
        if (!doc.HasMember("toUserId")) {
            send_json(http::status::bad_request, make_error("missing toUserId"));
            return;
        }
        std::uint64_t to = 0;
        if (!json_uint64(doc["toUserId"], to) || to == 0) {
            send_json(http::status::bad_request, make_error("invalid toUserId"));
            return;
        }

        auto self = shared_from_this();
        db_.transferGroup(groupId, uid, to,
            [self, uid, groupId, to](bool ok, const std::string& err, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // 转让：给全群推 transferred（user_id=新群主），再给旧群主本人补一条
                // roleChanged（他的角色从 1 变成 2）。
                self->push_event_many(member_ids(res.members), MsgType::GroupMemberChanged,
                    uid, 0, serialize_group_member_changed("transferred", groupId, to, 1, uid));
                self->push_event_many(member_ids(res.members), MsgType::GroupMemberChanged,
                    uid, 0, serialize_group_member_changed("roleChanged", groupId, uid, 2, uid));
                self->send_json(http::status::ok, make_group_ok(res.group));
            });
    }

    // BE-GROUP-03：邀请成员（群主/管理员）
    void do_invite_members(std::uint64_t uid, std::uint64_t groupId) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;
        std::vector<std::uint64_t> members;
        if (!doc.HasMember("memberIds") || !parse_id_array(doc["memberIds"], members)
            || members.empty()) {
            send_json(http::status::bad_request, make_error("invalid memberIds"));
            return;
        }

        auto self = shared_from_this();
        db_.inviteMembers(groupId, uid, members,
            [self, uid, groupId](bool ok, const std::string& err, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // 给全群成员（含新成员）广播：每个新增一人一条 added 事件。
                auto targets = member_ids(res.members);
                for (std::uint64_t addedUid : res.affected_ids) {
                    self->push_event_many(targets, MsgType::GroupMemberChanged, uid, 0,
                        serialize_group_member_changed("added", groupId, addedUid, 3, uid));
                }
                self->send_json(http::status::ok, make_group_detail_ok(res.group, res.members));
            });
    }

    // BE-GROUP-04：移出成员
    void do_remove_member(std::uint64_t uid, std::uint64_t groupId, std::uint64_t target) {
        auto self = shared_from_this();
        db_.removeMember(groupId, uid, target,
            [self, uid, groupId, target](bool ok, const std::string& err, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // res.members 是移除后的剩余成员（不含 target）；单独给 target 也推一条，
                // 让被移者立即刷新群列表。
                self->push_event_many(member_ids(res.members), MsgType::GroupMemberChanged,
                    uid, 0, serialize_group_member_changed("removed", groupId, target, 0, uid));
                self->push_event(target, MsgType::GroupMemberChanged, uid, 0,
                    serialize_group_member_changed("removed", groupId, target, 0, uid));
                self->send_json(http::status::ok, make_ok());
            });
    }

    // BE-GROUP-04：设置/取消管理员（仅群主）
    void do_set_admin(std::uint64_t uid, std::uint64_t groupId) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;
        std::uint64_t target = 0;
        if (!doc.HasMember("userId") || !json_uint64(doc["userId"], target) || target == 0) {
            send_json(http::status::bad_request, make_error("invalid userId"));
            return;
        }
        bool admin = true;
        if (doc.HasMember("admin") && doc["admin"].IsBool()) admin = doc["admin"].GetBool();

        auto self = shared_from_this();
        db_.setAdmin(groupId, uid, target, admin,
            [self, uid, groupId, target, admin](bool ok, const std::string& err,
                                                GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->push_event_many(member_ids(res.members), MsgType::GroupMemberChanged,
                    uid, 0,
                    serialize_group_member_changed("roleChanged", groupId, target,
                                                   admin ? 2 : 3, uid));
                self->send_json(http::status::ok, make_ok());
            });
    }

    // BE-GROUP-03：申请入群
    void do_create_group_request(std::uint64_t uid) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;
        if (!doc.HasMember("groupId")) {
            send_json(http::status::bad_request, make_error("missing groupId"));
            return;
        }
        std::uint64_t groupId = 0;
        if (!json_uint64(doc["groupId"], groupId) || groupId == 0) {
            send_json(http::status::bad_request, make_error("invalid groupId"));
            return;
        }
        std::string message;
        if (doc.HasMember("message") && doc["message"].IsString())
            message = doc["message"].GetString();
        if (message.size() > 255) {
            send_json(http::status::bad_request, make_error("message too long"));
            return;
        }

        auto self = shared_from_this();
        db_.createGroupRequest(groupId, uid, message,
            [self, uid](bool ok, const std::string& err, GroupRequestInfo r) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // 实时推给群主；管理员靠登录补推/GET 列表获取。群主离线则不丢，
                // 因为申请已落库，status=0 会在他登录时被补推。
                self->push_event(r.owner_id, MsgType::GroupRequest, uid, r.id,
                                 serialize_group_request_event(r, "new"));
                self->send_json(http::status::ok, make_group_request_ok(r));
            });
    }

    // BE-GROUP-03：审批入群申请（通过/拒绝）
    void do_handle_group_request(std::uint64_t uid, std::uint64_t requestId, bool accept) {
        auto self = shared_from_this();
        db_.handleGroupRequest(requestId, uid, accept,
            [self, uid, accept](bool ok, const std::string& err,
                                GroupRequestInfo r, GroupOpResult res) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                // 结果推给申请人。
                const bool delivered = self->push_event(
                    r.requester_id, MsgType::GroupRequestResult, uid, r.id,
                    serialize_group_request_result_event(r, accept ? "accepted" : "rejected"));
                if (delivered) {
                    self->db_.markGroupRequestNotified(r.id,
                        [](bool ok2, const std::string& err2) {
                            if (!ok2)
                                std::cerr << "[group] mark notified failed: "
                                          << err2 << std::endl;
                        });
                }
                // 通过则申请人已成为成员：给全群补一条 added 事件。
                if (accept) {
                    self->push_event_many(member_ids(res.members), MsgType::GroupMemberChanged,
                        uid, 0,
                        serialize_group_member_changed("added", r.group_id, r.requester_id, 3, uid));
                }
                self->send_json(http::status::ok, make_group_request_ok(r));
            });
    }

    // BE-GROUP-03：入群申请列表（direction=incoming|outgoing|all）
    void do_list_group_requests(std::uint64_t uid, const std::string& query) {
        std::string direction = query_value(query, "direction");
        if (direction.empty()) direction = "incoming";
        if (direction != "incoming" && direction != "outgoing" && direction != "all") {
            send_json(http::status::bad_request, make_error("invalid direction"));
            return;
        }
        auto self = shared_from_this();
        db_.listGroupRequests(uid, direction,
            [self](bool ok, const std::string& err, std::vector<GroupRequestInfo> reqs) {
                if (!ok) {
                    self->send_json(http::status::bad_request, make_error(err));
                    return;
                }
                self->send_json(http::status::ok, make_group_requests_ok(reqs));
            });
    }
    /*************群聊功能：HTTP 处理器结束***************/

    // -----------------------------------------------------------------------
    // 好友事件推送
    // -----------------------------------------------------------------------
    // 只在对方“已经 TCP 登录”时才能推送；对方离线时丢弃事件没关系，
    // 因为事实已经落库，登录时会有补偿（待处理申请 / 未通知结果）。
    // 返回值表示“对方是否在线且事件已入写队列”。
    bool push_event(std::uint64_t to, MsgType type, std::uint64_t from,
                    std::uint64_t eventId, std::string content) {
        auto peer = sessions_.find(to);
        if (!peer) return false;

        Message m;
        m.type    = type;
        m.from    = from;
        m.to      = to;
        m.msg_id  = eventId;      // 申请事件里复用 msg_id 携带 requestId
        m.ts      = now_ms();
        m.content = std::move(content);
        peer->deliver(m);
        return true;
    }

    /*************群聊功能：向一批成员广播同一事件（去重后逐个投递）***************/
    // 群事件（成员变化/资料变更/解散）通常需要推给全群。这里对 userId 去重，
    // 离线成员自动跳过（sessions_.find 返回空），不影响事实一致性：
    // 客户端登录后调用 GET /api/groups 拉全量对齐即可。
    void push_event_many(const std::vector<std::uint64_t>& to, MsgType type,
                         std::uint64_t from, std::uint64_t eventId,
                         const std::string& content) {
        std::vector<std::uint64_t> uniq = to;
        std::sort(uniq.begin(), uniq.end());
        uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
        for (std::uint64_t uid : uniq) push_event(uid, type, from, eventId, content);
    }

    // 便捷：从成员列表取 userId，可排除某个用户（如不给操作者自己重复推）。
    static std::vector<std::uint64_t> member_ids(const std::vector<GroupMemberInfo>& ms,
                                                 std::uint64_t exclude = 0) {
        std::vector<std::uint64_t> ids;
        ids.reserve(ms.size());
        for (const auto& m : ms) if (m.user_id != exclude) ids.push_back(m.user_id);
        return ids;
    }
    /*************群聊功能：批量事件推送辅助***************/

    // 发送 JSON 响应，随后关闭连接（短连接）。
    void send_json(http::status status, std::string body) {
        auto self = shared_from_this();
        auto res = std::make_shared<http::response<http::string_body>>(
            status, req_.version());
        res->set(http::field::server, "KC-CXIM");
        res->set(http::field::content_type, "application/json; charset=utf-8");
        res->keep_alive(false);
        res->body() = std::move(body);
        res->prepare_payload();

        http::async_write(socket_, *res,
            [self, res](beast::error_code ec, std::size_t /*bytes*/) {
                beast::error_code ignored;
                self->socket_.shutdown(tcp::socket::shutdown_send, ignored);
                self->socket_.close(ignored);
            });
    }

    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    tcp::socket socket_;
    Db&             db_;
    TokenService&   tokens_;
    SessionManager& sessions_;
};

} // namespace

// ---------------------------------------------------------------------------
// http_server
// ---------------------------------------------------------------------------
http_server::http_server(boost::asio::io_context& io, unsigned short port,
                         Db& db, TokenService& tokens, SessionManager& sessions)
    : acceptor_(io, tcp::endpoint(tcp::v4(), port)),
      db_(db),
      tokens_(tokens),
      sessions_(sessions) {
    do_accept();
}

void http_server::run() {
    std::cout << "[http] listening on port "
              << acceptor_.local_endpoint().port() << std::endl;
}

void http_server::do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<http_session>(std::move(socket), db_, tokens_,
                                               sessions_)->run();
            }
            if (acceptor_.is_open()) do_accept();
        });
}

} // namespace kc
