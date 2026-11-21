#include "http_server.h"                       // 本模块声明

#include "db.h"                                // Db：把重活投递到 worker 线程
#include "friend_types.h"                      // 好友事件 JSON 序列化
#include "group_types.h"                       // /*************群聊功能：群领域模型与事件序列化***************/
#include "session.h"                           // SessionManager::find 返回的 session::deliver
#include "session_manager.h"                   // 按 userId 查找在线 TCP 连接
#include "token_service.h"                     // TokenService：HTTP 接口鉴权
/*************图片文件功能（七牛云直传）：控制面所需的模块***************/
#include "file_types.h"                        // FileRuntime/uuid/元数据/白名单
#include "object_store.h"                      // ObjectStorage（mock/七牛）+ MockObjectStore
#include "qiniu_signer.h"                      // mock-object 校验需要复用签名实现
#include <cstdio>                              // std::snprintf：object_key 的日期段
#include <ctime>                               // gmtime：object_key 的 yyyy-mm 前缀
/*************图片文件功能（七牛云直传）：控制面所需的模块***************/

#include <boost/beast/core.hpp>                // Beast 核心：flat_buffer / error_code 等
#include <boost/beast/http.hpp>                // Beast HTTP：request/response/async_read/write
#include <boost/beast/version.hpp>             // Beast 版本号（用于 Server 头）
#include <boost/asio/steady_timer.hpp>         // P1-6：keep-alive 空闲计时器

#include "rapidjson/document.h"                // 解析请求 JSON
#include "rapidjson/stringbuffer.h"            // 生成响应 JSON 的缓冲区
#include "rapidjson/writer.h"                  // 把 JSON 写进缓冲区

#include <cctype>                              // isdigit / isxdigit
#include <algorithm>                           // /*************群聊功能：std::sort/std::unique 事件扇出去重***************/
#include <atomic>                              // P0-4：HTTP 在途请求计数
#include <chrono>                              // 事件时间戳 / 令牌桶计时
#include <cstdlib>                             // P0-4：getenv 读取过载阈值
#include <iostream>                            // 打印监听/错误日志
#include <memory>                              // std::shared_ptr 管理连接生命周期
#include <string>                              // std::string
#include <unordered_map>                       // P0-4：按 IP 的限流桶
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

/*************P0-3：登录/刷新统一返回 access + refresh 两个 token***************/
// token          : access token（原字段名保持不变，老客户端继续能用）
// refreshToken   : 只用于 POST /api/token/refresh，不能当 Bearer 用
// expiresIn      : access 有效秒数，客户端据此决定何时提前刷新
// refreshExpiresIn : refresh 有效秒数
// 老客户端忽略这三个新增字段即可，接口向后兼容。
std::string make_token_ok(std::uint64_t userId,
                          const std::string& username,
                          const std::string& token,
                          const std::string& refreshToken,
                          long long expiresIn,
                          long long refreshExpiresIn) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("userId");   w.Uint64(userId);
    w.Key("username"); w.String(username.c_str(), username.size());
    w.Key("token");    w.String(token.c_str(), token.size());
    w.Key("refreshToken");   w.String(refreshToken.c_str(), refreshToken.size());
    w.Key("expiresIn");      w.Int64(expiresIn);
    w.Key("refreshExpiresIn"); w.Int64(refreshExpiresIn);
    w.Key("tokenType");      w.String("Bearer");
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

// 兼容保留：只做注册用的 userId 响应见 make_register_ok。
/*************P0-3：登录/刷新统一返回 access + refresh 两个 token***************/

/*************P1-7：批量接口的响应（创建申请 / 删除好友）***************/
// created  成功建立/处理的条目；
// skipped  被跳过的目标 + 原因，客户端可据此单独提示，而不必整批失败。
std::string make_requests_created_ok(const std::vector<FriendRequestInfo>& created,
                                     const std::vector<std::pair<std::uint64_t, std::string>>& skipped) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("requests"); w.StartArray();
    for (const auto& r : created) {
        w.StartObject();
        w.Key("requestId");    w.Uint64(r.id);
        w.Key("requesterId");  w.Uint64(r.requester_id);
        w.Key("targetId");     w.Uint64(r.target_id);
        w.Key("targetUsername"); w.String(r.target_username.c_str(), r.target_username.size());
        w.Key("targetNickname"); w.String(r.target_nickname.c_str(), r.target_nickname.size());
        w.Key("message");      w.String(r.message.c_str(), r.message.size());
        w.Key("status");       w.Int(r.status);
        w.Key("createdAt");    w.Uint64(r.created_ts);
        w.EndObject();
    }
    w.EndArray();
    w.Key("skipped"); w.StartArray();
    for (const auto& s : skipped) {
        w.StartObject();
        w.Key("toUserId"); w.Uint64(s.first);
        w.Key("reason");   w.String(s.second.c_str(), s.second.size());
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_deleted_ok(std::size_t requested, std::size_t deleted) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    /*************core-dump 修复 ⑨：括号必须配平***************/
    // 这一版多写了**一个** EndObject（两个 StartObject、三个 EndObject）。
    // RapidJSON 的 Writer 只在 RAPIDJSON_ASSERT(level_stack_.GetSize() >= sizeof(Level))
    // 上兜底，而 Release/-DNDEBUG 下这个断言是空的：
    //   * Debug / RelWithDebInfo → level_stack_ 下溢 → assert 失败 → **SIGABRT(134)**，
    //     正好是 fixme.md 判读表里"崩溃，需要 core"的另一种退出码；
    //   * Release → 少弹一层，吐出 {"...":{"requested":N,"deleted":M}}} 这种
    //     多一个右花括号的非法 JSON，客户端 json 解析直接失败。
    // 触发条件：POST /api/friends/batch-delete 成功返回。
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("requested"); w.Uint64(requested);
    w.Key("deleted");   w.Uint64(deleted);
    w.EndObject();      // data
    w.EndObject();      // 外层：与上面两个 StartObject 一一对应
    /*************core-dump 修复 ⑨：括号必须配平***************/
    return std::string(sb.GetString(), sb.GetSize());
}
/*************P1-7：批量接口的响应***************/

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

// ============================================================================
// /*************图片文件功能（七牛云直传）：文件接口的响应构造***************/
// ----------------------------------------------------------------------------
// 响应里**只出现**：file_id / key / 上传地址 / uptoken / ticket / 换取 URL。
// 绝不出现：七牛 AK/SK、ticket 密钥、真实 bucket 内部路径之外的任何秘密。
// uptoken 本身只授权"往这一个 key 写不超过 fsizeLimit 的一次对象"，
// 泄漏后果被 policy 限死——这是"凭证下发给客户端"安全性的全部依据（规划约束原文）。
// ============================================================================
std::string make_file_token_ok(const std::string& fileId, const std::string& key,
                               const std::string& uploadUrl, const std::string& uptoken,
                               long long expiresIn, const std::string& ticket) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("fileId");    w.String(fileId.c_str(), fileId.size());
    w.Key("key");       w.String(key.c_str(), key.size());
    w.Key("uploadUrl"); w.String(uploadUrl.c_str(), uploadUrl.size());
    w.Key("uptoken");   w.String(uptoken.c_str(), uptoken.size());
    w.Key("expiresIn"); w.Int64(expiresIn);
    w.Key("ticket");    w.String(ticket.c_str(), ticket.size());
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

std::string make_file_commit_ok(const std::string& fileId, const std::string& url,
                                long long size, const std::string& qetag) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("code");    w.Int(0);
    w.Key("message"); w.String("ok");
    w.Key("data");    w.StartObject();
    w.Key("fileId"); w.String(fileId.c_str(), fileId.size());
    w.Key("url");    w.String(url.c_str(), url.size());
    w.Key("size");   w.Int64(size);
    w.Key("qetag");  w.String(qetag.c_str(), qetag.size());
    w.EndObject();
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

// 解析 /api/files/<uuid>（GET 下载 / DELETE 删除共用）。
// uuid 形状由 isUuidLike 把关（36 位 [0-9a-f-]）：形状不对的 path 一律
// 匹配失败，路由会继续往下落到 404——不给路径注入留缝隙。
bool parse_file_uuid(const std::string& path, const std::string& prefix, std::string& uuidOut) {
    if (path.size() != prefix.size() + 36) return false;
    if (path.compare(0, prefix.size(), prefix) != 0) return false;
    uuidOut = path.substr(prefix.size());
    return isUuidLike(uuidOut);
}

