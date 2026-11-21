#pragma once
#include <cstdint>
#include <string>

// ============================================================================
// /*************图片文件功能（七牛云直传）：qiniu_signer——签名算法库***************/
// ----------------------------------------------------------------------------
// 这个文件只做一件事：按七牛云 Kodo 的官方签名规范，用 OpenSSL 现算签名。
// 不引入七牛 SDK（项目约束"不引入新第三方库"），但三种签名的规则都是公开的，
// 官方文档要点（实现时逐条对照，注释里给出出处，方便将来核对协议变更）：
//
//   1) 上传凭证 uploadToken
//      https://developer.qiniu.com/kodo/4088/upsafe-kodo  （直传与凭证）
//      https://developer.qiniu.com/kodo/manual/1208/upload-token
//      规则：encodedPolicy = urlsafe_base64(policy_json)
//            sign          = urlsafe_base64( hmac_sha1(SecretKey, encodedPolicy) )
//            uploadToken   = AccessKey + ":" + sign + ":" + encodedPolicy
//      ⚠ 待签串**只有 encodedPolicy**，绝不包含上传 URL 的 path。
//        "policy + \n + encodedUrl" 那一套是 **3) 管理凭证** 的规则，两者混用
//        会让七牛恒定回 401 {"error":"bad token"}（和 AK/SK 对不对没关系）。
//        本项目 2026-11-20 就栽在这里：多拼了 "\n"+b64("/")，导致服务端签的
//        uptoken 与 step3_verify/qiniu_token_probe.py 同时验签失败——而两处
//        用的是同一份错误算法，所以"黄金样例"自检全绿也照样传不上去
//        （自洽 ≠ 与官方规范一致，见下方 3) 与本文件末的对照）。
//      要点：policy 里的 scope 锁死 bucket:key（客户端只能传这一个对象），
//            fsizeLimit 由七牛端强制执行（超限上传直接被七牛拒绝），
//            ⚠⚠ 有效期字段**必须叫 deadline**（绝对 Unix 秒）。官方 SDK 的
//            构造参数里那个 `expires` 是"多少秒之后失效"的**入参**，SDK 会把它
//            换算成 policy JSON 的 deadline 再签——直接把 expires 当 JSON 字段名
//            写进 policy，七牛**整字段忽略**，截止时间按 0 处理，于是每次上传都
//            回 401 {"error":"expired token"}（签名却是好的，极具误导性）。
//            2026-11-20 实测钉死：同一 AK/SK、同一 up-z0，policy 用 expires→401，
//            用 deadline→200，两个都给→200。
//
//   2) 私有空间下载签名（下载凭证 token 方案）
//      依据（本仓库自带的官方 C SDK）：qiniu/rs.c 的 Qiniu_RS_GetPolicy_MakeRequest
//        authstr = baseUrl + ("?"|"&") + "e=" + deadline     // baseUrl **含 scheme://host**
//        token   = AccessKey + ":" + urlsafe_b64(hmac_sha1(SecretKey, authstr))
//        最终 URL = authstr + "&token=" + token              // 与待签串同一个字符串
//      即：**待签串是完整的绝对下载 URL（含 scheme://host/path）+ "?e=" + deadline**。
//      ⚠ 真机 2026-11-21 实测：只签 "/<key>?e=..."（丢掉 scheme://host）时 Kodo 回
//        401 {"error":"download token auth failed"}——签名看着"自洽"（我们自己的
//        mock 也照同样的错口径验得过），但七牛不认。所以本函数刻意**不**规定第三参
//        是 path 还是整条 URL：真实模式必须传绝对 URL（object_store.cpp
//        ::QiniuObjectStore::makeDownloadUrl），mock 传相对路径由自家校验。
//      连带效果：**scheme 也被签进去了**——QiniuConfig.json 的 domain 写 https 就用
//        https 请求，写 http 就用 http 请求，换协议必然验签失败。
//      要点：deadline 过期后 URL 立刻失效——这正是"消息里不能存签名直链"
//            的原因（历史消息会随签名过期而打不开），我们只存服务器换取入口。
//
//   3) 管理 API 鉴权（stat / delete 等，rs.*）
//      出处：本仓库 sdk/qiniu-c-sdk-master/qiniu/auth_mac.c 的 Qiniu_Mac_AuthV2
//      + Qiniu_Mac_HmacV2（Qiniu_Client_Call 见到 itbl->AuthV2 非空就一定走它），
//      即官方「Qiniu 签名算法」v7。规则：
//            signStr = "<METHOD> <Path>[?<Query>]" "\n"
//                      "Host: <Host>" "\n"
//                      ["Content-Type: <CT>" "\n"]
//                      ["X-Qiniu-<X>: <v>" "\n" ...按头名排序]
//                      "\n" [<Body>]              ← 头与 body 之间那个空行必有
//            sign          = urlsafe_base64( hmac_sha1(SecretKey, signStr) )
//            Authorization = "Qiniu " + AccessKey + ":" + sign
//      三个易错点（本项目 2026-11 连着踩过两次，两次都是"自洽但 401"）：
//        · METHOD 与 Path 之间是**空格**，Host 不是单独一行而是 "Host: xxx" 头形式；
//        · 签名串与 body 之间**必须有一个空行**（"\n\n"），漏掉就 BadToken；
//        · 请求里实际发出的 Content-Type / X-Qiniu-* 头必须与签名里出现的行**成对**
//          （发了不签、签了不发，都算签名不符）。X-Qiniu-* 的头名按官方 SDK
//          canonical_mime_header_key 的写法规范化成 "X-Qiniu-Date" 这种首字母大写。
//      另注：<Host> 必须与实际发出的 Host 头逐字节一致（object_store.cpp 的
//        QiniuHttpAgent 用 req.set(field::host, host_)，host_ 即 rs_host，
//        两边同源，不会漂）。
//
// 线程模型：全部是微秒级的纯 CPU 计算（HMAC/Base64/拼接），与 JWT HS256 同性质，
// 允许在 io 线程直接调用；真正会阻塞的外网调用在 qiniu_client（object_store.cpp）。
// ============================================================================

