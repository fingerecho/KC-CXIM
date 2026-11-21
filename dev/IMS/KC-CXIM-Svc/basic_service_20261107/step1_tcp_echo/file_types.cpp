#include "file_types.h"

#include "qiniu_signer.h"

#include <openssl/rand.h>          // RAND_bytes：uuid 的密码学级随机源

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string_view>

// ============================================================================
// /*************图片文件功能（七牛云直传）：领域模型实现***************/
// ============================================================================

namespace kc {

// ---------------------------------------------------------------------------
// uuid v4：16 字节 CSPRNG 随机，按 8-4-4-4-12 排版；
// version 位固定 4（第 7 字节高 4 位）、variant 位固定 10xx（第 9 字节高 2 位）。
// 这两个位不是装饰：形状校验（isUuidLike）+ 标准排布让"是不是 uuid"一眼可判。
// ---------------------------------------------------------------------------
std::string generateUuidV4() {
    unsigned char b[16];
    // RAND_bytes 失败（系统熵源异常）时返回 0：这时宁可直接失败也不能
    // 回退到弱随机——弱 uuid 会被枚举，等价于把别人的文件目录暴露出去。
    if (RAND_bytes(b, 16) != 1) {
        return {};
    }
    b[6] = static_cast<unsigned char>((b[6] & 0x0F) | 0x40);  // version 4
    b[8] = static_cast<unsigned char>((b[8] & 0x3F) | 0x80);  // variant 10xx

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(hex[b[i] >> 4]);
        out.push_back(hex[b[i] & 0xF]);
    }
    return out;
}

bool isUuidLike(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// extractFileIdFromContent：只取 file_id，形状必须是 uuid。
// 放在 io 线程做（RapidJSON 解析几百字节，微秒级），真正的"权属校验"
// 在 DB worker（session.cpp 的 do_send_media），这里只是粗筛。
// ---------------------------------------------------------------------------
bool extractFileIdFromContent(const std::string& content, std::string& uuid, std::string& err) {
    rapidjson::Document doc;
    doc.Parse(content.data(), content.size());
    if (doc.HasParseError() || !doc.IsObject()) { err = "media content is not json object"; return false; }
    if (!doc.HasMember("file_id") || !doc["file_id"].IsString()) { err = "missing file_id"; return false; }
    uuid.assign(doc["file_id"].GetString(), doc["file_id"].GetStringLength());
    if (!isUuidLike(uuid)) { err = "bad file_id"; return false; }
    return true;
}

// ---------------------------------------------------------------------------
// buildMediaMetaJson：服务端权威重建。
// name/mime 里可能有引号/中文/控制字符——一律走 RapidJSON Writer 输出，
// 由它负责转义（手工拼字符串早晚拼出一个转义事故，这里不给机会）。
// ---------------------------------------------------------------------------
std::string buildMediaMetaJson(const FileRecord& rec, const std::string& baseUrl) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("file_id"); w.String(rec.uuid.c_str(), static_cast<unsigned>(rec.uuid.size()));
    const std::string url = baseUrl + "/api/files/" + rec.uuid;
    w.Key("url");     w.String(url.c_str(), static_cast<unsigned>(url.size()));
    w.Key("name");    w.String(rec.name.c_str(), static_cast<unsigned>(rec.name.size()));
    w.Key("size");    w.Int64(rec.size);
    w.Key("mime");    w.String(rec.mime.c_str(), static_cast<unsigned>(rec.mime.size()));
    w.Key("qetag");   w.String(rec.qetag.c_str(), static_cast<unsigned>(rec.qetag.size()));
    w.EndObject();
    return std::string(sb.GetString(), sb.GetSize());
}

// ---------------------------------------------------------------------------
// 扩展名白名单（规划口径）：
//   图片：jpg jpeg png gif webp bmp
//   文档：pdf txt md doc docx xls xlsx ppt pptx
//   压缩包：zip rar 7z
//   音视频：mp3 mp4 wav
// 默认拒绝一切其它扩展名（exe/bat/cmd/sh/js/ps1/msi/com/scr/vbs/jsp/php/htm/html
// 等可执行或可诱发浏览器误执行的类型自然也在拒绝之列——白名单制不需要枚举黑名单）。
// MIME 只是展示归一化的参考：**扩展名才是闸门**，因为客户端自报 mime
// 毫无约束力（伪造头是 1 行代码的事），七牛私有下载响应里服务端也会按这里
// 归一化后的 mime 设置（真实模式七牛按对象上传时的 Content-Type 返回，
// mock 模式按本表返回，两边都取"白名单允许的值域"）。
// ---------------------------------------------------------------------------
std::string extensionOf(const std::string& name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};
    std::string ext;
    for (std::size_t i = dot + 1; i < name.size(); ++i) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
        if (!std::isalnum(static_cast<unsigned char>(c))) return {};   // 含非法字符=不给凭证
        ext.push_back(c);
        if (ext.size() > 10) return {};                                // 超长"扩展名"不是扩展名
    }
    return ext;
}