// object_key 的日期段：UTC yyyy-mm。目录按月分片，
// 单目录对象数可控（七牛单 bucket 对象数无硬上限，但运维扫描/清理按前缀更快）。
std::string yyyymmUtc() {
    char buf[8];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);          // MSVC：gmtime 非线程安全，必须用 _s 版本
#else
    gmtime_r(&t, &tmv);          // POSIX：线程安全版本
#endif
    std::snprintf(buf, sizeof(buf), "%04d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1);
    return buf;
}
/*************图片文件功能（七牛云直传）：文件接口的响应构造结束***************/

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

// ============================================================================
// /*************P0-4：入口层过载保护（限流 / 在途上限 / 请求体上限）***************/
// 为什么要放在 HTTP 入口而不是 DB 队列里？
//   DB 任务队列被打满时，DAO 的回调签名各不相同，无法统一回一个"忙"错误；
//   而在入口挡住，既能给出语义正确的 429/503 + Retry-After，又能在客户端超时
//   之前就把压力卸掉。注册风暴的真实伤害是"把所有人的登录都拖成十几秒"，
//   所以必须优先保证已经在跑的会话可用，而不是硬吞下所有新请求。
//
// 环境变量（未设置则用默认值）：
//   HTTP_MAX_INFLIGHT  同时在途的 HTTP 请求数上限（默认 512），超过回 503
//   HTTP_BODY_MAX      请求体字节上限（默认 64KB），超过回 413
//   AUTH_RATE_BURST    认证类接口全局限流桶容量（默认 40）
//   AUTH_RATE_PER_SEC  认证类接口全局每秒令牌（默认 20）
//                      —— 2 核 + cost=10 的容量约 28 次哈希/秒，取 ~70% 留余量
//   AUTH_IP_PER_SEC    单 IP 每秒令牌（默认 30）
//
// 每 IP 阈值必须**宽于**全局阈值，这是两个桶的分工决定的：
//   * 全局桶 = 机器容量闸门（保护那 2 个核），真正会bind 的是它；
//   * 每 IP 桶 = 反滥用（挡单机刷注册/密码爆破），只应该在明显异常时才bind。
// 反过来配（例如全局 20/s、每 IP 5/s）会让每 IP 桶变成实际限制：
// 办公室/CGNAT/移动基站共享出口 IP 的一整批真实用户会互相拖死，
// 而单机压测（一台机器发 100 个用户的注册+登录 = 202 次认证）也会在服务器
// 明明空闲的情况下被大面积 429 —— 那种情况下 429 是我们在拒绝自己的验收测试。
//
// 关于 X-Forwarded-For：本服务当前是**直接暴露端口**（DBConfig/压测都走公网 IP），
// 所以取 socket 对端地址就是权威来源。若将来挂到 nginx/CLB 后面，必须改成
// 只信任来自可信代理的 XFF（否则客户端自己塞一个 XFF 就能绕过每 IP 限制），
// 这是安全配置，不是可以顺手加的代码。
// ============================================================================
namespace {

struct OverloadConfig {
    std::size_t maxInflight = 512;
    std::size_t bodyMax     = 64 * 1024;
    double      authBurst   = 40;
    double      authPerSec  = 20;
    double      ipBurst     = 120;
    double      ipPerSec    = 30;
    std::size_t ipTableMax  = 8192;
};

const OverloadConfig& overloadConfig() {
    static const OverloadConfig cfg = [] {
        OverloadConfig c;
        auto envNum = [](const char* name, double def) -> double {
            const char* v = std::getenv(name);
            if (!v || !*v) return def;
            try { double d = std::stod(v); return d > 0 ? d : def; } catch (...) { return def; }
        };
        c.maxInflight = static_cast<std::size_t>(envNum("HTTP_MAX_INFLIGHT", c.maxInflight));
        c.bodyMax     = static_cast<std::size_t>(envNum("HTTP_BODY_MAX",     c.bodyMax));
        c.authBurst   = envNum("AUTH_RATE_BURST",   c.authBurst);
        c.authPerSec  = envNum("AUTH_RATE_PER_SEC", c.authPerSec);
        c.ipPerSec    = envNum("AUTH_IP_PER_SEC",   c.ipPerSec);
        c.ipBurst     = c.ipPerSec * 2;
        return c;
    }();
    return cfg;
}

// /*************P0-4：全局在途"响应"计数***************/
// 这里刻意数的是**正在处理的请求**（进 send_json +1，写完 -1），
// 不是"打开着的连接数"。曾经写成 http_session 构造 +1 / 析构 -1，看起来等价，
// 实际差得很远：对象由 keep-alive 空闲计时器持有，一条连接回完响应后还会活到
// 60s 计时到期，于是这个计数变成"最近 60 秒内开过的所有连接"。
// 结果就是自己造出 503 风暴：注册风暴里每条 register/login 都是一个新连接，
// 几百个连接在几秒内把计数顶过 512，之后**每个新连接在解析第一个请求之前**
// 就被 503 关掉 —— 服务明明空闲，客户端却全被拒。
// 用"响应"做单位就自洽了：一个请求最多一个响应，加减天然成对，也不受连接寿命影响。
std::atomic<std::size_t>& inflightCount() {
    static std::atomic<std::size_t> n{0};
    return n;
}
/*************P0-4：全局在途"响应"计数***************/

// 令牌桶：capacity 是桶容量（允许的突发量），perSec 是补充速率。
// 需要在外部持锁的情况下调用（见 RateLimiter::allow）。
class TokenBucket {
public:
    TokenBucket(double capacity, double perSec)
        : capacity_(capacity), tokens_(capacity), perSec_(perSec),
          last_(std::chrono::steady_clock::now()) {}

    bool take() {
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(tokens_ + dt * perSec_, capacity_);
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

    // 距离能取到 1 个令牌还要等多久（秒，向上取整，最小 1）——写进 Retry-After。
    int secondsUntilNext() const {
        if (perSec_ <= 0.0) return 1;
        const int sec = static_cast<int>((1.0 - tokens_) / perSec_) + 1;
        return sec < 1 ? 1 : sec;
    }

private:
    double capacity_;
    double tokens_;
    double perSec_;
    std::chrono::steady_clock::time_point last_;
};

// 认证类接口（注册 / 登录 / 刷新）的限流：全局桶 + 每 IP 桶。
class RateLimiter {
public:
    explicit RateLimiter(const OverloadConfig& c)
        : global_(c.authBurst, c.authPerSec),
          ipBurst_(c.ipBurst), ipPerSec_(c.ipPerSec), ipMax_(c.ipTableMax) {}

