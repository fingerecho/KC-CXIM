#pragma once
#include "message.h"

// RapidJSON 是"header-only"库：只需要 include 头文件，不用链接 .lib。
//   document.h     -> 解析 + DOM 树（Document 就是一个 JSON 值树）
//   writer.h       -> 把数据写成 JSON 文本
//   stringbuffer.h -> Writer 的输出缓冲区
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include <string>
#include <vector>

// ============================================================================
// JSON <-> Message 的编解码
// ----------------------------------------------------------------------------
// 这些函数是 inline 的，写在头文件里即可，不需要额外的 .cpp，
// 因此 CMake 不用为它们增加源文件。
// ============================================================================

namespace kc {

// 把 RapidJSON 的字符串按长度安全地拷到 std::string（可包含 '\0'）。
inline void assign_json_string(const rapidjson::Value& v, std::string& out) {
    out.assign(v.GetString(), v.GetStringLength());
}

// ---------------------------------------------------------------------------
// parse_message：把网络收到的 body 字节解析成 Message 结构体。
// ---------------------------------------------------------------------------
inline bool parse_message(const std::vector<char>& body, Message& out, std::string& err) {
    rapidjson::Document doc;

    // 【关键】必须传长度！body 是 vector<char>，末尾没有 '\0' 终止符，
    // 若调用 doc.Parse(body.data())（只传指针），RapidJSON 会一直往后找 '\0'，
    // 造成越界读（崩溃或读到垃圾）。带长度的重载能精确限定读取范围。
    doc.Parse(body.data(), body.size());

    if (doc.HasParseError()) {
        err = "json parse error, offset=" + std::to_string(doc.GetErrorOffset());
        return false;
    }
    if (!doc.IsObject()) { err = "json root is not object"; return false; }

    // ---- 必填字段校验 -----------------------------------------------------
    if (!doc.HasMember("from") || !doc["from"].IsUint64()) { err = "bad/absent from"; return false; }
    if (!doc.HasMember("type") || !doc["type"].IsInt())    { err = "bad/absent type"; return false; }

    out.from = doc["from"].GetUint64();
    out.type = static_cast<MsgType>(doc["type"].GetInt());

    // "to" 是可选字段：登录 / 心跳允许没有接收者。
    if (doc.HasMember("to") && doc["to"].IsUint64()) out.to = doc["to"].GetUint64();

    // ---- 按消息类型校验各自需要的字段 ------------------------------------
    switch (out.type) {
    case MsgType::Login:
    case MsgType::LoginAck:
    case MsgType::Error:
    case MsgType::SendAck:
        // content 存在就解析；错误通知/回执里通常是提示文字。
        if (doc.HasMember("content") && doc["content"].IsString())
            assign_json_string(doc["content"], out.content);
        break;

    case MsgType::Heartbeat:
        // 心跳不需要 content，直接放过。
        break;

    case MsgType::Text:
    case MsgType::Image:
        // 文本/图片必须带 content。
        if (!doc.HasMember("content") || !doc["content"].IsString()) { err = "missing content"; return false; }
        assign_json_string(doc["content"], out.content);
        break;

    case MsgType::HistoryReq:
        // 历史请求：content 可选（里面放 limit，如 "20"）。
        if (doc.HasMember("content") && doc["content"].IsString())
            assign_json_string(doc["content"], out.content);
        break;

    case MsgType::HistoryResp:
        // 历史响应：content 是一段 JSON 数组文本。
        if (!doc.HasMember("content") || !doc["content"].IsString()) { err = "missing history content"; return false; }
        assign_json_string(doc["content"], out.content);
        break;

    case MsgType::Ack:
        // 回执：content 可选，约定 "delivered" 表示只更新投递、不更新已读。
        if (doc.HasMember("content") && doc["content"].IsString())
            assign_json_string(doc["content"], out.content);
        break;

    case MsgType::FriendRequest:
    case MsgType::FriendRequestResult:
    case MsgType::FriendListChanged:
        // 好友事件是“服务端 -> 客户端”的下行消息，content 是一段 JSON 文本。
        // 服务端不会接收这三类消息；这里支持解析主要是让测试客户端/多端 SDK
        // 能复用同一套 message_codec，而不需要各自再写一份解析逻辑。
        if (!doc.HasMember("content") || !doc["content"].IsString()) {
            err = "missing friend event content";
            return false;
        }
        assign_json_string(doc["content"], out.content);
        break;

    default:
        // 未知类型直接拒绝，避免服务器处理不认识的协议。
        err = "unknown type"; return false;
    }

    // ---- 可选字段（数据库接入后新增） ------------------------------------
    if (doc.HasMember("msg_id") && doc["msg_id"].IsUint64()) out.msg_id = doc["msg_id"].GetUint64();
    if (doc.HasMember("ts")     && doc["ts"].IsUint64())     out.ts     = doc["ts"].GetUint64();
    if (doc.HasMember("conversation_id") && doc["conversation_id"].IsUint64())
        out.conversation_id = doc["conversation_id"].GetUint64();
    if (doc.HasMember("seq") && doc["seq"].IsUint64())
        out.seq = doc["seq"].GetUint64();

    return true;
}

// ---------------------------------------------------------------------------
// serialize_message：把 Message 结构体编码成 JSON 文本。
// ---------------------------------------------------------------------------
inline std::string serialize_message(const Message& m) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    w.StartObject();
    w.Key("from");   w.Uint64(m.from);
    w.Key("to");     w.Uint64(m.to);
    w.Key("type");   w.Int(static_cast<int>(m.type));
    w.Key("msg_id"); w.Uint64(m.msg_id);
    w.Key("ts");     w.Uint64(m.ts);
    w.Key("conversation_id"); w.Uint64(m.conversation_id);
    w.Key("seq");    w.Uint64(m.seq);
    w.Key("content"); w.String(m.content.c_str(), m.content.size());
    w.EndObject();

    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// serialize_history：把一批历史消息编码成 JSON 数组文本，放进 HistoryResp
// 的 content 字段里（content 本身是一个字符串，所以这里再序列化一层）。
// ---------------------------------------------------------------------------
inline std::string serialize_history(const std::vector<StoredMessage>& msgs) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);

    w.StartArray();
    for (const auto& m : msgs) {
        w.StartObject();
        w.Key("msg_id");          w.Uint64(m.msg_id);
        w.Key("conversation_id"); w.Uint64(m.conversation_id);
        w.Key("seq");             w.Uint64(m.seq);
        w.Key("from");            w.Uint64(m.from);
        w.Key("type");            w.Int(static_cast<int>(m.type));
        w.Key("ts");              w.Uint64(m.ts);
        w.Key("content");         w.String(m.content.c_str(), m.content.size());
        w.EndObject();
    }
    w.EndArray();

    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// parse_history：客户端/测试用，把 HistoryResp.content 解析回 StoredMessage 列表。
