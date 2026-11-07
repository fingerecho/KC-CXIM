#pragma once

// ============================================================================
// 应用层分帧协议（Frame）
// ----------------------------------------------------------------------------
// TCP 是"字节流"：它不保证一次 read 就等于一条完整消息，也不保证消息边界。
// 例如发送方写了 "AAAA" + "BBBB"，接收方可能一次读到 "AAAABBBB"（粘包），
// 也可能先读到 "AAA" 再读到 "ABBBB"（半包）。
//
// 因此我们必须自己定义"一条消息到哪里结束"，这里采用最经典的做法：
//
//     +------------------+--------------------------------+
//     | 4 字节长度 len   |  len 字节的 JSON 消息体         |
//     | （网络字节序）   |                                |
//     +------------------+--------------------------------+
//
// 发送方：先算 body 的字节数，用 htonl 转成"网络字节序"（大端）写成 4 字节
//         头，再紧跟 body。
// 接收方：先精确读 4 字节头 -> ntohl 还原成本机字节序 -> 得到 body 长度，
//         再按该长度精确读 body。
// 这样接收方永远知道"当前这条消息还有多少字节没读完"，粘包/半包都被消化掉。
// ============================================================================

#if defined(_WIN32)
#  include <winsock2.h>   // Windows：htonl / ntohl 的声明在 winsock2.h
#else
#  include <arpa/inet.h>  // Linux/macOS：htonl / ntohl 的声明在 arpa/inet.h
#endif

#include <cstdint>   // std::uint32_t
#include <cstring>   // std::memcpy
#include <vector>
#include <stdexcept>

namespace kc::frame {

// 长度头固定占 4 字节。32 位无符号数最大能表示约 4GB 的 body 长度。
constexpr std::size_t HEADER_LEN = 4;

// 单条消息体的防御性上限。防止恶意客户端声明一个超大长度，让服务器
// 按该长度去分配内存（内存耗尽攻击）。真实 IM 的图片建议传 URL/分片，
// 而不是把原始二进制塞进单条消息里。
constexpr std::uint32_t MAX_BODY = 4096;

// ---------------------------------------------------------------------------
// encode：组帧时使用。把"本机字节序"的 body_len 转成"网络字节序"写进 header。
//   htonl = host to network long，即把 32 位整数转换到大端表示。
//   不同 CPU 的字节序不同（x86 是小端），网络传输统一约定为大端。
// ---------------------------------------------------------------------------
inline void encode(std::uint32_t body_len, char* header) {
    std::uint32_t net_len = htonl(body_len);
    // 这里用 memcpy 而不是 *(std::uint32_t*)header = net_len，原因有二：
    //   1) header 不保证 4 字节对齐，直接按 uint32_t* 解引用在部分平台会崩；
    //   2) 违反"严格别名规则"（strict aliasing）属于未定义行为。
    std::memcpy(header, &net_len, HEADER_LEN);
}

// ---------------------------------------------------------------------------
// decode：拆帧时使用。从 4 字节 header 还原出"本机字节序"的 body 长度。
//   ntohl = network to host long。
// ---------------------------------------------------------------------------
inline std::uint32_t decode(const char* header) {
    std::uint32_t net_len = 0;
    std::memcpy(&net_len, header, HEADER_LEN);  // 同样避免对齐/别名问题
    return ntohl(net_len);
}

} // namespace kc::frame
