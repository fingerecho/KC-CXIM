#pragma once
#include <cstdint>
#include <string>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

// ============================================================================
// /*************图片文件功能（七牛云直传）：文件领域模型与元数据工具***************/
// ----------------------------------------------------------------------------
// 与 friend_types.h / group_types.h 同一思路：把"跨模块共享的结构 + JSON
// 序列化/校验"集中到一个头文件，HTTP 层（http_server.cpp）、TCP 层
// （session.cpp）、DB 层（db.cpp）都从这里取，避免同一份元数据格式存在
// 多份实现而漂移。
//
// 消息元数据 JSON（type=2 图片 / type=4 文件的 content）：
//   {"file_id":"<uuid>","url":"<FILE_BASE_URL>/api/files/<uuid>",
//    "name":"照片.png","size":345678,"mime":"image/png","qetag":"<hex32>"}
// 要点（与规划一致）：
//   * url 永远是**服务器换取入口**，绝不是七牛签名直链——私有空间签名 URL
//     带 deadline，写进消息就等于"半年后必坏"的历史消息；
//   * 这份 JSON 由服务端在发送校验通过后**整体重建**（客户端自报的
//     name/size/mime/url 一律不信），所以接收端看到的就是权威值。
// ============================================================================

namespace kc {

// 一条 files 表记录（DAO 读出 / commit 写入共用一个结构，列顺序在 db.cpp 集中定义）。
struct FileRecord {
    bool            found   = false;   // getFileByUuid 用：uuid 是否存在
    std::uint64_t   id          = 0;   // files.id
    std::string     uuid;              // 对外文件标识（随机 v4，防枚举）
    std::uint64_t   uploader_id = 0;   // 上传者 userId（发送校验的权属依据）
    std::string     name;              // 原始文件名（仅展示）
    std::int64_t    size          = 0; // 字节数，以七牛 stat 返回为准登记
    std::string     mime;              // MIME（白名单校验后存下的展示值）
    std::string     qetag;             // 七牛对象校验和（见 schema 注释：非 sha256）
    std::string     object_key;        // 七牛对象名 im/<yyyy-mm>/<uuid>.<ext>
    std::string     bucket;            // 提交时所在 bucket（换区域/迁移时可追溯）
    int             zone        = 0;   // 七牛区域编号（z0=1,z1=2,z2=3…存配置原值）
    int             status      = 0;   // 1=正常 2=已删除
};

// ---------------------------------------------------------------------------
// uuid v4：36 字符 "8-4-4-4-12" 小写十六进制。
// 随机源用 OpenSSL RAND_bytes（操作系统密码学级随机）而不是 mt19937：
// uuid 在这里同时是**防枚举标识**（/api/files/{uuid} 只要登录就能换下载），
// 伪随机 + 时间播种会被预测，必须用 CSPRNG。
// ---------------------------------------------------------------------------
std::string generateUuidV4();

// 宽松校验 uuid 形状（仅 [0-9a-f-]、36 位）：所有进入 SQL 拼接/URL 的 uuid
// 先过这一关，形状不对直接拒绝，不给注入和路径穿越留口子。
bool isUuidLike(const std::string& s);

// ---------------------------------------------------------------------------
// 消息 content 里媒体元数据的解析/重建。
// ---------------------------------------------------------------------------
// 从 type=2/4 的 content 中抽取客户端自报的 file_id（发送校验第一步，io 线程纯 CPU）。
// 不校验其它字段：name/size/url 等都会在权属校验通过后被服务端重建覆盖。
bool extractFileIdFromContent(const std::string& content, std::string& uuid, std::string& err);

// 服务端权威重建媒体元数据 JSON（发送前改写 + HTTP commit 响应都用它）。
std::string buildMediaMetaJson(const FileRecord& rec, const std::string& baseUrl);

// ---------------------------------------------------------------------------
// 扩展名/MIME 白名单（规划：图片 + 常见文档/压缩包/音视频；默认拒绝可执行类型）。
// 返回 false = 不允许签发上传凭证。allowedMimePrefix 同时回填一个"合理"的
// 展示 MIME（客户端自报 mime 只是参考，最终以扩展名为准做展示归一化）。
// ---------------------------------------------------------------------------
bool mediaTypeAllowed(const std::string& name, const std::string& selfReportedMime,
                      bool strict, std::string& normalizedMime);

// 取小写扩展名（最后一个 '.' 之后，仅 [a-z0-9]{1,10}）；不合法返回空串。
std::string extensionOf(const std::string& name);

// 文件名消毒：去掉控制字符与换行、截断到 255 字节（UTF-8 边界安全截断：
// 多字节字符中间切开会让 MariaDB 严格模式报 1406——批量申请附言的老坑，
// 这里不再犯第二次）。
std::string sanitizeFileName(const std::string& raw);

// ---------------------------------------------------------------------------
// FileRuntime：文件功能运行期参数（全部环境变量，fromEnv 一次读取）。
//   QINIU_ENABLED            0=MockObjectStore（默认，开发/CI 全流程可用）
//                            1=QiniuObjectStore（真实对接，需 QiniuConfig.json）
//   FILE_MAX_SIZE_MB         单文件大小上限（默认 20，签发凭证时校验；
//                            真实模式同时写进 uploadToken policy 由七牛强制）
//   FILE_URL_TTL_SEC         下载签名 URL 时效（默认 600s）
//   FILE_USER_DAILY_QUOTA_MB 每人每日上传配额（默认 500，按已 commit 的 size 累计）
//   FILE_BASE_URL            消息元数据 url 前缀（默认 http://127.0.0.1:8080；
//                            生产必须设为客户端可达的 HTTP 对外地址）
//   FILE_TYPE_STRICT         1=白名单严格模式（默认；0 仅作逃生阀，不建议）
// ---------------------------------------------------------------------------
struct FileRuntime {
    bool        qiniuEnabled = false;
    std::int64_t maxSize     = 20LL * 1024 * 1024;
    long long   urlTtlSec    = 600;
    long long   quotaBytes   = 500LL * 1024 * 1024;
    bool        typeStrict   = true;
    std::string baseUrl      = "http://127.0.0.1:8080";

    static FileRuntime fromEnv();
};

// 当前 Unix 秒（七牛上传凭证 policy.deadline、ticket exp、下载签名 e= 共用）。
long long nowUnix();

// 消息元数据 url 前缀（FILE_BASE_URL）。函数内 static：环境变量只在进程
// 启动时读一次，TCP/HTTP 两条路径拿到的是同一个值（两边拼出的 url 不一致
// 是比"暂时不可达"更难查的坑——客户端本地存了 A url、换下载时 B url 验签失败）。
const std::string& mediaBaseUrl();

// 排障面包屑（FILE_CRUMBS=1 时启用）：每个文件处理步骤以"打开-追加-关闭"
// 立刻落盘 file_crumbs.log——进程被硬崩时没有任何缓冲丢失，最后一行面包屑
// 就是死亡现场。（本轮 commit/put 随机杀进程的取证手段，保留为运维工具。）
void fileCrumb(const char* where);

} // namespace kc