// ---------------------------------------------------------------------------
inline bool parse_history(const std::string& text, std::vector<StoredMessage>& out, std::string& err) {
    rapidjson::Document doc;
    doc.Parse(text.c_str(), text.size());
    if (doc.HasParseError()) { err = "history json parse error"; return false; }
    if (!doc.IsArray())      { err = "history json is not array"; return false; }

    for (const auto& v : doc.GetArray()) {
        if (!v.IsObject()) continue;
        StoredMessage m;
        if (v.HasMember("msg_id")          && v["msg_id"].IsUint64())          m.msg_id          = v["msg_id"].GetUint64();
        if (v.HasMember("conversation_id") && v["conversation_id"].IsUint64()) m.conversation_id = v["conversation_id"].GetUint64();
        if (v.HasMember("seq")             && v["seq"].IsUint64())             m.seq             = v["seq"].GetUint64();
        if (v.HasMember("from")            && v["from"].IsUint64())            m.from            = v["from"].GetUint64();
        if (v.HasMember("type")            && v["type"].IsInt())               m.type            = static_cast<MsgType>(v["type"].GetInt());
        if (v.HasMember("ts")              && v["ts"].IsUint64())              m.ts              = v["ts"].GetUint64();
        if (v.HasMember("content")         && v["content"].IsString())         assign_json_string(v["content"], m.content);
        out.push_back(std::move(m));
    }
    return true;
}

} // namespace kc