bool mediaTypeAllowed(const std::string& name, const std::string& selfReportedMime,
                      bool strict, std::string& normalizedMime) {
    static const std::set<std::string> kAllowed = {
        "jpg","jpeg","png","gif","webp","bmp",
        "pdf","txt","md","doc","docx","xls","xlsx","ppt","pptx",
        "zip","rar","7z",
        "mp3","mp4","wav"
    };
    static const std::set<std::string> kImage = {"jpg","jpeg","png","gif","webp","bmp"};

    const std::string ext = extensionOf(name);
    if (ext.empty()) return false;
    if (kAllowed.find(ext) == kAllowed.end()) return false;   // 白名单之外一票否决

    // 自报 MIME 只做一致性参考：image/* 的扩展名必须是图片扩展名，反之亦然。
    // 客户端乱报（比如图片报 octet-stream）不拒绝——展示归一化后以扩展名为准，
    // 拒绝它没有安全收益，只会把正常客户端挡在门外。
    // 另外**客户端没带 mime 字段时必须放行**（第一版把"空 mime"当成非 image/*
    // 参与 != 比较，导致严格模式下所有不带 mime 的图片请求被误判
    // "file type not allowed"——curl 实测踩中的就是这个）。
    const bool mimeSaysImage = selfReportedMime.rfind("image/", 0) == 0;
    const bool extIsImage    = kImage.count(ext) > 0;
    if (strict && !selfReportedMime.empty() && mimeSaysImage != extIsImage) return false;

    if (extIsImage) {
        normalizedMime = "image/";
        normalizedMime += (ext == "jpg" ? "jpeg" : ext);
    } else if (ext == "pdf")      normalizedMime = "application/pdf";
    else if (ext == "txt" || ext == "md") normalizedMime = "text/plain";
    else if (ext == "mp3")        normalizedMime = "audio/mpeg";
    else if (ext == "mp4")        normalizedMime = "video/mp4";
    else if (ext == "wav")        normalizedMime = "audio/wav";
    else if (ext == "zip")        normalizedMime = "application/zip";
    else if (ext == "rar")        normalizedMime = "application/x-rar-compressed";
    else if (ext == "7z")         normalizedMime = "application/x-7z-compressed";
    else if (ext == "doc")        normalizedMime = "application/msword";
    else if (ext == "docx")       normalizedMime = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    else if (ext == "xls")        normalizedMime = "application/vnd.ms-excel";
    else if (ext == "xlsx")       normalizedMime = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    else if (ext == "ppt")        normalizedMime = "application/vnd.ms-powerpoint";
    else if (ext == "pptx")       normalizedMime = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
    else                          normalizedMime = "application/octet-stream";
    return true;
}

// UTF-8 边界安全截断：从后往前最多回退 3 字节，保证不切在多字节序列中间。
static void truncateUtf8Safe(std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return;
    std::size_t cut = maxBytes;
    // UTF-8 续字节形如 10xxxxxx（0x80~0xBF）：遇到就往前退。
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
}

std::string sanitizeFileName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F) continue;     // 控制字符/DEL 一律剔除（日志注入、终端转义都挡掉）
        out.push_back(static_cast<char>(c));
    }
    truncateUtf8Safe(out, 255);                  // 与 files.name VARCHAR(255) 对齐
    return out;
}

// ---------------------------------------------------------------------------
// FileRuntime::fromEnv：与 DbRuntime/OverloadConfig 同款"环境变量 + 非法回默认"。
// ---------------------------------------------------------------------------
FileRuntime FileRuntime::fromEnv() {
    FileRuntime rt;
    auto envNum = [](const char* name, long long def) -> long long {
        const char* v = std::getenv(name);
        if (!v || !*v) return def;
        try { return std::stoll(v); } catch (...) { return def; }
    };
    rt.qiniuEnabled = envNum("QINIU_ENABLED", 0) != 0;
    const long long mb = envNum("FILE_MAX_SIZE_MB", 20);
    rt.maxSize = (mb >= 1 && mb <= 1024) ? mb * 1024 * 1024 : 20LL * 1024 * 1024;
    const long long ttl = envNum("FILE_URL_TTL_SEC", 600);
    rt.urlTtlSec = (ttl >= 30 && ttl <= 86400) ? ttl : 600;
    const long long quota = envNum("FILE_USER_DAILY_QUOTA_MB", 500);
    rt.quotaBytes = (quota >= 1) ? quota * 1024 * 1024 : 500LL * 1024 * 1024;
    rt.typeStrict = envNum("FILE_TYPE_STRICT", 1) != 0;
    if (const char* base = std::getenv("FILE_BASE_URL"); base && *base) {
        rt.baseUrl = base;
        // 去掉结尾 '/'：拼 "/api/files/<uuid>" 时避免双斜杠（历史坑：nginx
        // 双斜杠直接 404，客户端表现为"图片全裂"却查不出为什么）。
        while (!rt.baseUrl.empty() && rt.baseUrl.back() == '/') rt.baseUrl.pop_back();
    }
    return rt;
}

long long nowUnix() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void fileCrumb(const char* where) {
    // 设计：低频、每步一条；fopen("a") 自带原子追加语义，多线程安全；
    // 每条都 fclose 落盘——宁慢勿丢，这是给"进程猝死"取证用的。
    static const bool on = [] {
        const char* v = std::getenv("FILE_CRUMBS");
        return v && *v && std::string(v) != "0";
    }();
    if (!on) return;
    if (FILE* f = std::fopen("file_crumbs.log", "a")) {
        const long long now = nowUnix();
        std::fprintf(f, "%lld %s\n", now, where);
        std::fclose(f);
    }
}

const std::string& mediaBaseUrl() {
    static const std::string base = [] {
        if (const char* v = std::getenv("FILE_BASE_URL"); v && *v) {
            std::string s = v;
            while (!s.empty() && s.back() == '/') s.pop_back();
            return s;
        }
        // 默认跟随 HTTP_PORT：验收脚本里 HTTP 端口是环境变量传的，
        // 让"url 里的端口"与"服务实际监听的端口"默认保持一致。
        std::string port = "8080";
        if (const char* p = std::getenv("HTTP_PORT"); p && *p) port = p;
        return std::string("http://127.0.0.1:") + port;
    }();
    return base;
}

} // namespace kc
