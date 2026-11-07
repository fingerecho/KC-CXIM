#pragma once
#include "message.h"

// RapidJSON 是"header-only"库：只需要 include 头文件，不用链接 .lib。
//   document.h   -> 解析 + DOM 树（Document 就是一个 JSON 值树）
//   writer.h     -> 把数据写成 JSON 文本
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

// ---------------------------------------------------------------------------
// parse_message：把网络收到的 body 字节解析成 Message 结构体。
//   body : 收到的原始消息体（不含 4 字节长度头）
//   out  : 解析成功后写入这里
//   err  : 解析失败时写入原因，方便日志排查
// 返回 true 表示成功，false 表示失败（调用方应断开连接或回错误）。
// ---------------------------------------------------------------------------
inline bool parse_message(const std::vector<char>& body, Message& out, std::string& err) {
    rapidjson::Document doc;   // Document 就是一个可查询的 JSON 树

    // 【关键】必须传长度！body 是 vector<char>，末尾没有 '\0' 终止符，
    // 若调用 doc.Parse(body.data())（只传指针），RapidJSON 会一直往后找 '\0'，
    // 造成越界读（崩溃或读到垃圾）。带长度的重载能精确限定读取范围。
    doc.Parse(body.data(), body.size());

    // HasParseError 为 true 说明不是合法 JSON（或 JSON 后面还有多余字符等）。
    if (doc.HasParseError()) {
        err = "json parse error, offset=" + std::to_string(doc.GetErrorOffset());
        return false;
    }
    // 约定每条消息的 JSON 根必须是一个对象 {...}，而不是数组或裸字符串。
    if (!doc.IsObject()) { err = "json root is not object"; return false; }

    // ---- 必填字段校验 -----------------------------------------------------
    // HasMember 前必须先确认是对象（上面已确认），否则行为不确定。
    // IsUint64 / IsInt 是类型检查：类型不对就直接拒绝，绝不做隐式强转，
    // 否则恶意客户端传个字符串会导致 GetUint64() 断言失败。
    if (!doc.HasMember("from") || !doc["from"].IsUint64()) { err = "bad/absent from"; return false; }
    if (!doc.HasMember("type") || !doc["type"].IsInt())    { err = "bad/absent type"; return false; }

    out.from = doc["from"].GetUint64();                 // 发送者 userId
    out.type = static_cast<MsgType>(doc["type"].GetInt());  // 数值转枚举

    // "to" 是可选字段：登录 / 心跳允许没有接收者。
    if (doc.HasMember("to") && doc["to"].IsUint64()) out.to = doc["to"].GetUint64();

    // ---- 按消息类型校验各自需要的字段 ------------------------------------
    switch (out.type) {
    case MsgType::Login:
        // 登录时 content 存 token；这里先按"可选字符串"处理。
        if (doc.HasMember("content") && doc["content"].IsString())
            out.content.assign(doc["content"].GetString(), doc["content"].GetStringLength());
        break;

    case MsgType::LoginAck:
    case MsgType::Heartbeat:
        // 这两类不需要 content，直接放过。
        break;

    case MsgType::Text:
    case MsgType::Image:
        // 文本/图片必须带 content。用 GetStringLength 而不是 strlen，
        // 因为文本里理论上可能包含 '\0'，按长度拷贝才安全。
        if (!doc.HasMember("content") || !doc["content"].IsString()) { err = "missing content"; return false; }
        out.content.assign(doc["content"].GetString(), doc["content"].GetStringLength());
        break;

    default:
        // 未知类型直接拒绝，避免服务器处理不认识的协议。
        err = "unknown type"; return false;
    }

    // ---- 可选字段 ---------------------------------------------------------
    if (doc.HasMember("msg_id") && doc["msg_id"].IsUint64()) out.msg_id = doc["msg_id"].GetUint64();
    if (doc.HasMember("ts")     && doc["ts"].IsUint64())     out.ts     = doc["ts"].GetUint64();

    return true;
}

// ---------------------------------------------------------------------------
// serialize_message：把 Message 结构体编码成 JSON 文本。
// 这里用 Writer 逐字段写出，比手写字符串拼接更安全（自动转义引号/换行等）。
// ---------------------------------------------------------------------------
inline std::string serialize_message(const Message& m) {
    rapidjson::StringBuffer sb;                                  // 输出缓冲区
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);            // 流式写 JSON

    w.StartObject();
    w.Key("from");   w.Uint64(m.from);
    w.Key("to");     w.Uint64(m.to);
    w.Key("type");   w.Int(static_cast<int>(m.type));            // 枚举转回数字
    w.Key("msg_id"); w.Uint64(m.msg_id);
    w.Key("ts");     w.Uint64(m.ts);
    // 显式传长度，content 里即使有 '\0' 也能正确输出。
    w.Key("content"); w.String(m.content.c_str(), m.content.size());
    w.EndObject();

    // 用"指针 + 长度"构造 std::string，避免依赖缓冲区里的 '\0'。
    return std::string(sb.GetString(), sb.GetSize());
}

} // namespace kc
