// ============================================================================
// 排障工具（FILE_CRUMBS 同族）：文件票据/存储链路的进程内 loopback 压测。
// 背景：服务器上"mock-put 处理完之后进程随机静默死亡"的形态 = 典型堆损坏
// （某处越界写把堆元数据踩坏，之后任意一次 malloc/free 随机爆）。
// 本程序把 put/commit 依赖的全部自有代码（makeTicket / verifyTicket /
// mockPut / mockGet / stat / signUploadToken / makeDownloadUrl /
// verifyObjectToken / 各类 base64/HMAC）在**开启 debug heap 的同一进程**里
// 高速循环上万次——若存在越界写，/MTd 的 _CRTDBG_CHECK 或堆哨兵会当场抓获
// 并报出文件与行号，不需要服务器、网络或数据库。
// 用法：test_file_loopback [轮数，默认 20000]；非 0 退出 = 发现问题。
// ============================================================================
#include "file_types.h"
#include "object_store.h"
#include "qiniu_signer.h"

#include <iostream>
#include <random>
#include <string>
#include <vector>

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

int g_fail = 0;
void expect(bool cond, const char* what) {
    if (!cond) { std::cout << "  FAIL: " << what << "\n"; ++g_fail; }
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_DEBUG)
    // 打开分配追踪 + 每次堆操作都校验：越界写/free 损坏在下一次调用即暴露。
    _CrtSetDbgFlag(_CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    // 关键：把 CRT 堆报告改道到 stderr（否则无调试器时弹对话框/进
    // OutputDebugString，控制台里什么都看不到——服务器排障时也栽过这个）。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    const int rounds = (argc > 1) ? std::stoi(argv[1]) : 20000;

    std::cerr << "[lc] 1 before io" << std::endl;
    boost::asio::io_context io;   // Mock 的 post 目标；本程序不 run 它（stat 的 cb 排队的场景见下）
    std::cerr << "[lc] 2 io ok" << std::endl;
    kc::FileRuntime rt = kc::FileRuntime::fromEnv();
    std::cerr << "[lc] 3 rt ok" << std::endl;
    auto store = kc::makeObjectStorage(rt, io);
    std::cerr << "[lc] 4 store made" << std::endl;
    std::mt19937_64 rng(12345);   // 固定种子：复现性优先于随机性
    std::cerr << "[lc] 5 rng ok" << std::endl;

    std::cout << "[loopback] store=" << store->kind() << " rounds=" << rounds << "\n";
    std::cerr << "[lc] 6 first cout done" << std::endl;

    long long okTickets = 0, okPuts = 0, okStats = 0, okDl = 0;
    for (int i = 0; i < rounds; ++i) {
        std::cerr << "[r" << i << "] start" << std::endl;
        // ① 模拟 upload-token 尾段：uuid、key、uptoken、ticket
        const std::string uuid = kc::generateUuidV4();
        std::cerr << "[r" << i << "] uuid done" << std::endl;
        expect(!uuid.empty(), "uuid non-empty");
        const std::string name = "文件_" + std::to_string(rng() % 100000) + ".png";
        const std::string mime = "image/png";
        const long long declared = 1 + static_cast<long long>(rng() % (20LL * 1024 * 1024));
        std::string normMime;
        expect(kc::mediaTypeAllowed(name, mime, true, normMime), "mediaTypeAllowed");
        std::cerr << "[r" << i << "] whitelist done" << std::endl;
        const std::string key = "im/2026-11/" + uuid + ".png";
        const std::string uptoken = store->signUploadToken(key, declared, kc::nowUnix() + 3600);
        std::cerr << "[r" << i << "] uptoken done" << std::endl;
        expect(!uptoken.empty(), "signUploadToken");
        const std::string ticket = store->makeTicket(uuid, 1000 + (i % 7), key, declared, name, mime, 7200);
        std::cerr << "[r" << i << "] ticket done" << std::endl;
        expect(!ticket.empty(), "makeTicket");

        // ② 模拟 put/commit 的 verifyTicket：正例 + 篡改负例
        std::cerr << "[r" << i << "] vt.pre" << std::endl;
        std::string k2, n2, m2; long long d2 = 0;
        std::cerr << "[r" << i << "] vt.call" << std::endl;
        if (store->verifyTicket(ticket, uuid, 1000 + (i % 7), k2, d2, n2, m2)) {
            ++okTickets;
            expect(k2 == key && n2 == name && m2 == mime && d2 == declared, "ticket roundtrip fields");
        } else {
            expect(false, "verifyTicket must accept honest ticket");
        }
        std::cerr << "[r" << i << "] vt.done" << std::endl;
        {   // 篡改一位：必须被拒
            std::cerr << "[r" << i << "] tam.a" << std::endl;
            std::string bad = ticket;
            std::cerr << "[r" << i << "] tam.b size=" << bad.size() << std::endl;
            const std::size_t at = bad.size() / 3;
            bad[at] = (bad[at] == 'a') ? 'b' : 'a';
            std::cerr << "[r" << i << "] tam.c" << std::endl;
            expect(!store->verifyTicket(bad, uuid, 1000 + (i % 7), k2, d2, n2, m2),
                   "tampered ticket must be rejected");
            std::cerr << "[r" << i << "] tam.d" << std::endl;
            expect(!store->verifyTicket(ticket, uuid, 9999, k2, d2, n2, m2),
                   "wrong uploader must be rejected");
            std::cerr << "[r" << i << "] tam.e" << std::endl;
        }

        // ③ mockPut + stat（走 g_mockIo post；这里直接调 cb 的等价同步路径）
        if (auto* mock = dynamic_cast<kc::MockObjectStore*>(store.get())) {
            mock->mockPut(key, declared);
            std::cerr << "[r" << i << "] put.done" << std::endl;
            ++okPuts;
            long long sz = 0; std::string qe;
            expect(mock->mockGet(key, sz, qe) && sz == declared, "mockGet size");

            // stat 是 post 语义：run 一轮 io_context 让回调落地并销毁
            std::cerr << "[r" << i << "] stat.call" << std::endl;
            mock->stat(key, [&okStats](bool tok, const std::string&, bool ex, long long s, const std::string&) {
                if (tok && ex && s >= 0) ++okStats;
            });
            io.poll();     // 执行已排队的回调（不 run，避免阻塞）
            std::cerr << "[r" << i << "] stat.polled" << std::endl;

            // ④ 下载 URL 生成 + 验签
            kc::FileRecord rec; rec.object_key = key; rec.uuid = uuid; rec.status = 1;
            const std::string url = mock->makeDownloadUrl(rec, 600);
            const auto q = url.find('?');
            const std::string path = url.substr(0, q);
            std::string e = url.substr(q + 1);
            const auto ep = e.find("e="); const auto tp = e.find("token=");
            const std::string eStr = e.substr(ep + 2, e.find('&', ep) - ep - 2);
            const std::string tok = e.substr(tp + 6);
            std::cerr << "[r" << i << "] dl.made" << std::endl;
            if (mock->verifyObjectToken(path + "?e=" + eStr, tok)) ++okDl;
            expect(mock->verifyObjectToken(path + "?e=" + eStr, tok), "download sign verify");
            expect(!mock->verifyObjectToken(path + "?e=" + eStr + "x", tok), "tampered expiry rejected");

            mock->remove(key, [](bool, const std::string&) {});   // 幂等清理，防 map 无限膨胀
            io.poll();
        }

        // ⑤ 入口签名（Bearer 免鉴权路径）往返
        const std::string path = "/api/files/" + uuid;
        const std::string q = store->makeEntryQuery(path, 600);
        const auto epos = q.find("e=");
        const auto vpos = q.find("vtoken=");
        const std::string eStr = q.substr(epos + 2, q.find('&', epos) - epos - 2);
        const std::string vt   = q.substr(vpos + 7);
        expect(store->verifyEntryQuery(path, eStr, vt), "entry query roundtrip");
        expect(!store->verifyEntryQuery(path, eStr, vt + "x"), "entry tamper rejected");

        // ⑥ 元数据 JSON 往返（buildMediaMetaJson / extractFileIdFromContent）
        kc::FileRecord rec; rec.uuid = uuid; rec.uploader_id = 7; rec.name = name;
        rec.size = declared; rec.mime = mime; rec.qetag = "q"; rec.status = 1;
        const std::string meta = kc::buildMediaMetaJson(rec, "http://x:1");
        std::string fid, err;
        expect(kc::extractFileIdFromContent(meta, fid, err) && fid == uuid, "meta roundtrip");

        if ((i + 1) % 2000 == 0) std::cout << "[loopback] " << (i + 1) << " rounds clean\n";
    }

    std::cout << "[loopback] tickets=" << okTickets << " puts=" << okPuts
              << " stats=" << okStats << " dl=" << okDl << "\n";
    if (g_fail) { std::cout << "[loopback] " << g_fail << " FAILED\n"; return 1; }
    std::cout << "[loopback] ALL PASS\n";
    return 0;
}
