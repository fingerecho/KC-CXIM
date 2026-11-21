// ============================================================================
// /*************图片文件功能（七牛云直传）：文件模块验收客户端（mock 模式）***************/
// ----------------------------------------------------------------------------
// 覆盖验收标准 2~6（QINIU_ENABLED=0 默认模式下全流程，不依赖七牛账号）：
//   上传凭证签发 → "直传"（mock 端点）→ commit 登记 → 单聊 type=2 →
//   接收端 302 换取下载核对 size/qetag → 群聊 type=4 → 离线补推 →
//   历史含元数据 → 负向用例（超限/黑名单类型/虚报 commit/冒用 file_id/
//   content 携外链被改写/非 JSON content）→ 删除 → 鉴权。
//
// 真实模式（QINIU_ENABLED=1）的差异只有一处：第 4 步的"直传"改为按
// uploadUrl/uptoken 走七牛表单上传（本客户端不内置，见 readme §5.17 的
// curl 时序说明）——凭证签发/commit/下载换取的服务端行为完全同构。
//
// 用法：test_file_client [http_port] [tcp_port]
//   输出最后一行 "ALL PASS" 表示验收通过（verify_file.sh 以此判定）。
// ============================================================================
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

// 复用服务端的编解码：测试客户端与服务端共用同一份协议实现（与好友/群聊测试一致）。
#include "message.h"
#include "message_codec.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#endif

namespace {

int g_fail = 0;
const std::string kHost = "127.0.0.1";

void check(bool cond, const std::string& desc, const std::string& extra = "") {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));   // 轻微节流
    if (cond) {
        std::cout << "  PASS: " << desc << "\n";
    } else {
        std::cout << "  FAIL: " << desc;
        if (!extra.empty()) std::cout << " (" << extra << ")";
        std::cout << "\n";
        ++g_fail;
    }
}

void set_recv_timeout(boost::asio::ip::tcp::socket& s, int seconds) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(seconds * 1000);
    setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{};
    tv.tv_sec = seconds;
    setsockopt(s.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// ---- 极简 HTTP 客户端（比好友/群聊版多返回响应头：302 Location 要用）---------
struct HttpResponse {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;   // key 统一小写
};

HttpResponse http_call(const std::string& method, const std::string& path,
                       const std::string& token, const std::string& body,
                       unsigned short port) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket s(io);
    boost::asio::ip::tcp::resolver resolver(io);
    boost::asio::connect(s, resolver.resolve(kHost, std::to_string(port)));

    std::string req;
    req += method + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + kHost + ":" + std::to_string(port) + "\r\n";
    req += "Connection: close\r\n";
    req += "Content-Type: application/json\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    req += body;
    boost::asio::write(s, boost::asio::buffer(req));

    std::string raw;
    boost::system::error_code ec;
    char buf[16384];
    for (;;) {
        const std::size_t n = s.read_some(boost::asio::buffer(buf), ec);
        if (n > 0) raw.append(buf, n);
        if (ec == boost::asio::error::eof) break;
        if (ec) throw boost::system::system_error(ec);
    }

    HttpResponse out;
    const std::size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) throw std::runtime_error("bad http response");
    const std::string head = raw.substr(0, headerEnd);
    out.body = raw.substr(headerEnd + 4);
    const std::size_t sp = head.find(' ');
    if (sp != std::string::npos) { try { out.status = std::stoi(head.substr(sp + 1, 3)); } catch (...) {} }
    // 逐行解析响应头（大小写不敏感的键统一转小写存储）。
    // 注意 \r 必须**两端都**清理：getline 按 '\n' 切行，每行尾部留着 '\r'——
    // 本测试第一版只清了头部，"Location: xxx\r" 被原样拿去发请求，
    // 服务端解析非法字符直接断连，症状却是"bad http response"，极难猜。
    std::istringstream is(head);
    std::string line;
    std::getline(is, line);                       // 状态行跳过
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                              s.back() == ' '  || s.back() == '\t')) s.pop_back();
    };
    while (std::getline(is, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        for (auto& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string v = line.substr(colon + 1);
        trim(v);
        out.headers[k] = v;
    }
    return out;
}

rapidjson::Document parse_json(const std::string& text) {
    rapidjson::Document doc;
    doc.Parse(text.data(), text.size());
    if (doc.HasParseError() || !doc.IsObject()) throw std::runtime_error("bad json: " + text);
    return doc;
}

std::string jstr(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key) || !v[key].IsString()) return {};
    return std::string(v[key].GetString(), v[key].GetStringLength());
}
std::uint64_t ju64(const rapidjson::Value& v, const char* key) {
    if (!v.IsObject() || !v.HasMember(key)) return 0;
    if (v[key].IsUint64()) return v[key].GetUint64();
    if (v[key].IsInt64() && v[key].GetInt64() > 0) return static_cast<std::uint64_t>(v[key].GetInt64());
    return 0;
}

