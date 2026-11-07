#pragma once
#include <cstdint>
#include <string>

// ============================================================================
// 业务层消息模型
// ----------------------------------------------------------------------------
// 前面 frame.h 只负责"把一串字节安全地切成一条条"，但字节内容是什么？
// 这里用 Message 结构体描述一条 IM 消息的业务含义：
//   - 谁发的      -> from
//   - 发给谁      -> to
//   - 消息类型    -> type（文本 / 图片 / 心跳 / 登录 ...）
//   - 业务内容    -> content
// 真正上网传输时，Message 会被编码成 JSON（见 message_codec.h），
// 解析出来后再装回这个结构体，业务代码就只跟结构体打交道。
// ============================================================================

namespace kc {

// 消息类型。用 enum class 而不是普通 enum 的好处：
//   1) 有强类型作用域，必须写 MsgType::Text，避免名字污染；
//   2) 不会隐式转换成 int，减少误用。
// 底层类型固定为 uint16_t，保证线上协议里的数值稳定（不会因编译器变化）。
enum class MsgType : std::uint16_t {
    Text      = 1,     // 文本消息
    Image     = 2,     // 图片消息（content 放 URL 或 base64，不要塞原始二进制）
    Heartbeat = 3,     // 心跳，用于保活 / 检测连接是否还活着
    Login     = 100,   // 登录请求：from = userId，content = token
    LoginAck  = 101,   // 登录成功回执
    Error     = 102,   // 错误通知（如"未登录就发消息"）
};

// 一条消息的完整描述。所有字段都有默认值，避免未初始化读取。
struct Message {
    std::uint64_t from   = 0;                 // 发送者 userId（登录时也用它表示"我是谁"）
    std::uint64_t to     = 0;                 // 接收者 userId；心跳/登录时可为 0
    MsgType       type   = MsgType::Text;     // 消息类型
    std::uint64_t msg_id = 0;                 // 消息序号，客户端用来做去重/回执关联
    std::uint64_t ts     = 0;                 // 时间戳（毫秒），用于排序/展示
    std::string   content;                    // 文本正文 / 图片地址 / token 等
};

} // namespace kc