    // 返回 0 表示放行；否则返回建议等待秒数（写进 Retry-After）。
    int allow(const std::string& ip) {
        std::lock_guard<std::mutex> lk(mu_);

        // 先看全局桶：全局容量见底时所有来源一起退避。
        // 此时不去动每 IP 的账，否则一个来源的超额会消耗掉别的来源的额度。
        if (!global_.take()) return global_.secondsUntilNext();

        if (ip.empty()) return 0;      // 拿不到对端地址（极端情况）：只受全局限流约束
        auto it = perIp_.find(ip);
        if (it == perIp_.end()) {
            // 表太大就整表清一次：这是过载保护，不是精确计费，
            // 宁可让少数 IP 多拿一轮额度，也不让这张表无界增长。
            if (perIp_.size() >= ipMax_) perIp_.clear();
            it = perIp_.emplace(ip, TokenBucket(ipBurst_, ipPerSec_)).first;
        }
        if (!it->second.take()) return it->second.secondsUntilNext();
        return 0;
    }

private:
    std::mutex                                    mu_;
    TokenBucket                                   global_;
    std::unordered_map<std::string, TokenBucket>  perIp_;
    double                                        ipBurst_;
    double                                        ipPerSec_;
    std::size_t                                   ipMax_;
};

RateLimiter& authLimiter() {
    static RateLimiter limiter(overloadConfig());
    return limiter;
}

// Db 回 kBusyError 时统一映射到 503 + Retry-After（P0-4）。
bool isBusyError(const std::string& err) {
    return err == Db::kBusyError;
}

} // namespace
// ============================================================================
// /*************P0-4：入口层过载保护***************/

// ---------------------------------------------------------------------------
// http_session：一条 HTTP 连接（P1-6：支持 keep-alive 复用）
// ---------------------------------------------------------------------------
// 路由分三类：
//   * 公开接口：/api/register、/api/login、/api/token/refresh；
//   * 需要 JWT 的接口：搜索用户、好友申请（含批量）、好友列表、好友维护；
//   * 群聊接口：见后面的 do_group_* 。
// 所有耗时操作（SQL、bcrypt）都交给 Db 的两个 worker 池；
// 本类只在 io_context 线程里解析 JSON、入队、回写响应和推送好友事件。
//
// /*************P1-6：为什么改成短连接 -> keep-alive***************/
// 原来每个响应都 keep_alive(false) 并由服务端 shutdown/close：
//   * 注册风暴时每秒几十请求会在服务端留下几千个 TIME_WAIT（44 req/s × 60s），
//     fd 与端口都在churn，500 并发时报 connect 失败(10060) 有它一份；
//   * 跨公网每个请求都要重新握手 + 慢启动，光 RTT 就给每个 auth 请求加几毫秒到
//     几十毫秒。
// 现在：Content-Length 未超限就继续在同一连接上读下一个请求，配合
//   * 空闲计时（keep-alive 60s 无新请求就关）；
//   * 单连接请求数上限（防止一条连接无限占着一个 session 对象）；
//   * 请求体上限（Beast 默认对 request 是 1MB，这里收紧到 64KB）。
// 客户端显式发 Connection: close 时行为与以前一致。
// ---------------------------------------------------------------------------
class http_session : public std::enable_shared_from_this<http_session> {
public:
    http_session(tcp::socket socket, Db& db, TokenService& tokens,
                 SessionManager& sessions,
                 std::shared_ptr<ObjectStorage> store, FileRuntime fileRt)
        : socket_(std::move(socket)),
          /*************core-dump 修复 ⑤：HTTP 连接也要有自己的 strand***************/
          // TCP 侧每条连接一个 strand（session.h:strand_），HTTP 侧一直没有：
          // 所有 handler 直接挂在 io_context 上，而 io_context 由 2 个以上线程
          // run()。于是同一条 HTTP 连接的"读完成 / 写完成 / 空闲计时器 /
          // DB 回调里的 send_json"可能落在**不同线程上同时执行**，
          // 一起碰 socket_ / buffer_ / parser_ / req_ 这几个非原子成员。
          // Beast 明确要求"同一条流上同一方向同时只能有一个异步操作"，
          // 违反它的表现不是"偶发错响应"，而是随机 core dump。
          // 现在与 TCP 侧对齐：本连接的一切状态变化都串行在自己的 strand 上。
          strand_(net::make_strand(socket_.get_executor())),
          idle_timer_(strand_),
          /*************core-dump 修复 ⑤：HTTP 连接也要有自己的 strand***************/
          db_(db),
          tokens_(tokens),
          sessions_(sessions),
          /*************图片文件功能（七牛云直传）：存储抽象与运行期参数注入***************/
          store_(std::move(store)),
          frt_(fileRt) {}
    // 注意：**不要**在这里给 inflight 计数 +1/-1。连接寿命由 keep-alive 空闲计时器
    // 决定（最长 60s），拿它当"在途请求数"会严重高估，见 inflightCount() 的说明。

    void run() {
        // P1-5：IM/REST 都是小报文请求-响应，关掉 Nagle 避免与对端延迟 ACK
        // 互相等出 40~200ms 的停顿。
        boost::system::error_code ec;
        socket_.set_option(net::ip::tcp::no_delay(true), ec);

        /*************core-dump 修复 ⑤：入口也切进 strand（与 session::start 一致）***************/
        auto self = shared_from_this();
        net::post(strand_, [self] { self->do_read(); });
    }

private:
    // 读下一个请求的头+体（parser 每次新建，天然支持 keep-alive 的顺序解析）。
    void do_read() {
        /*************P0-4：入口准入 = 在途响应数 + DB 队列深度***************/
        // 只看"在途响应数"是不够的：仍有一批 DAO 走软上限投递口（不丢任务、也不回
        // kBusyError，见 db.h:submit），它们可以把 sqlQueue_ 堆得比 queueLimit 还深，
        // 这时热路径会开始 busy 拒绝，而新请求还在源源进来。
        // 所以准入判断同时看队列深度：**在把新任务塞进队列之前**就拒，
        // 而不是等队列已经深了再让每个请求各自撞墙。
        if (inflightCount().load(std::memory_order_relaxed) >
                overloadConfig().maxInflight ||
            db_.sqlQueueDepth() > overloadConfig().maxInflight) {
            send_json(http::status::service_unavailable,
                      make_error(Db::kBusyError), 2, /*closeAfter=*/true);
            return;
        }
        /*************P0-4：入口准入 = 在途响应数 + DB 队列深度***************/

        parser_ = std::make_unique<http::request_parser<http::string_body>>();
        parser_->body_limit(overloadConfig().bodyMax);   // P0-4：请求体上限

        // 空闲计时：每次开始读下一个请求时重设一次 deadline。
        // expires_after 会自动以 operation_aborted 取消上一个还在等的 async_wait，
        // 所以这里再挂一个新的等待即可，全程最多只有一个待触发的定时回调。
        idle_timer_.expires_after(std::chrono::seconds(kKeepAliveIdleSec));
        {
            /*************core-dump 修复 ⑤：timer 回调也进 strand***************/
            auto self = shared_from_this();
            idle_timer_.async_wait(net::bind_executor(strand_,
                [self](beast::error_code ec) {
                    if (ec) return;             // 被下一次读请求取消，正常路径
                    self->close_socket();       // 60s 没有新请求：回收这条 keep-alive 连接
                }));
        }

        auto self = shared_from_this();
        http::async_read(socket_, buffer_, *parser_,
            net::bind_executor(strand_,
            [self](beast::error_code ec, std::size_t /*bytes*/) {
                if (ec) {                           // 读失败/超限/对端关闭
                    if (ec == http::error::body_limit) {
                        self->send_json(http::status::payload_too_large,
                                        make_error("request body too large"),
                                        0, true);
                        return;
                    }
                    self->close_socket();
                    return;
                }
                self->req_ = self->parser_->release();
                ++self->requestsHandled_;
                self->handle_request();
            }));
    }

    /*************关闭连接：先取消空闲计时器，再只关发送方向***************/
    // 1) 必须 cancel idle_timer_：那个 async_wait 的回调持有 self（shared_ptr），
    //    不取消的话对象会一直活到 60s 到期，keep-alive 连接在"已经关闭"之后
    //    仍然占着内存与一个定时器条目，也让任何按连接寿命统计的指标虚高。
    //    TCP 侧的 do_close() 一直是这么做的（session.cpp: idle_timer_.cancel()）。
    // 2) 用 shutdown_send 而不是 shutdown_both：请求体超限（413）等路径是在
    //    **还有没读完的字节**时关闭的，双向 shutdown + close 会让内核发 RST，
    //    而 RST 会丢弃对端尚未读取的数据——包括我们刚写出去的那份 413 响应，
    //    客户端只看到一个被重置的连接。只关发送方向 = 正常 FIN，
    //    响应能被读完，剩余上行数据由内核丢弃。
    void close_socket() {
        fileCrumb("close.begin");
        boost::system::error_code ignored;
        idle_timer_.cancel();
        socket_.shutdown(tcp::socket::shutdown_send, ignored);
        socket_.close(ignored);
        fileCrumb("close.end");
    }
    /*************关闭连接：先取消空闲计时器，再只关发送方向***************/

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
            if (!check_auth_rate()) return;              // P0-4：注册风暴先限流
            std::string username, password;
            if (!parse_credentials(username, password)) return;
            do_register(username, password);
            return;
        }
        if (path == "/api/login") {
            if (method != http::verb::post) return method_not_allowed();
            if (!check_auth_rate()) return;              // P0-4：登录风暴先限流
            std::string username, password;
            if (!parse_credentials(username, password)) return;
            do_login(username, password);
            return;
        }
        /*************P0-3：刷新接口——重连不再触发 bcrypt***************/
        // 只做一次 HMAC-SHA256 验签（微秒级），既不查库也不算哈希，
        // 因此"100 个客户端同时重连"在服务端的花费几乎是零。
        if (path == "/api/token/refresh") {
            if (method != http::verb::post) return method_not_allowed();
            if (!check_auth_rate()) return;              // 仍是认证类接口，纳入同一桶
            do_refresh_token();
            return;
        }
        /*************P0-3：刷新接口——重连不再触发 bcrypt***************/