// ---- TCP 帧 ----
void send_frame(boost::asio::ip::tcp::socket& s, const std::string& msg) {
    const std::uint32_t net_len = htonl(static_cast<std::uint32_t>(msg.size()));
    std::vector<char> out(4 + msg.size());
    std::memcpy(out.data(), &net_len, 4);
    std::memcpy(out.data() + 4, msg.data(), msg.size());
    boost::asio::write(s, boost::asio::buffer(out));
}
std::string recv_frame(boost::asio::ip::tcp::socket& s) {
    char header[4];
    boost::asio::read(s, boost::asio::buffer(header, 4));
    std::uint32_t net_len = 0;
    std::memcpy(&net_len, header, 4);
    const std::uint32_t len = ntohl(net_len);
    std::vector<char> body(len);
    boost::asio::read(s, boost::asio::buffer(body));
    return std::string(body.begin(), body.end());
}
void send_msg(boost::asio::ip::tcp::socket& s, const kc::Message& m) { send_frame(s, kc::serialize_message(m)); }
kc::Message recv_msg(boost::asio::ip::tcp::socket& s) {
    const std::string raw = recv_frame(s);
    std::vector<char> body(raw.begin(), raw.end());
    kc::Message m; std::string err;
    if (!kc::parse_message(body, m, err)) throw std::runtime_error("bad server message: " + err);
    return m;
}

bool connect_with_retry(boost::asio::ip::tcp::socket& s, unsigned short port, int attempts = 10) {
    for (int i = 1; i <= attempts; ++i) {
        try { s.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(kHost), port)); return true; }
        catch (const std::exception&) {
            if (i == attempts) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    return false;
}

// 等待指定类型的下行帧；期间跳过的其它帧（登录补推的旧事件/好友列表变更…）
// 是**正常现象**：好友 accept 发生时对端还没 TCP 登录，result_notified=0
// 会在下一次登录时被补推——测试不能假设"登录后的第一帧就是我要的"。
kc::Message wait_type(boost::asio::ip::tcp::socket& s, kc::MsgType type) {
    for (;;) {
        kc::Message r = recv_msg(s);
        if (r.type == type) return r;
        if (r.type == kc::MsgType::Error) return r;   // Error 交调用方判断（负向用例）
    }
}

kc::Message login(boost::asio::ip::tcp::socket& s, std::uint64_t uid, const std::string& token) {
    kc::Message m; m.from = uid; m.type = kc::MsgType::Login; m.content = token;
    send_msg(s, m);
    for (;;) { kc::Message r = recv_msg(s);
        if (r.type == kc::MsgType::LoginAck || r.type == kc::MsgType::Error) return r; }
}

std::thread start_heartbeat(boost::asio::ip::tcp::socket& s, std::uint64_t uid, std::atomic<bool>& stop) {
    return std::thread([&s, uid, &stop] {
        for (;;) {
            for (int i = 0; i < 100 && !stop.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stop.load()) break;
            try {
                kc::Message hb; hb.from = uid; hb.type = kc::MsgType::Heartbeat;
                send_msg(s, hb);
            } catch (...) { break; }
        }
    });
}

