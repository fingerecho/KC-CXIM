#pragma once
#include <boost/asio/io_context.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "file_types.h"

// ============================================================================
// /*************图片文件功能（七牛云直传）：ObjectStorage 存储抽象***************/
// ----------------------------------------------------------------------------
// 规划要求"预留七牛云对象存储接口、图片文件不落本地服务器磁盘"。兑现方式：
//
//   * ObjectStorage 抽象接口 = 控制面的全部对外动作：
//       - 签发上传凭证（本地 HMAC 计算，io 线程直接调用）
//       - 校验对象真实存在 stat / 删除对象 remove（外网 HTTP 调用！）
//       - 生成有时效的下载 URL（本地计算）
//   * 两个实现：
//       - QiniuObjectStore：真实七牛 Kodo（私有 bucket + 直传 + 签名下载）。
//       - MockObjectStore：本地内存模拟七牛语义（存在性/大小/删除/签名 URL），
//         由 QINIU_ENABLED=0（默认）选用——没有七牛账号也能跑通
//         "凭证→直传→commit→发消息→302 下载"全流程验收。
//
// 线程纪律（对应 readme §11 的三条纪律）：
//   * sign* / make*Url / ticket 系列全部是微秒级本地计算（OpenSSL HMAC），
//     允许在 io 线程同步调用；
//   * stat/remove 是**外网阻塞 IO**：内部走独立的两线程执行池（QiniuHttpAgent
//     同款结构），绝不借 Db 的 SQL worker —— SQL worker 的任务都持有 DB 连接，
//     让 3 秒超时的外网调用占住连接，等于用七牛故障拖垮数据库吞吐。
//     回调可能在任意线程触发：调用方（http 层）经由 send_json 的 strand
//     切换回本连接，因此这里不需要再包一层 post；
//   * 失败统一回"传输错误"（503 语义）或"不存在"（400 语义），调用方映射状态码。
//
// 无状态"签发记录"（ticket）：
//   upload-token 阶段不写 files 表（规划红线），但 commit 又必须证明
//   "这个 file_id 是刚才是你本人申请的、key/大小没有被人换过"。
//   解法与本项目 JWT 同一思想：把 (uuid|uploader|key|declaredSize|expire)
//   用 HMAC-SHA256 签成一张无状态票据，commit 时验签——防伪造、防换人、防过期，
//   且服务器重启不丢"签发记录"（它是自包含的）。
//   ticketSecret：真实模式取 QiniuConfig.ticket_secret（缺省用 secret_key），
//   mock 模式用固定开发密钥并在启动日志里警告——泄漏后果上限是
//   "伪造一次上传凭证申请"，拿不到七牛 SK，权限边界见 qiniu_signer.h。
// ============================================================================

namespace kc {

// ---------------------------------------------------------------------------
// QiniuConfig：QiniuConfig.json 的结构与加载（风格仿 JwtConfig）。
// 字段（access_key/secret_key/bucket/up_host/domain 必填）：
//   access_key / secret_key   七牛 AK/SK。**绝不打印日志、绝不下发客户端**。
//   ticket_secret             可选：票据/入口签名专用密钥（与 SK 分域）；缺省用 secret_key。
//   bucket / zone             zone：0=z0 华东,1=z1 华北,2=z2 华南,3=na0 北美,4=as0 东南亚
//   up_host                   上传域名，如 "https://up-z0.qiniup.com"（给客户端直传用）
//   domain                    下载域名（含 scheme），绑定七牛空间或 CDN 加速域名
//   private_space             默认 true：下载 URL 必须带签名（规划口径）
//   rs_scheme / rs_host       管理 API，默认 "https" + "rs.qbox.me"
//   ca_bundle                 可选：自定义 CA 文件路径（Windows 下 OpenSSL 不读
//                             系统证书库时的逃生阀，注释见 QiniuHttpAgent）
// ---------------------------------------------------------------------------
struct QiniuConfig {
    std::string access_key;
    std::string secret_key;
    std::string ticket_secret;      // 空 = 用 secret_key
    std::string bucket;
    int         zone = 0;
    std::string up_host;
    std::string domain;
    bool        private_space = true;
    std::string rs_scheme = "https";
    std::string rs_host   = "rs.qbox.me";
    std::string ca_bundle;

    static QiniuConfig load(const std::string& path);   // 失败抛 runtime_error
};

// ---------------------------------------------------------------------------
// ObjectStorage：接口定义。
// ---------------------------------------------------------------------------
class ObjectStorage {
public:
    using StatCb = std::function<void(bool transportOk, const std::string& err,
                                      bool exists,
                                      long long size, const std::string& qetag)>;
    using OpCb   = std::function<void(bool ok, const std::string& err)>;

    virtual ~ObjectStorage() = default;

    virtual const char* kind() const = 0;              // "mock" / "qiniu"
    // mock 专有端点（/api/files/mock-put、/api/files/mock-object/*）只在
    // mock 实现下开放；真实模式访问它们一律 404——不给生产环境留旁路。
    virtual bool isMock() const { return false; }

    // ---- 纯本地计算：io 线程可直接调 ---------------------------------------
    // 上传凭证。fsizeLimit 写进 policy，真实模式下由**七牛端**强制执行。
    virtual std::string signUploadToken(const std::string& key,
                                        long long fsizeLimit,
                                        long long expiresUnix) = 0;
    // 客户端直传目标（真实模式=七牛 uphost；mock 模式=本服务器的模拟端点路径）。
    virtual std::string uploadUrlForClient() const = 0;
    // 生成一条 ttlSec 时效的下载 URL。
    //   真实模式：绝对 URL（domain + 私有签名 e/token）；
    //   mock 模式：相对路径（mock-object 端点 + 同样的 e/token 签名方案）。
    //   两者放进同一个 302 Location 都是合法形态。
    virtual std::string makeDownloadUrl(const FileRecord& rec, long long ttlSec) = 0;