        // ====================================================================
        // /*************图片文件功能（七牛云直传）：公开区（无 Bearer 的入口）***************/
        // --------------------------------------------------------------------
        // 三条路由在统一鉴权之前处理：
        //   1) GET /api/files/{uuid}?e=&vtoken=  —— <img>/浏览器标签带不了
        //      Authorization 头，改用服务端签发的短时效签名入口（规划里
        //      "302 + 私有空间签名 URL"难题的解法）。有 Authorization 时仍走 Bearer。
        //   2) GET /api/files/mock-object/{key}  —— 仅 mock 模式：模拟七牛的
        //      私有下载端点（同样靠签名 URL 访问，语义对齐真实模式）。
        // 真实模式下访问任何 mock-* 路由一律 404：不给生产环境留旁路。
        // ====================================================================
        {
            std::string fuuid;
            // 2) mock-object：签名 URL 直取模拟字节（仅 mock 模式开放）。
            static const std::string kMockObjPrefix = "/api/files/mock-object/";
            if (method == http::verb::get &&
                path.compare(0, kMockObjPrefix.size(), kMockObjPrefix) == 0 &&
                path.size() > kMockObjPrefix.size()) {
                if (!store_->isMock()) {
                    send_json(http::status::not_found, make_error("mock endpoints disabled"));
                    return;
                }
                do_mock_object_download(path, query);
                return;
            }
            // 1) 下载换取入口：Bearer 或签名参数二选一。
            if (method == http::verb::get && parse_file_uuid(path, "/api/files/", fuuid)) {
                if (req_.count(http::field::authorization)) {
                    std::uint64_t duid = 0;
                    std::string dusername;
                    if (!authenticate(duid, dusername)) return;   // 失败已回 401
                    do_file_download(fuuid);
                    return;
                }
                const std::string eStr  = query_value(query, "e");
                const std::string vtok  = query_value(query, "vtoken");
                if (!eStr.empty() && !vtok.empty() &&
                    store_->verifyEntryQuery(path, eStr, vtok)) {
                    do_file_download(fuuid);                      // 免 Bearer 的签名入口
                    return;
                }
                send_json(http::status::unauthorized,
                          make_error("missing authorization or valid signature"));
                return;
            }
        }
        // /*************图片文件功能（七牛云直传）：公开区结束***************/

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

        /*************P1-7：批量好友写接口（削峰、减少锁碰撞）***************/
        // 单条接口在"给 100 个人发申请"这种场景下会产生 100 个事务、100 次
        // 连接借还，并且都在同一个 requester 的索引区间上抢锁。批量接口把它压成
        // 一个事务、一次多行语句，同时把加锁顺序固定为按 id 升序（见 db.cpp）。
        if (path == "/api/friends/requests/batch") {
            if (method == http::verb::post) { do_create_request_batch(uid); return; }
            return method_not_allowed();
        }
        if (path == "/api/friends/batch-delete") {
            if (method == http::verb::post) { do_delete_friends_batch(uid); return; }
            return method_not_allowed();
        }
        /*************P1-7：批量好友写接口***************/

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

        // ====================================================================
        // /*************图片文件功能（七牛云直传）：控制面路由（Bearer 鉴权后）***************/
        // ====================================================================
        // 路由顺序注意：具体路径（upload-token/commit/mock-put）必须在
        // "{uuid}" 通配之前匹配——虽然 uuid 形状校验（parse_file_uuid）
        // 本就不会把这些词误认成 uuid，但具体在前、通配在后是这套 if 链
        // 的一贯纪律（群聊路由同理）。
        if (path == "/api/files/upload-token") {
            if (method != http::verb::post) return method_not_allowed();
            do_file_upload_token(uid);
            return;
        }
        if (path == "/api/files/commit") {
            if (method != http::verb::post) return method_not_allowed();
            do_file_commit(uid);
            return;
        }
        if (path == "/api/files/mock-put") {
            if (method != http::verb::post) return method_not_allowed();
            do_file_mock_put(uid);          // 真实模式下内部回 404
            return;
        }
        {
            std::string fuuid;
            if (parse_file_uuid(path, "/api/files/", fuuid)) {
                // GET 已在公开区处理（那里支持双鉴权）；到这里说明是其它方法。
                if (method == http::verb::delete_) {
                    do_file_delete(uid, fuuid);
                    return;
                }
                return method_not_allowed();
            }
        }
        // /*************图片文件功能（七牛云直传）：控制面路由结束***************/

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

    /*************P0-4：认证类接口限流***************/
    // 本机容量参考：2 核 + cost=10 ⇒ 约 28 次哈希/秒；默认全局 20/s 留 30% 余量，
    // 且这 20/s 里还要给 mariadbd 留算力。超过就回 429 + Retry-After，
    // 让客户端退避，而不是把所有人的登录一起拖成十几秒。
    std::string peer_ip() const {
        boost::system::error_code ec;
        auto ep = socket_.remote_endpoint(ec);
        if (ec) return {};
        return ep.address().to_string();
    }

    // 返回 true 表示放行；false 表示已经回过 429，调用方直接 return。
    bool check_auth_rate() {
        const int waitSec = authLimiter().allow(peer_ip());
        if (waitSec == 0) return true;
        send_json(http::status::too_many_requests,
                  make_error("too many auth requests"), waitSec, /*closeAfter=*/false);
        return false;
    }

    // Db 报"忙"时统一映射成 503 + Retry-After，而不是笼统的 400/500（P0-4）。
    void send_db_error(http::status fallback, const std::string& err) {
        if (isBusyError(err)) {
            send_json(http::status::service_unavailable, make_error(err), 2, false);
            return;
        }
        send_json(fallback, make_error(err));
    }
    /*************P0-4：认证类接口限流***************/

    // ---- 注册 / 登录 ------------------------------------------------------
    void do_register(const std::string& username, const std::string& password) {
        auto self = shared_from_this();
        db_.registerUser(username, password,
            [self](bool ok, const std::string& err, std::uint64_t userId) {
                if (!ok) {
                    self->send_db_error(http::status::bad_request, err);
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
                    // 账号/密码错误保持 401；只有"服务端忙"才换 503。
                    self->send_db_error(http::status::unauthorized, err);
                    return;
                }
                /*************P0-3：登录同时签发 access + refresh***************/
                // 之后客户端重连只需拿 refresh 换 access，不再触发 bcrypt 校验密码。
                const std::string token  = self->tokens_.sign(userId, name);
                const std::string refresh = self->tokens_.signRefresh(userId, name);
                self->send_json(http::status::ok, make_token_ok(
                    userId, name, token, refresh,
                    self->tokens_.accessExpireSeconds(),
                    self->tokens_.refreshExpireSeconds()));
                /*************P0-3：登录同时签发 access + refresh***************/
            });
    }