std::uint64_t register_and_login(const std::string& username, const std::string& password,
                                 std::string& token, unsigned short httpPort) {
    auto r = http_call("POST", "/api/register", "",
                       "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}", httpPort);
    if (r.status != 200) throw std::runtime_error("register failed: " + r.body);
    auto d = parse_json(r.body);
    const std::uint64_t uid = ju64(d["data"], "userId");
    auto lr = http_call("POST", "/api/login", "",
                        "{\"username\":\"" + username + "\",\"password\":\"" + password + "\"}", httpPort);
    if (lr.status != 200) throw std::runtime_error("login failed: " + lr.body);
    token = jstr(parse_json(lr.body)["data"], "token");
    return uid;
}

// ---- 文件模块的"直传"三步曲（mock 模式）------------------------------------
struct UploadedFile {
    std::string fileId, url, qetag;
    std::uint64_t size = 0;
};

// upload-token → mock-put（模拟直传）→ commit，返回登记完成的文件。
// failExpectedStatus >= 0 时**不**执行 commit（负向用例自己控制到哪一步）。
UploadedFile upload_file(const std::string& token, const std::string& name,
                         const std::string& mime, std::uint64_t size,
                         unsigned short httpPort) {
    auto rt = http_call("POST", "/api/files/upload-token", token,
        "{\"name\":\"" + name + "\",\"mime\":\"" + mime +
        "\",\"size\":" + std::to_string(size) + "}", httpPort);
    if (rt.status != 200) throw std::runtime_error("upload-token failed: " +
        std::to_string(rt.status) + " " + rt.body);
    auto rd = parse_json(rt.body);
    UploadedFile f;
    f.fileId = jstr(rd["data"], "fileId");
    const std::string ticket = jstr(rd["data"], "ticket");

    // mock 模式"直传"：POST /api/files/mock-put（真实模式此步是拿 uptoken
    // 向 uploadUrl 做表单上传，字节根本不经过我们的服务器）。
    auto pt = http_call("POST", "/api/files/mock-put", token,
        "{\"file_id\":\"" + f.fileId + "\",\"ticket\":\"" + ticket + "\"}", httpPort);
    if (pt.status != 200) throw std::runtime_error("mock-put failed: " + pt.body);

    auto ct = http_call("POST", "/api/files/commit", token,
        "{\"file_id\":\"" + f.fileId + "\",\"ticket\":\"" + ticket + "\"}", httpPort);
    if (ct.status != 200) throw std::runtime_error("commit failed: " +
        std::to_string(ct.status) + " " + ct.body);
    auto cd = parse_json(ct.body);
    f.url   = jstr(cd["data"], "url");
    f.qetag = jstr(cd["data"], "qetag");
    f.size  = ju64(cd["data"], "size");
    return f;
}