    // ---- 外网调用：回调在任意线程触发，调用方自己切 strand ------------------
    virtual void stat(const std::string& key, StatCb cb) = 0;
    virtual void remove(const std::string& key, OpCb cb) = 0;

    // ---- commit 登记 files 表时要存的"空间归属"元信息 ----------------------
    virtual std::string bucketName() const = 0;
    virtual int         zoneId()     const = 0;

    // ---- 无状态票据（实现共用，密钥来自 ticketSecret_）---------------------
    // name/mime 进票据而不是让 commit 再报一次：凭证申请时已经过白名单校验，
    // 把**校验过的值**签死在票据里，commit 落库的就必然是申请时那份——
    // 客户端连"换个名字再 commit"的机会都没有（防"凭证申请 png、commit 传 exe"
    // 这类偷换，白名单闸门只把一次也够）。
    std::string makeTicket(const std::string& uuid, std::uint64_t uploader,
                           const std::string& key, long long declaredSize,
                           const std::string& name, const std::string& mime,
                           long long ttlSec) const;
    bool verifyTicket(const std::string& ticket, const std::string& uuid,
                      std::uint64_t uploader, std::string& keyOut,
                      long long& sizeOut, std::string& nameOut,
                      std::string& mimeOut) const;

    // ---- 下载入口的免 Bearer 签名（<img src> 场景）-------------------------
    // path 形如 "/api/files/<uuid>"；返回 "?e=<deadline>&vtoken=<sign>"。
    std::string makeEntryQuery(const std::string& path, long long ttlSec) const;
    bool verifyEntryQuery(const std::string& path, const std::string& eStr,
                          const std::string& vtoken) const;

protected:
    explicit ObjectStorage(std::string ticketSecret)
        : ticketSecret_(std::move(ticketSecret)) {}
    std::string ticketSecret_;
};

// ---------------------------------------------------------------------------
// MockObjectStore：内存模拟七牛语义（仅开发/CI）。
// "字节"并不真的传输——mock-put 端点登记 (key,size)，stat 即查此表；
// mock-object GET 返回按登记 size 合成的字节（上限 64KB，见实现注释）。
// 这是刻意的：模拟的是七牛的**协议语义**（凭证→存在→签名→时效），
// 不是七牛的数据面。真实数据面永远只有真实模式。
// ---------------------------------------------------------------------------
class MockObjectStore final : public ObjectStorage {
public:
    explicit MockObjectStore(boost::asio::io_context& mainIo);

    const char* kind() const override { return "mock"; }
    bool isMock() const override { return true; }

    std::string signUploadToken(const std::string& key, long long fsizeLimit,
                                long long expiresUnix) override;
    std::string uploadUrlForClient() const override;
    std::string makeDownloadUrl(const FileRecord& rec, long long ttlSec) override;

    void stat(const std::string& key, StatCb cb) override;
    void remove(const std::string& key, OpCb cb) override;

    // —— mock 端点专用（http_server 里的 mock-put / mock-object 调这些）——
    void mockPut(const std::string& key, long long size);          // 模拟"对象已存在"
    bool mockGet(const std::string& key, long long& size, std::string& qetag);
    // mock-object 的下载签名校验（pathWithE 形如 "/api/files/mock-object/<enc>?e=<deadline>"）。
    bool verifyObjectToken(const std::string& pathWithE, const std::string& tokenParam) const;

    std::string bucketName() const override { return "mock-bucket"; }
    int         zoneId()     const override { return 0; }

    // 断连模拟（验收 6：控制面故障不拖累主链路）。
    void setDown(bool down) { down_.store(down); }

private:
    struct Obj { long long size; std::string qetag; };
    std::mutex                          mu_;
    std::unordered_map<std::string, Obj> objects_;
    std::atomic<bool>                   down_{false};
};

// ---------------------------------------------------------------------------
// QiniuObjectStore：真实七牛 Kodo。
// ---------------------------------------------------------------------------
class QiniuObjectStore final : public ObjectStorage {
public:
    QiniuObjectStore(boost::asio::io_context& mainIo, QiniuConfig cfg);
    ~QiniuObjectStore() override;

    const char* kind() const override { return "qiniu"; }

    std::string signUploadToken(const std::string& key, long long fsizeLimit,
                                long long expiresUnix) override;
    std::string uploadUrlForClient() const override;
    std::string makeDownloadUrl(const FileRecord& rec, long long ttlSec) override;

    void stat(const std::string& key, StatCb cb) override;
    void remove(const std::string& key, OpCb cb) override;

    std::string bucketName() const override { return cfg_.bucket; }
    int         zoneId()     const override { return cfg_.zone; }

private:
    QiniuConfig cfg_;
    std::unique_ptr<class QiniuHttpAgent> agent_;   // 独立线程池的外网调用执行器
};

// 工厂：QINIU_ENABLED 决定实现。真实模式缺 QiniuConfig.json 直接抛异常
// （启动失败比"悄悄退回 mock"诚实——生产上 mock 等于没接存储）。
std::shared_ptr<ObjectStorage> makeObjectStorage(const FileRuntime& rt,
                                                 boost::asio::io_context& mainIo);
// 配置文件路径（QINIU_CONFIG 环境变量可覆盖，默认项目根 QiniuConfig.json）。
std::string qiniuConfigPath();

} // namespace kc