    /*************P0-3：POST /api/token/refresh***************/
    // 入参：{"refreshToken":"..."}
    // 成本：一次 HMAC-SHA256 验签 + 两次 HMAC 签发，全部在 io 线程完成，
    //       不查库、不算 bcrypt、不占用 Db 的两个 worker 池。
    // 输出：新的 access token + 新的 refresh token（滑动过期，刷新即轮换）。
    // 已知边界：refresh 是无状态的，服务端没有撤销表，因此"注销/改密后立即失效"
    //          需要后续接入 jti 黑名单（readme.md 的"安全边界"一节有说明）。
    void do_refresh_token() {
        rapidjson::Document doc;
        if (!read_json(doc)) return;

        if (!doc.HasMember("refreshToken") || !doc["refreshToken"].IsString()) {
            send_json(http::status::bad_request, make_error("missing refreshToken"));
            return;
        }
        const std::string refresh(doc["refreshToken"].GetString(),
                                  doc["refreshToken"].GetStringLength());

        std::uint64_t uid = 0;
        std::string username;
        if (!tokens_.verifyRefresh(refresh, uid, username)) {
            // 签名错/过期/类型不对（比如拿 access token 来刷新）统一 401。
            send_json(http::status::unauthorized, make_error("invalid or expired refresh token"));
            return;
        }

        const std::string token   = tokens_.sign(uid, username);
        const std::string rotated = tokens_.signRefresh(uid, username);
        send_json(http::status::ok, make_token_ok(
            uid, username, token, rotated,
            tokens_.accessExpireSeconds(),
            tokens_.refreshExpireSeconds()));
    }
    /*************P0-3：POST /api/token/refresh***************/

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
                    self->send_db_error(http::status::bad_request, err);
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
                    // 业务拒绝（已是好友、重复申请、被拉黑等）统一 400；
                    // "服务端忙"换成 503 + Retry-After（P0-4）。
                    self->send_db_error(http::status::bad_request, err);
                    return;
                }

                // 实时推给被申请人；对方离线则不推，等他登录时由
                // session::push_login_sync() 从 friend_requests 补推。
                self->push_event(targetId, MsgType::FriendRequest, uid, r.id,
                                 serialize_friend_request_event(r, "new"));

                self->send_json(http::status::ok, make_request_ok(r));
            });
    }

    /*************P1-7：批量创建好友申请***************/
    // 场景：用户从"可能认识的人"里一键加 50 个好友。用单条接口就是 50 个事务、
    // 50 次连接借还，还都在同一个 requester 的索引区间上抢锁；批量接口把它压成
    // 一个事务 + 一次多行 INSERT，并固定按目标 id 升序加锁。
    // 入参：{"toUserIds":[1,2,3], "message":"..."}（单次上限 kBatchMaxTargets）
    // 输出：data.requests（成功项） + data.skipped（被跳过项 + 原因）。
    void do_create_request_batch(std::uint64_t uid) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;

        if (!doc.HasMember("toUserIds") || !doc["toUserIds"].IsArray()) {
            send_json(http::status::bad_request, make_error("missing toUserIds"));
            return;
        }
        std::vector<std::uint64_t> targets;
        for (auto& v : doc["toUserIds"].GetArray()) {
            std::uint64_t t = 0;
            if (!json_uint64(v, t) || t == 0) continue;
            targets.push_back(t);
        }
        if (targets.empty()) {
            send_json(http::status::bad_request, make_error("toUserIds is empty"));
            return;
        }
        if (targets.size() > kBatchMaxTargets) {
            send_json(http::status::bad_request, make_error("too many toUserIds"));
            return;
        }

        std::string message;
        if (doc.HasMember("message") && doc["message"].IsString())
            message = doc["message"].GetString();
        /*************超长附言：拒绝，不要截断***************/
        // 曾经写成 message.resize(255)，看着像"与单条接口一致的宽松裁剪"，其实是两个坑：
        //   1) 255 是**字节**，中文附言会被从多字节字符中间切断，MariaDB 严格模式
        //      对非法 UTF-8 序列直接报错（1406/1300）；
        //   2) 批量只有一条多行 INSERT，于是第 1 步失败 = 整批 50 个人全部失败，
        //      而单条接口此时给的是干净的 400。
        // 结论：与单条接口一样直接拒绝，让调用方改请求，而不是让它随机炸整批。
        if (message.size() > 255) {
            send_json(http::status::bad_request, make_error("message too long"));
            return;
        }
        /*************超长附言：拒绝，不要截断***************/

        auto self = shared_from_this();
        db_.createFriendRequestsBatch(uid, std::move(targets), message,
            [self, uid](bool ok, const std::string& err,
                        std::vector<FriendRequestInfo> created,
                        std::vector<std::pair<std::uint64_t, std::string>> skipped) {
                if (!ok) {
                    self->send_db_error(http::status::bad_request, err);
                    return;
                }
                // 成功项逐条实时推给被申请人（离线由登录补推兜底）。
                for (const auto& r : created) {
                    self->push_event(r.target_id, MsgType::FriendRequest, uid, r.id,
                                     serialize_friend_request_event(r, "new"));
                }
                self->send_json(http::status::ok, make_requests_created_ok(created, skipped));
            });
    }

    // P1-7：批量删除好友。入参 {"friendIds":[...]}，一个事务内两条多行 DELETE。
    void do_delete_friends_batch(std::uint64_t uid) {
        rapidjson::Document doc;
        if (!read_json(doc)) return;

        if (!doc.HasMember("friendIds") || !doc["friendIds"].IsArray()) {
            send_json(http::status::bad_request, make_error("missing friendIds"));
            return;
        }
        std::vector<std::uint64_t> ids;
        for (auto& v : doc["friendIds"].GetArray()) {
            std::uint64_t f = 0;
            if (!json_uint64(v, f) || f == 0) continue;
            ids.push_back(f);
        }
        if (ids.empty()) {
            send_json(http::status::bad_request, make_error("friendIds is empty"));
            return;
        }
        if (ids.size() > kBatchMaxTargets) {
            send_json(http::status::bad_request, make_error("too many friendIds"));
            return;
        }

        auto self = shared_from_this();
        const std::size_t requested = ids.size();
        db_.deleteFriendsBatch(uid, std::move(ids),
            [self, uid, requested](bool ok, const std::string& err,
                                   std::vector<std::uint64_t> removed) {
                if (!ok) {
                    self->send_db_error(http::status::bad_request, err);
                    return;
                }
                /*************P1-7：批量删除也必须推事件***************/
                // 单条删除会向"我"和"对方"各推一条 FriendListChanged；
                // 批量如果少了这一步，被删的那 N 个人的 TCP 会话就完全不知道
                // 好友列表已经变了，要等下次重连/重新拉列表才发现——
                // 这恰恰是批量接口最容易被漏掉的行为差异。
                for (auto f : removed) {
                    self->push_event(uid, MsgType::FriendListChanged, uid, 0,
                                     serialize_friend_list_changed("deleted", f, "", 0));
                    self->push_event(f, MsgType::FriendListChanged, uid, 0,
                                     serialize_friend_list_changed("deleted", uid, "", 0));
                }
                // deleted 用"真正从我的好友列表消失的人数"，不是 DELETE 的 affected rows，
                // 也不是请求里给的 id 个数（里面可能有根本不是我好友的目标）。
                self->send_json(http::status::ok,
                                make_deleted_ok(requested, removed.size()));
            });
    }
    /*************P1-7：批量创建好友申请 / 批量删除好友***************/

    // ---- BE-FRIEND-04：通过 / 拒绝申请 -----------------------------------
    void do_handle_request(std::uint64_t uid, std::uint64_t requestId, bool accept) {
        auto self = shared_from_this();
        db_.handleFriendRequest(uid, requestId, accept,
            [self, uid, accept](bool ok, const std::string& err,
                                FriendRequestInfo r) {
                if (!ok) {
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
    // /*************图片文件功能（七牛云直传）：HTTP 处理器（控制面）***************/
    // ====================================================================
    // 五个处理器 + 两个发送辅助，全部是"小 JSON + 本地签名 + 外网/DB 入队"：
    // io 线程上没有任何字节搬运或阻塞外网调用（stat/rm 在 ObjectStorage
    // 自己的线程里跑，DB 在 Db worker 里跑，结果统一回 strand 再发响应）。
    // ====================================================================

    // 302 跳转响应：Location 指向有时效的七牛（或 mock）签名下载 URL。
    // 为什么必须是 302 而不是返回 JSON 让客户端二跳：
    // <img src> / <video src> 这类浏览器原生加载路径只能"一个 URL 跟到底"，
    // 不能解析 JSON 再跳——302 是唯一同时兼容 API 客户端和页面直链的形态。
    void send_redirect(const std::string& location, bool closeAfter = false) {
        // 与 send_json 同样的 strand 纪律：DB 回调可能来自任意 io 线程。
        if (!strand_.running_in_this_thread()) {
            auto self = shared_from_this();
            net::post(strand_, [self, location, closeAfter]() mutable {
                self->send_redirect(location, closeAfter);
            });
            return;
        }

        const bool wantKeepAlive =
            !closeAfter &&
            requestsHandled_ < kMaxRequestsPerConn &&
            req_.version() >= 11 &&
            req_.keep_alive();

        auto self = shared_from_this();
        auto res = std::make_shared<http::response<http::string_body>>(
            http::status::found, req_.version());
        res->set(http::field::server, "KC-CXIM");
        res->set(http::field::location, location);
        res->set(http::field::cache_control, "no-store");   // 签名 URL 有 deadline，绝不许缓存
        res->keep_alive(wantKeepAlive);
        res->prepare_payload();

        inflightCount().fetch_add(1, std::memory_order_relaxed);
        http::async_write(socket_, *res,
            net::bind_executor(strand_,
            [self, res, wantKeepAlive](beast::error_code ec, std::size_t) {
                inflightCount().fetch_sub(1, std::memory_order_relaxed);
                if (ec || !wantKeepAlive) { self->close_socket(); return; }
                self->do_read();
            }));
    }

    // mock-object 专用：返回合成字节 + 元信息头（Content-Type 固定 octet-stream，
    // 真实大小/校验和放在 X-Mock-Size / X-Mock-Qetag 头里，验收脚本据此核对）。
    void send_bytes(http::status status, const std::string& body,
                    const std::vector<std::pair<std::string, std::string>>& extraHeaders,
                    bool closeAfter = false) {
        if (!strand_.running_in_this_thread()) {
            auto self = shared_from_this();
            net::post(strand_, [self, status, body, extraHeaders, closeAfter]() mutable {
                self->send_bytes(status, body, extraHeaders, closeAfter);
            });
            return;
        }
        const bool wantKeepAlive =
            !closeAfter &&
            requestsHandled_ < kMaxRequestsPerConn &&
            req_.version() >= 11 &&
            req_.keep_alive();

        auto self = shared_from_this();
        auto res = std::make_shared<http::response<http::string_body>>(status, req_.version());
        res->set(http::field::server, "KC-CXIM");
        res->set(http::field::content_type, "application/octet-stream");
        for (const auto& h : extraHeaders) res->set(h.first, h.second);
        res->keep_alive(wantKeepAlive);
        res->body() = body;
        res->prepare_payload();

        inflightCount().fetch_add(1, std::memory_order_relaxed);
        http::async_write(socket_, *res,
            net::bind_executor(strand_,
            [self, res, wantKeepAlive](beast::error_code ec, std::size_t) {
                inflightCount().fetch_sub(1, std::memory_order_relaxed);
                if (ec || !wantKeepAlive) { self->close_socket(); return; }
                self->do_read();
            }));
    }

    // ---- POST /api/files/upload-token ------------------------------------
    // 签发上传凭证。校验链：白名单 → 单文件大小 → 日配额（DB 查当日已用）
    // → 服务端生成 uuid/object_key → 签 uptoken + 无状态 ticket。
    // 注意此步**不写 files 表**（规划红线）：没有 commit 的文件在业务上不存在。
    void do_file_upload_token(std::uint64_t uid) {
        fileCrumb("token.enter");
        rapidjson::Document doc;
        if (!read_json(doc)) return;

        // ---- 入参解析 ------------------------------------------------------
        if (!doc.HasMember("name") || !doc["name"].IsString()) {
            send_json(http::status::bad_request, make_error("missing name"));
            return;
        }
        std::string rawName(doc["name"].GetString(), doc["name"].GetStringLength());
        std::string mime;
        if (doc.HasMember("mime") && doc["mime"].IsString())
            mime.assign(doc["mime"].GetString(), doc["mime"].GetStringLength());

        std::int64_t declared = 0;
        if (doc.HasMember("size")) {
            if (doc["size"].IsInt64())       declared = doc["size"].GetInt64();
            else if (doc["size"].IsUint64()) declared = static_cast<std::int64_t>(doc["size"].GetUint64());
        }
        if (declared <= 0) {
            send_json(http::status::bad_request, make_error("missing or invalid size"));
            return;
        }
        // 单文件上限：这里挡的是"申报超限"；真实模式下七牛还会按 policy 的
        // fsizeLimit 强制拦截实际上传超限的对象——两道闸互为冗余，不是重复。
        if (declared > frt_.maxSize) {
            send_json(http::status::bad_request,
                      make_error("file too large (limit " +
                                 std::to_string(frt_.maxSize / 1024 / 1024) + "MB)"));
            return;
        }

        const std::string name = sanitizeFileName(rawName);
        if (name.empty()) {
            send_json(http::status::bad_request, make_error("invalid name"));
            return;
        }
        std::string normMime;
        if (!mediaTypeAllowed(name, mime, frt_.typeStrict, normMime)) {
            // 黑名单类型（exe/sh/js...）与未知扩展名都落在这里。
            send_json(http::status::bad_request, make_error("file type not allowed"));
            return;
        }

        // ---- 日配额（DB worker 查询，不阻塞 io）---------------------------
        auto self = shared_from_this();
        fileCrumb("token.quota-submit");
        db_.fileQuotaToday(uid,
            [self, uid, name, normMime, declared](bool ok, const std::string& err, long long used) {
                if (!ok) { self->send_db_error(http::status::bad_request, err); return; }
                if (used + declared > self->frt_.quotaBytes) {
                    // 429 + Retry-After：与现有过载语义一致（客户端按退避重试）。
                    self->send_json(http::status::too_many_requests,
                                    make_error("daily upload quota exceeded"),
                                    /*retryAfterSec=*/3600, /*closeAfter=*/false);
                    return;
                }
                self->finish_upload_token(uid, name, normMime, declared);
            });
    }

    // 凭证签发的"后半段"：生成 uuid/key、签 uptoken 与 ticket（全本地计算）。
    void finish_upload_token(std::uint64_t uid, const std::string& name,
                             const std::string& mime, std::int64_t declared) {
        fileCrumb("token.finish");
        const std::string uuid = generateUuidV4();
        if (uuid.empty()) {
            // CSPRNG 不可用：宁可失败也不退回弱随机（可枚举=可拖库）。
            send_json(http::status::service_unavailable, make_error("entropy failure"));
            return;
        }
        // key 完全由服务端生成：im/<yyyy-mm>/<uuid>.<ext>
        // uuid 锁进 uptoken 的 policy scope，客户端物理上写不了别人的路径。
        const std::string key = "im/" + yyyymmUtc() + "/" + uuid + "." + extensionOf(name);

        // 上传凭证有效期：1 小时。凭证只授权单 key 单对象，宁短勿长。
        static constexpr long long kTokenTtl   = 3600;
        // ticket（commit 的"签发记录"证明）：2 小时——覆盖"大文件传完再 commit"。
        static constexpr long long kTicketTtl  = 7200;

        const std::string uptoken =
            store_->signUploadToken(key, declared, nowUnix() + kTokenTtl);
        const std::string ticket =
            store_->makeTicket(uuid, uid, key, declared, name, mime, kTicketTtl);

        send_json(http::status::ok,
                  make_file_token_ok(uuid, key, store_->uploadUrlForClient(),
                                     uptoken, kTokenTtl, ticket));
        fileCrumb("token.sent");
    }

    // ---- POST /api/files/commit -------------------------------------------
    // 入参 {file_id, ticket}。闭环：验票据 → 七牛 stat 核验对象真实存在 →
    // 以 stat 权威值登记 files 表。没 commit 成功 = files 表没记录 =
    // 这个 file_id 永远发不出去消息（TCP 侧 do_send_media 会拒绝）。
    void do_file_commit(std::uint64_t uid) {
        fileCrumb("commit.enter");
        rapidjson::Document doc;
        if (!read_json(doc)) return;

        std::string fileId, ticket;
        if (doc.HasMember("file_id") && doc["file_id"].IsString())
            fileId.assign(doc["file_id"].GetString(), doc["file_id"].GetStringLength());
        if (doc.HasMember("ticket") && doc["ticket"].IsString())
            ticket.assign(doc["ticket"].GetString(), doc["ticket"].GetStringLength());

        if (!isUuidLike(fileId) || ticket.empty()) {
            send_json(http::status::bad_request, make_error("missing file_id/ticket"));
            return;
        }

        // 票据校验：归属本人 + 未过期 + key/大小/name/mime 都是签发时那份。
        std::string key, name, mime;
        long long declared = 0;
        if (!store_->verifyTicket(ticket, fileId, uid, key, declared, name, mime)) {
            send_json(http::status::forbidden, make_error("invalid or expired upload ticket"));
            return;
        }

        auto self = shared_from_this();
        fileCrumb("commit.ticket-ok, stat-call");
        // stat 是外网调用：回调在 QiniuHttpAgent 线程触发，send_json/send_db_error
        // 内部都会切回 strand，这里不需要再手动 post（与 Db 回调同纪律）。
        store_->stat(key, [self, uid, fileId, key, name, mime, declared]
                     (bool transportOk, const std::string& err, bool exists,
                      long long size, const std::string& qetag) {
            if (!transportOk) {
                // 验收 6：七牛打不通 => 503 + Retry-After，文本链路不受影响。
                self->send_json(http::status::service_unavailable,
                                make_error(err.empty() ? "storage unavailable" : err),
                                /*retryAfterSec=*/2, /*closeAfter=*/false);
                return;
            }
            if (!exists) {
                // 虚报：没传过对象就 commit。400 而不是 404——请求本身不合法。
                self->send_json(http::status::bad_request, make_error("object not uploaded"));
                return;
            }
            // 真实上传的大小以 stat 为准；再次校验上限——
            // mock 模式没有七牛那道 fsizeLimit 闸，这里是唯一防线；
            // 真实模式则是"客户端传了小于申报的值"也能如实登记。
            if (size > self->frt_.maxSize) {
                self->send_json(http::status::bad_request, make_error("object exceeds size limit"));
                return;
            }

            fileCrumb("commit.stat-cb");
            FileRecord rec;
            rec.uuid        = fileId;
            rec.uploader_id = uid;
            rec.name        = name;
            rec.mime        = mime;
            rec.size        = size;
            rec.qetag       = qetag;
            rec.object_key  = key;
            rec.bucket      = self->store_->bucketName();
            rec.zone        = self->store_->zoneId();
            rec.status      = 1;

            fileCrumb("commit.commitFile-call");
            self->db_.commitFile(rec, [self, fileId, size, qetag]
                                 (bool ok, const std::string& err, std::uint64_t) {
                if (!ok) { self->send_db_error(http::status::bad_request, err); return; }
                const std::string url = mediaBaseUrl() + "/api/files/" + fileId;
                fileCrumb("commit.done-cb");
                self->send_json(http::status::ok, make_file_commit_ok(fileId, url, size, qetag));
            });
        });
    }

    // ---- GET /api/files/{uuid}：302 换取下载 --------------------------------
    // 权限口径（MVP，规划已确认的取舍）：登录用户（或持有服务端签发的
    // 短时效 vtoken）拿到 uuid 即可换取。"限引用过该文件的会话成员"需要
    // 文件-会话关联表，是后续项——注释留痕，不假装已经做了。
    void do_file_download(const std::string& uuid) {
        fileCrumb("dl.enter");
        auto self = shared_from_this();
        db_.getFileByUuid(uuid,
            [self, uuid](bool ok, const std::string& err, const FileRecord& rec) {
                if (!ok) { self->send_db_error(http::status::bad_request, err); return; }
                if (!rec.found || rec.status != 1) {
                    self->send_json(http::status::not_found, make_error("file not found"));
                    return;
                }
                // 每次访问都现场签一条短时效 URL（FILE_URL_TTL_SEC），
                // 所以消息体里存的永远是稳定的换取入口，签名永远不会过期腐化。
                self->send_redirect(self->store_->makeDownloadUrl(rec, self->frt_.urlTtlSec));
            });
    }

    // ---- DELETE /api/files/{uuid}：仅上传者本人 ------------------------------
    void do_file_delete(std::uint64_t uid, const std::string& uuid) {
        auto self = shared_from_this();
        db_.deleteFileByUuid(uuid, uid,
            [self, uuid](bool ok, const std::string& err, const FileRecord& rec) {
                if (!ok) { self->send_db_error(http::status::not_found, err); return; }
                // 物理删除七牛对象：先标库再删对象（顺序反了会出现
                // "对象没了但库还说正常"的窗口期——那时消息里的文件点开是
                // 七牛 612，比这里 302 到不存在的 key 更难排查）。
                // remove 失败只记日志：files 行已 status=2，业务侧不可见；
                // 对象成为孤儿由"超期告警+人工/定时 batch 清理"兜底（规划口径）。
                self->store_->remove(rec.object_key, [](bool ok2, const std::string& err2) {
                    if (!ok2) {
                        std::cerr << "[file] storage remove failed (row already marked deleted): "
                                  << err2 << std::endl;
                    }
                });
                self->send_json(http::status::ok, make_ok());
            });
    }

    // ---- POST /api/files/mock-put：mock 模式的"直传"端点 --------------------
    // 真实模式 404。语义：把 (key -> size) 登记进 MockObjectStore，
    // 之后 stat 才能"存在"、mock-object 才有字节可取——commit 防虚报的
    // 负向用例（不 mock-put 直接 commit 必须被拒）依赖的就是这一步。
    void do_file_mock_put(std::uint64_t uid) {
        fileCrumb("put.enter");
        if (!store_->isMock()) {
            send_json(http::status::not_found, make_error("mock endpoints disabled"));
            return;
        }
        rapidjson::Document doc;
        if (!read_json(doc)) return;

        std::string fileId, ticket;
        if (doc.HasMember("file_id") && doc["file_id"].IsString())
            fileId.assign(doc["file_id"].GetString(), doc["file_id"].GetStringLength());
        if (doc.HasMember("ticket") && doc["ticket"].IsString())
            ticket.assign(doc["ticket"].GetString(), doc["ticket"].GetStringLength());
        if (!isUuidLike(fileId) || ticket.empty()) {
            send_json(http::status::bad_request, make_error("missing file_id/ticket"));
            return;
        }
        std::string key, name, mime;
        long long declared = 0;
        if (!store_->verifyTicket(ticket, fileId, uid, key, declared, name, mime)) {
            send_json(http::status::forbidden, make_error("invalid or expired upload ticket"));
            return;
        }
        // isMock() 为真才走到这里，static_cast 安全（工厂只造这两种实现）。
        static_cast<MockObjectStore*>(store_.get())->mockPut(key, declared);
        fileCrumb("put.mocked");
        send_json(http::status::ok, make_ok());
    }

    // ---- GET /api/files/mock-object/{key}：mock 模式的"私有下载"端点 ---------
    // 与七牛私有空间同构：URL 必须带 ?e=<deadline>&token=<ak>:<sign>，
    // 签名过期/伪造一律 403；字节是合成的（上限 64KB），真实大小在头里。
    void do_mock_object_download(const std::string& path, const std::string& query) {
        static const std::string kPrefix = "/api/files/mock-object/";

        const std::string eStr = query_value(query, "e");
        const std::string tok  = query_value(query, "token");
        long long deadline = 0;
        try { deadline = std::stoll(eStr); } catch (...) { deadline = 0; }
        if (eStr.empty() || tok.empty() || deadline < nowUnix()) {
            send_json(http::status::forbidden, make_error("missing or expired signature"));
            return;
        }

        auto* mock = static_cast<MockObjectStore*>(store_.get());
        // 待签串 = "<path>?e=<deadline>"（与 makeDownloadUrl 的生成口径一致，
        // 少一个字符都会验签失败——这正是签名 URL 防篡改的性质本身）。
        if (!mock->verifyObjectToken(path + "?e=" + eStr, tok)) {
            send_json(http::status::forbidden, make_error("bad signature"));
            return;
        }

        // key 在 path 里是 URL 编码过的（urlEncode(key, keepSlash=true)），
        // 解码还原；url_decode 复用上面搜索接口那个实现（对 '+' 的语义在
        // key 场景无影响：object_key 由服务端生成，只含 [a-z0-9/._-]）。
        const std::string key = url_decode(path.substr(kPrefix.size()));

        long long size = 0;
        std::string qetag;
        if (!mock->mockGet(key, size, qetag)) {
            send_json(http::status::not_found, make_error("object not found"));
            return;
        }

        // 合成字节：min(size, 64KB)。真实场景 20MB 的文件在 mock 里没必要
        // 真占 20MB 内存——验收核对的是 X-Mock-Size/X-Mock-Qetag 与登记值一致。
        const std::size_t n = static_cast<std::size_t>(
            std::min<long long>(size, 64 * 1024));
        std::string body;
        body.reserve(n);
        static const char* pattern = "MOCKQINIU-DATA;";
        for (std::size_t i = 0; i < n; ++i) body.push_back(pattern[i % 15]);

        send_bytes(http::status::ok, body,
                   { {"X-Mock-Size", std::to_string(size)},
                     {"X-Mock-Qetag", qetag},
                     {"X-Mock-Key", key} });
    }
    // /*************图片文件功能（七牛云直传）：HTTP 处理器结束***************/

    // ====================================================================
    // /*************群聊功能：HTTP 处理器（BE-GROUP-01~08）***************/
    // ====================================================================
    // 约定与好友模块一致：
    //   * 所有“写事实”成功后，通过 SessionManager 把群事件实时推给在线成员；
    //   * 离线成员不重推事件，登录后靠 GET /api/groups 对齐（成员/资料类），
    //     入群申请与结果则由 session::push_login_sync() 做数据库补偿推送；
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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
                    self->send_db_error(http::status::bad_request, err);
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

    /*************P1-6：发送 JSON 响应（keep-alive 感知的短/长连接切换）***************/
    // retryAfterSec > 0 时附带 Retry-After 头（429/503 用，P0-4）。
    // closeAfter 为 true 时强制关连接：请求体超限（解析状态已不可信）、
    // 服务端过载（不该再留这条连接）等场景。
    // 其余情况按客户端意愿决定：客户端要 keep-alive 且没超过单连接请求数上限
    // 就继续在同一连接上读下一个请求，否则写完关闭。
    void send_json(http::status status, std::string body,
                   int retryAfterSec = 0, bool closeAfter = false) {
        /*************core-dump 修复 ⑤：从 DB 回调进来时先切回本连接的 strand***************/
        // 本函数既被"读完成 handler"（已在 strand 内）调用，也被 Db 的 postToIo
        // 回调（在**任意** io 线程上）调用。后者会读 req_（版本/keep-alive）、
        // 并在 socket_ 上发起 async_write —— 与同一连接上正在跑的读/写/计时器
        // handler 并发，就是 Beast 明令禁止的那种重叠。
        // 先切回 strand，之后的每一步都与其他 handler 互斥，req_ 也不会被
        // 并发的 `req_ = parser_->release()` 改写。
        if (!strand_.running_in_this_thread()) {
            auto self = shared_from_this();
            net::post(strand_,
                      [self, status, body = std::move(body), retryAfterSec, closeAfter]() mutable {
                          self->send_json(status, std::move(body), retryAfterSec, closeAfter);
                      });
            return;
        }

        const bool wantKeepAlive =
            !closeAfter &&
            requestsHandled_ < kMaxRequestsPerConn &&
            req_.version() >= 11 &&
            req_.keep_alive();

        auto self = shared_from_this();
        auto res = std::make_shared<http::response<http::string_body>>(
            status, req_.version());
        res->set(http::field::server, "KC-CXIM");
        res->set(http::field::content_type, "application/json; charset=utf-8");
        if (retryAfterSec > 0) {
            res->set(http::field::retry_after, std::to_string(retryAfterSec));
        }
        res->keep_alive(wantKeepAlive);
        res->body() = std::move(body);
        res->prepare_payload();

        // P0-4：在途计数以"响应"为单位 —— 进入本函数 +1，写完成 -1，
        // 两个操作在同一条路径上，任何分支（成功/失败/keep-alive/关闭）都不会失配。
        inflightCount().fetch_add(1, std::memory_order_relaxed);
        http::async_write(socket_, *res,
            net::bind_executor(strand_,
            [self, res, wantKeepAlive](beast::error_code ec, std::size_t /*bytes*/) {
                fileCrumb("wrcb");
                inflightCount().fetch_sub(1, std::memory_order_relaxed);
                if (ec || !wantKeepAlive) {
                    self->close_socket();
                    return;
                }
                self->do_read();      // 同一连接继续接下一个请求
            }));
    }
    /*************P1-6：发送 JSON 响应（keep-alive 感知的短/长连接切换）***************/

    /*************成员声明顺序 = 实际初始化顺序（这里踩过坑，别再改回去）***************/
    // C++ 按**声明顺序**初始化成员，与初始化列表的书写顺序无关。
    // 之前 socket_ 声明在 idle_timer_ 之后，而构造函数写成
    //     : socket_(std::move(socket)), idle_timer_(socket_.get_executor())
    // 看着没问题，实际执行的是"先拿未构造的 socket_ 的 executor，再构造 socket_"：
    // 未定义行为，表现为 HTTP 响应发不出去、客户端收到 Connection reset。
    // 所以 socket_ 必须排在所有依赖它的成员前面。
    // 同理 strand_ 必须排在 idle_timer_ 前面：计时器要建在这个 strand 的
    // executor 上（core-dump 修复 ⑤），否则它的回调会落回裸 io_context。
    tcp::socket socket_;
    net::strand<net::any_io_executor> strand_;   // 本连接所有 handler 串行在这上面
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    // P1-6：parser 每个请求重建一次，天然支持在同一连接上顺序解多条消息。
    std::unique_ptr<http::request_parser<http::string_body>> parser_;
    net::steady_timer            idle_timer_;            // keep-alive 空闲计时
    std::size_t                  requestsHandled_ = 0;   // 本连接已处理的请求数
    Db&             db_;
    TokenService&   tokens_;
    SessionManager& sessions_;
    /*************图片文件功能（七牛云直传）：shared_ptr 保证 session 比 store 活得久也安全***************/
    std::shared_ptr<ObjectStorage> store_;
    FileRuntime     frt_;

    static constexpr int         kKeepAliveIdleSec   = 60;   // 空闲 60s 关闭
    static constexpr std::size_t kMaxRequestsPerConn = 256;  // 单连接请求数上限
    static constexpr std::size_t kBatchMaxTargets    = 200;  // P1-7：单次批量上限
};

} // namespace

// ---------------------------------------------------------------------------
// http_server
// ---------------------------------------------------------------------------
http_server::http_server(boost::asio::io_context& io, unsigned short port,
                         Db& db, TokenService& tokens, SessionManager& sessions,
                         std::shared_ptr<ObjectStorage> store, const FileRuntime& fileRt)
    : acceptor_(io, tcp::endpoint(tcp::v4(), port)),
      db_(db),
      tokens_(tokens),
      sessions_(sessions),
      /*************图片文件功能（七牛云直传）：存储抽象与文件参数注入***************/
      store_(std::move(store)),
      fileRt_(fileRt) {
    do_accept();
}

// 打印监听端口 + 本次实际生效的过载参数。
// 为什么要把这些数字打在启动日志里：它们全部由环境变量控制，配错时的表现是
// "限流/准入明明做了但压测数据没变化"，或者反过来"验收被 429/503 挡住却看不出是谁挡的"。
// 没有这一行，排查就只能靠猜；有了这一行，一眼就能确认服务端跑的是哪一档闸门
// （对应 readme §11.6 检查清单第 3 条，之前只覆盖了 [db] 那一半）。
void http_server::run() {
    const OverloadConfig& c = overloadConfig();
    std::cout << "[http] listening on port "
              << acceptor_.local_endpoint().port()
              << ", inflight_cap=" << c.maxInflight
              << ", body_max=" << c.bodyMax << "B"
              << ", auth_rate=" << c.authPerSec << "/s burst=" << c.authBurst
              << ", per_ip=" << c.ipPerSec << "/s (ip burst=" << c.ipBurst << ")"
              << std::endl;
    /*************图片文件功能（七牛云直传）：启动自述——跑的是 mock 还是真七牛必须一眼可见***************/
    // 验收/排障第一步是确认"连的是哪套存储"：mock 模式下所有文件 API 都会
    // "成功"，却不知道七牛根本没参与——启动日志里不写明，就是给自己埋一颗
    // "为什么线上没有文件"的雷。这里同样只打非敏感参数，AK/SK 永不出现。
    std::cout << "[file] storage=" << (store_ ? store_->kind() : "none")
              << ", max_size=" << (fileRt_.maxSize / 1024 / 1024) << "MB"
              << ", url_ttl=" << fileRt_.urlTtlSec << "s"
              << ", daily_quota=" << (fileRt_.quotaBytes / 1024 / 1024) << "MB"
              << ", type_strict=" << (fileRt_.typeStrict ? 1 : 0)
              << ", base_url=" << mediaBaseUrl()
              << std::endl;
    /*************图片文件功能（七牛云直传）：启动自述***************/
}

void http_server::do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<http_session>(std::move(socket), db_, tokens_,
                                               sessions_,
                                               store_, fileRt_)->run();
            }
            if (acceptor_.is_open()) do_accept();
        });
}

} // namespace kc