namespace kc {
namespace qsign {

// ---- 编码原语 --------------------------------------------------------------

// 标准 Base64（带 '=' 填充）。
std::string base64Encode(const std::string& data);
// 标准 Base64 解码；失败返回 false（不抛异常，调用方好处理坏输入）。
bool base64Decode(const std::string& text, std::string& out);
// 七牛用的 URL 安全 Base64：'+'->'-'、'/'->'_'，保留 '=' 填充
// （七牛官方实现保留了填充符，逐字节对照样例后确认，改动会导致验签失败）。
std::string urlSafeBase64Encode(const std::string& data);
bool urlSafeBase64Decode(const std::string& text, std::string& out);

// 百分号编码（RFC3986）。keepSlash=true 时 '/' 不转义——
// 七牛的 object key 在 URL 路径里是保留 '/' 的，转义整个 path 是常见错误。
std::string urlEncode(const std::string& s, bool keepSlash = false);

// ---- 哈希/HMAC 原语（返回值都是原始字节，长度由算法决定） -------------------
std::string hmacSha1(const std::string& key, const std::string& data);
std::string hmacSha256(const std::string& key, const std::string& data);
std::string sha1Hex(const std::string& data);   // 管理 API 的 signing_key 用

// ---- 三类签名 --------------------------------------------------------------

// 1) 上传凭证。policyJson 是完整的上传策略 JSON（调用方负责拼 scope/deadline/fsizeLimit）。
//    签名只覆盖 encodedPolicy，所以**没有** uploadPath 参数——上一次有它的时候
//    它被拼进了待签串，结果是所有直传 401 bad token（见文件头 1) 的 ⚠ 段）。
std::string makeUploadToken(const std::string& accessKey,
                            const std::string& secretKey,
                            const std::string& policyJson);

// 2) 私有下载：对 urlWithoutEQuery（**不含 e/token 参数本身**的那半条 URL）补
//    "?e=" + deadline 后整体签名，返回 token 值 "<AccessKey>:<sign>"。
//    ⚠ 真实模式必须传**绝对 URL**（"https://host/key"，含 scheme+host）——七牛签的
//      就是整条 URL，只传 "/key" 会得到 401 download token auth failed；
//      mock 模式传相对路径（由自家 verifyObjectToken 校验，口径自洽即可）。
//    返回的 token 必须接在**同一个待签串**之后（换了 URL 就是换了签名对象）。
std::string makeDownloadToken(const std::string& accessKey,
                              const std::string& secretKey,
                              const std::string& urlWithoutEQuery,
                              long long deadlineUnix);

// 校验我们自己入口 URL 的签名（与 2) 同一算法；用于 GET /api/files/{uuid}?e=&token=）。
// entrySecret 用"签发记录密钥"而非七牛 SK——入口签名和 Qiniu 签名刻意分域，
// 万一入口密钥泄露不影响 bucket 管理权限（最小权限原则）。
bool verifyEntryToken(const std::string& entrySecret,
                      const std::string& pathWithQuery,
                      const std::string& token);

// 3) 管理 API：生成 Authorization 头值（官方 "Qiniu AK:sign" v7，见文件头 3) 规则）。
//    method 大写（"GET"/"POST"）；host 必须与实际发出的 Host 头逐字节一致；
//    path 如 "/stat/<entry>"；query 传空串表示无查询参数，非空时必须是**已按 key
//    排序、已 URL 编码**的原样串（本函数只把它接在 path 后面，不做二次加工）。
//    contentType / xQiniuDate：**调用方必须把传进来的这两个值真的作为请求头发出去**
//    （object_store.cpp::stat/remove 就是这么配的）；空串表示这一行不出现。
//    body 非空时接在末尾那个空行之后。
//    本函数纯输入输出、不含时间（时间戳由调用方算好后传进来），单测天然确定。
std::string makeMacAuthorization(const std::string& accessKey,
                                 const std::string& secretKey,
                                 const std::string& method,
                                 const std::string& host,
                                 const std::string& path,
                                 const std::string& query = {},
                                 const std::string& body = {},
                                 const std::string& contentType = {},
                                 const std::string& xQiniuDate = {});

// 管理 API 的 entry 编码：**urlsafe_base64("<bucket>:<key>")**（官方顺序，
// 见 qiniu/rs.c 的 Concat3(bucket,":",key)）。参数写成 (key, bucket) 只是沿用
// 调用点习惯——拼接顺序以函数体里的注释为准，别按参数顺序猜。
std::string entryBase64(const std::string& key, const std::string& bucket);

} // namespace qsign
} // namespace kc
