#pragma once
#include <cstdint>
#include <string>

// ============================================================================
// 业务层消息模型
// ----------------------------------------------------------------------------
// frame.h 只负责"把一串字节安全地切成一条条"，但字节内容是什么？
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
    Text        = 1,     // 文本消息
    Image       = 2,     // 图片消息（content 放 URL 或 base64，不要塞原始二进制）
    Heartbeat   = 3,     // 心跳，用于保活 / 检测连接是否还活着
    Login       = 100,   // 登录请求：from = userId，content = token
    LoginAck    = 101,   // 登录成功回执
    Error       = 102,   // 错误通知（如"未登录就发消息"）

    // ---- 数据库接入后新增的业务类型 -------------------------------------
    HistoryReq  = 110,   // 历史消息请求：to = 对方 userId，seq = before_seq（0=最新），content=limit
    HistoryResp = 111,   // 历史消息响应：content = JSON 数组（见 serialize_history）
    Ack         = 112,   // 投递/已读回执：conversation_id + seq；content 可选 "delivered"
    SendAck     = 113,   // 服务端对"发送成功"的确认：msg_id/seq/ts 为服务端分配值
};

// 一条消息的完整描述。所有字段都有默认值，避免未初始化读取。
struct Message {
    std::uint64_t from   = 0;                 // 发送者 userId（登录时也用它表示"我是谁"）
    std::uint64_t to     = 0;                 // 接收者 userId；心跳/登录时可为 0
    MsgType       type   = MsgType::Text;     // 消息类型
    std::uint64_t msg_id = 0;                 // 客户端发送时=临时消息号；服务端下发时=服务端消息ID
    std::uint64_t ts     = 0;                 // 时间戳（毫秒），服务端消息由服务端生成
    std::string   content;                    // 文本正文 / 图片地址 / token 等

    // ---- 数据库接入后新增字段（用于排序、分页、回执） -------------------
    std::uint64_t conversation_id = 0;        // 会话ID（服务端下发 / 回执时使用）
    std::uint64_t seq             = 0;        // 会话内单调序号；历史请求时表示 before_seq
};

// 从数据库读出来的一条消息（比 Message 多出 conversation_id/seq，便于回执）。
struct StoredMessage {
    std::uint64_t msg_id          = 0;        // messages.id（服务端消息ID）
    std::uint64_t conversation_id = 0;        // 会话ID
    std::uint64_t seq             = 0;        // 会话内单调序号
    std::uint64_t from            = 0;        // 发送者 userId
    std::uint64_t to              = 0;        // 接收者（单聊时由业务层填写，便于投递）
    MsgType       type            = MsgType::Text;
    std::uint64_t ts              = 0;        // 毫秒时间戳
    std::string   content;
    bool          duplicate       = false;    // true 表示命中去重（未真正插入新消息）
};

} // namespace kc