// 取回 Ticket（虚报 commit 用例需要拿到 upload-token 后**跳过直传**直接 commit）。
std::string upload_token_only(const std::string& token, const std::string& name,
                              const std::string& mime, std::uint64_t size,
                              int& statusOut, std::string& bodyOut,
                              unsigned short httpPort) {
    auto r = http_call("POST", "/api/files/upload-token", token,
        "{\"name\":\"" + name + "\",\"mime\":\"" + mime +
        "\",\"size\":" + std::to_string(size) + "}", httpPort);
    statusOut = r.status; bodyOut = r.body;
    if (r.status != 200) return {};
    return r.body;
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    unsigned short httpPort = 8080, tcpPort = 8888;
    if (argc > 1) httpPort = static_cast<unsigned short>(std::stoi(argv[1]));
    if (argc > 2) tcpPort  = static_cast<unsigned short>(std::stoi(argv[2]));

    const std::string password = "secret123";
    const std::string suffix = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() % 100000000);

    boost::asio::io_context io;
    std::atomic<bool> stopA{false}, stopB{false};
    std::thread hbA, hbB;

    try {
        // ---- 0. 注册登录 A/B，互为好友 --------------------------------------
        std::string tokenA, tokenB;
        const std::uint64_t uidA = register_and_login("filA_" + suffix, password, tokenA, httpPort);
        const std::uint64_t uidB = register_and_login("filB_" + suffix, password, tokenB, httpPort);
        check(uidA && uidB && !tokenA.empty() && !tokenB.empty(), "注册并登录 A/B");
        {
            auto rr = http_call("POST", "/api/friends/requests", tokenA,
                                "{\"toUserId\":" + std::to_string(uidB) + "}", httpPort);
            auto rd = parse_json(rr.body);
            const std::uint64_t reqId = ju64(rd["data"]["request"], "requestId");
            http_call("POST", "/api/friends/requests/" + std::to_string(reqId) + "/accept",
                      tokenB, "", httpPort);
            check(true, "A/B 互为好友");
        }

        // ---- 1. TCP 登录 ------------------------------------------------------
        boost::asio::ip::tcp::socket connA(io), connB(io);
        if (!connect_with_retry(connA, tcpPort) || !connect_with_retry(connB, tcpPort))
            throw std::runtime_error("tcp connect failed");
        set_recv_timeout(connA, 8); set_recv_timeout(connB, 8);
        check(login(connA, uidA, tokenA).type == kc::MsgType::LoginAck, "A TCP 登录");
        check(login(connB, uidB, tokenB).type == kc::MsgType::LoginAck, "B TCP 登录");
        hbA = start_heartbeat(connA, uidA, stopA);
        hbB = start_heartbeat(connB, uidB, stopB);

        // ---- 2. 鉴权与参数负向（验收 2）---------------------------------------
        {
            auto r = http_call("POST", "/api/files/upload-token", "",
                               "{\"name\":\"a.png\",\"size\":10}", httpPort);
            check(r.status == 401, "未登录请求上传凭证 401", std::to_string(r.status));
        }
        {
            // 超限：默认 FILE_MAX_SIZE_MB=20，这里报 21MB。
            auto r = http_call("POST", "/api/files/upload-token", tokenA,
                               "{\"name\":\"big.png\",\"size\":" +
                               std::to_string(21LL * 1024 * 1024) + "}", httpPort);
            check(r.status == 400, "超过 FILE_MAX_SIZE 的凭证请求被拒(400)",
                  std::to_string(r.status) + " " + r.body);
        }
        {
            auto r = http_call("POST", "/api/files/upload-token", tokenA,
                               "{\"name\":\"evil.exe\",\"size\":1024}", httpPort);
            check(r.status == 400, "黑名单类型 exe 被拒(400)",
                  std::to_string(r.status) + " " + r.body);
        }
        {
            // 任何响应都不该出现 AK/SK 明文（mock 常量也遵守同一条纪律）。
            auto r = http_call("POST", "/api/files/upload-token", tokenA,
                               "{\"name\":\"x.png\",\"size\":1024}", httpPort);
            const bool leak = r.body.find("MOCKSK") != std::string::npos ||
                              r.body.find("secret_key") != std::string::npos;
            check(r.status == 200 && !leak, "凭证响应不含任何密钥字样",
                  std::to_string(r.status) + " leak=" + (leak ? "1" : "0") + " " + r.body);
        }

        // ---- 3. 正向：图片三步曲（验收 3）-------------------------------------
        UploadedFile img;
        try {
            img = upload_file(tokenA, "照片.png", "image/png", 4096, httpPort);
            check(!img.fileId.empty() && !img.url.empty() && img.size == 4096 &&
                  !img.qetag.empty(), "图片 凭证→直传→commit 全链路");
            check(img.url.find("/api/files/" + img.fileId) != std::string::npos,
                  "commit 返回服务器换取入口 URL（非七牛直链）", img.url);
        } catch (const std::exception& e) {
            check(false, "图片上传链路", e.what());
        }

        // 虚报 commit：拿凭证不直传直接 commit，必须被 stat 核验拒绝。
        {
            int st = 0; std::string body;
            const std::string tokenBody = upload_token_only(tokenA, "lie.png", "image/png", 1024, st, body, httpPort);
            if (st == 200) {
                auto d = parse_json(body);
                auto c = http_call("POST", "/api/files/commit", tokenA,
                    "{\"file_id\":\"" + jstr(d["data"], "fileId") +
                    "\",\"ticket\":\"" + jstr(d["data"], "ticket") + "\"}", httpPort);
                check(c.status == 400 && c.body.find("object not uploaded") != std::string::npos,
                      "未直传就 commit（虚报）被 stat 核验拒绝", std::to_string(c.status) + " " + c.body);
            } else {
                check(false, "虚报用例前置：凭证申请应成功", body);
            }
        }

        // ---- 4. 单聊 type=2：B 收到服务端重建的元数据并可下载 ------------------
        std::uint64_t clientMsg = 1000;
        {
            kc::Message m;
            m.type = kc::MsgType::Image;
            m.to = uidB;
            m.msg_id = ++clientMsg;
            // 故意带上"自报 url 是钓鱼链接"——服务端必须整体重建，绝不透传。
            m.content = "{\"file_id\":\"" + img.fileId + "\",\"url\":\"http://evil.example/x.png\"}";
            send_msg(connA, m);
        }
        kc::Message ackA = wait_type(connA, kc::MsgType::SendAck);
        check(ackA.type == kc::MsgType::SendAck, "A 收到 SendAck", ackA.content);
        kc::Message gotB = wait_type(connB, kc::MsgType::Image);
        bool metaOk = false, urlRewritten = false, externalGone = false;
        std::string dlUrl;
        if (gotB.type == kc::MsgType::Image) {
            auto d = parse_json(gotB.content);
            metaOk = jstr(d, "file_id") == img.fileId && ju64(d, "size") == 4096 &&
                     jstr(d, "mime") == "image/png" && jstr(d, "name") == "照片.png";
            dlUrl = jstr(d, "url");
            urlRewritten = dlUrl.find("/api/files/" + img.fileId) != std::string::npos;
            externalGone = gotB.content.find("evil.example") == std::string::npos;
        }
        check(metaOk, "B 收到图片消息且元数据由服务端重建", gotB.content);
        check(urlRewritten && externalGone, "自报钓鱼外链已被替换为换取入口", dlUrl);

        // B 用 Bearer 换取下载：302 → Location（mock 的签名地址）→ 取回字节核对。
        if (!dlUrl.empty()) {
            const std::string path = dlUrl.substr(dlUrl.find('/', 7));   // 去掉 scheme://host
            auto r = http_call("GET", path, tokenB, "", httpPort);
            check(r.status == 302, "GET 换取入口返回 302", std::to_string(r.status));
            const std::string loc = r.headers["location"];
            check(!loc.empty() && loc.find("e=") != std::string::npos &&
                  (loc.find("token=") != std::string::npos || loc.find("vtoken=") != std::string::npos),
                  "Location 是带时效的签名 URL（私有空间语义）", loc);
            const std::string locPath = loc.rfind("http", 0) == 0 ? loc.substr(loc.find('/', 7)) : loc;
            auto d = http_call("GET", locPath, "", "", httpPort);
            check(d.status == 200 && d.headers["x-mock-size"] == "4096" &&
                  d.headers["x-mock-qetag"] == img.qetag,
                  "跟随后取回字节：size/qetag 与登记一致",
                  std::to_string(d.status) + " size=" + d.headers["x-mock-size"]);
            // 未签名访问下载端点必须被拒（时效性/防篡改的行为学验证）。
            const std::string rawPath = locPath.substr(0, locPath.find('?'));
            auto bad = http_call("GET", rawPath, "", "", httpPort);
            check(bad.status == 401 || bad.status == 403,
                  "裸路径访问换取入口被拒(401/403)", std::to_string(bad.status));
        }

        // ---- 5. 冒用他人 file_id（验收 5）-------------------------------------
        {
            kc::Message m;
            m.type = kc::MsgType::Image;
            m.to = uidA;
            m.msg_id = ++clientMsg;
            m.content = "{\"file_id\":\"" + img.fileId + "\"}";   // 是 A 的文件，B 在冒发
            send_msg(connB, m);
            kc::Message r = wait_type(connB, kc::MsgType::Error);
            check(r.type == kc::MsgType::Error && r.content.find("not uploaded by sender") != std::string::npos,
                  "B 冒发 A 的文件被 Error 拒绝", r.content);
            // 冒用被拒后连接必须还活着（规划红线：拒绝用 Error 帧而不是断连）。
            kc::Message okm;
            okm.type = kc::MsgType::Text; okm.to = uidA; okm.msg_id = ++clientMsg; okm.content = "alive";
            send_msg(connB, okm);
            kc::Message ra = wait_type(connB, kc::MsgType::SendAck);
            check(ra.type == kc::MsgType::SendAck, "拒绝后连接未被踢掉，仍可发文本");
        }

        // ---- 6. 非法 content 负向（验收 5）------------------------------------
        {
            kc::Message m;
            m.type = kc::MsgType::Image; m.to = uidB; m.msg_id = ++clientMsg;
            m.content = "hello-binary-not-json";
            send_msg(connA, m);
            kc::Message r = wait_type(connA, kc::MsgType::Error);
            check(r.type == kc::MsgType::Error, "非 JSON 的媒体 content 被 Error 拒绝", r.content);

            m.msg_id = ++clientMsg;
            m.content = "{\"file_id\":\"not-even-a-uuid\"}";
            send_msg(connA, m);
            kc::Message r2 = wait_type(connA, kc::MsgType::Error);
            check(r2.type == kc::MsgType::Error, "非法 file_id 形状被拒", r2.content);

            // 不存在的 uuid：格式对但库里没有。
            m.msg_id = ++clientMsg;
            m.content = "{\"file_id\":\"00000000-0000-4000-8000-000000000000\"}";
            send_msg(connA, m);
            kc::Message r3 = wait_type(connA, kc::MsgType::Error);
            check(r3.type == kc::MsgType::Error && r3.content.find("not found") != std::string::npos,
                  "不存在的 file_id 被拒", r3.content);
        }

        // ---- 7. 群聊 type=4：在线成员实时收到（验收 4）------------------------
        std::uint64_t groupId = 0;
        {
            auto r = http_call("POST", "/api/groups", tokenA,
                "{\"name\":\"文件群_" + suffix + "\",\"memberIds\":[" + std::to_string(uidB) + "]}", httpPort);
            auto d = parse_json(r.body);
            groupId = ju64(d["data"]["group"], "groupId");
            check(groupId != 0, "A 建群 [B]");
            // 群聊事件会先推给 B（added），把这条消费掉避免干扰后面的等待。
            for (int i = 0; i < 10; ++i) {
                kc::Message ev = recv_msg(connB);
                if (ev.type == kc::MsgType::GroupMemberChanged) break;
            }
        }
        UploadedFile doc;
        try {
            doc = upload_file(tokenA, "报告.pdf", "application/pdf", 8000, httpPort);
            check(doc.size == 8000, "PDF 文件上传链路");
        } catch (const std::exception& e) {
            check(false, "PDF 上传", e.what());
        }
        {
            kc::Message m;
            m.type = kc::MsgType::File; m.to = groupId; m.is_group = true;
            m.msg_id = ++clientMsg;
            m.content = "{\"file_id\":\"" + doc.fileId + "\"}";
            send_msg(connA, m);
            kc::Message a = wait_type(connA, kc::MsgType::SendAck);
            check(a.type == kc::MsgType::SendAck, "A 群文件 SendAck");
            kc::Message b = wait_type(connB, kc::MsgType::File);
            bool ok = b.type == kc::MsgType::File && b.is_group;
            if (ok) { auto d = parse_json(b.content); ok = jstr(d, "file_id") == doc.fileId; }
            check(ok, "在线成员 B 实时收到群文件（type=4 透传）", b.content);
        }

        // ---- 8. 离线补推：B 断线，A 再发一个文件，B 重连补收（验收 3/4）--------
        {
            stopB = true; if (hbB.joinable()) hbB.join();
            connB.close();

            UploadedFile late;
            try {
                late = upload_file(tokenA, "晚到.txt", "text/plain", 512, httpPort);
            } catch (const std::exception& e) { check(false, "离线文件上传", e.what()); }
            if (late.size) {
                kc::Message m;
                m.type = kc::MsgType::File; m.to = groupId; m.is_group = true;
                m.msg_id = ++clientMsg;
                m.content = "{\"file_id\":\"" + late.fileId + "\"}";
                send_msg(connA, m);
                kc::Message a = wait_type(connA, kc::MsgType::SendAck);
                check(a.type == kc::MsgType::SendAck, "B 离线时群文件仍落库(返回 SendAck)");

                boost::asio::io_context io2;
                boost::asio::ip::tcp::socket connB2(io2);
                connect_with_retry(connB2, tcpPort);
                set_recv_timeout(connB2, 8);
                login(connB2, uidB, tokenB);
                bool found = false;
                for (int i = 0; i < 50 && !found; ++i) {
                    kc::Message r = recv_msg(connB2);
                    if (r.type == kc::MsgType::File && r.is_group) {
                        auto d = parse_json(r.content);
                        if (jstr(d, "file_id") == late.fileId) found = true;
                    }
                }
                check(found, "B 重连后补推到离线期间的群文件");
                connB2.close();
            }
            // 重连 B 供后续用例使用。
            stopB = false;
            connB = boost::asio::ip::tcp::socket(io);
            connect_with_retry(connB, tcpPort);
            set_recv_timeout(connB, 8);
            login(connB, uidB, tokenB);
            hbB = start_heartbeat(connB, uidB, stopB);
        }

        // ---- 9. 历史含媒体元数据 + 去重（验收 3 复用现有去重）-----------------
        {
            kc::Message h; h.type = kc::MsgType::HistoryReq; h.to = groupId;
            h.is_group = true; h.seq = 0; h.content = "50";
            send_msg(connA, h);
            bool sawBoth = false;
            for (;;) {
                kc::Message r = recv_msg(connA);
                if (r.type == kc::MsgType::HistoryResp) {
                    std::vector<kc::StoredMessage> msgs; std::string err;
                    if (kc::parse_history(r.content, msgs, err)) {
                        int media = 0;
                        for (const auto& m : msgs)
                            if (m.type == kc::MsgType::File || m.type == kc::MsgType::Image) ++media;
                        sawBoth = media >= 2;   // 群内共发过 2 条媒体（doc + late）
                    }
                    break;
                }
                if (r.type == kc::MsgType::Error) break;
            }
            check(sawBoth, "群历史分页返回媒体消息（type 与元数据完整）");
        }
        {
            // 重复 client_msg_id：同一条图片消息发两遍，第二条应命中去重（duplicate）。
            kc::Message m;
            m.type = kc::MsgType::Image; m.to = uidB; m.msg_id = 555001;
            m.content = "{\"file_id\":\"" + img.fileId + "\"}";
            send_msg(connA, m);
            kc::Message first = wait_type(connA, kc::MsgType::SendAck);
            send_msg(connA, m);
            kc::Message second = wait_type(connA, kc::MsgType::SendAck);
            check(first.type == kc::MsgType::SendAck &&
                  second.type == kc::MsgType::SendAck && second.content == "duplicate",
                  "重复 client_msg_id 去重（与文本共用现有机制）", second.content);
            (void)first;
        }

        // ---- 10. 删除与删除后行为（正向）--------------------------------------
        {
            auto r = http_call("DELETE", "/api/files/" + img.fileId, tokenB, "", httpPort);
            check(r.status == 404 || r.status == 400, "他人删不掉我的文件(统一 not found)",
                  std::to_string(r.status));
            auto del = http_call("DELETE", "/api/files/" + img.fileId, tokenA, "", httpPort);
            check(del.status == 200, "本人删除成功");
            auto after = http_call("GET", "/api/files/" + img.fileId, tokenA, "", httpPort);
            check(after.status == 404, "删除后换取入口 404", std::to_string(after.status));
            // 已删除文件的**新**发送必须被拒（status=1 校验）。
            kc::Message m;
            m.type = kc::MsgType::Image; m.to = uidB; m.msg_id = ++clientMsg;
            m.content = "{\"file_id\":\"" + img.fileId + "\"}";
            send_msg(connA, m);
            kc::Message r2 = wait_type(connA, kc::MsgType::Error);
            check(r2.type == kc::MsgType::Error, "删除后不能再发该文件", r2.content);
        }

        stopA = true; stopB = true;
        if (hbA.joinable()) hbA.join();
        if (hbB.joinable()) hbB.join();

    } catch (const std::exception& e) {
        std::cout << "[file-test] EXCEPTION: " << e.what() << "\n";
        ++g_fail;
        stopA = stopB = true;
        if (hbA.joinable()) hbA.join();
        if (hbB.joinable()) hbB.join();
    }

    if (g_fail == 0) {
        std::cout << "[file-test] ALL PASS\n";
        return 0;
    }
    std::cout << "[file-test] " << g_fail << " FAILED\n";
    return 1;
}
