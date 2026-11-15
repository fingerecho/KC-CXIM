#ifndef CXIM_HTTP_CLIENT_H_
#define CXIM_HTTP_CLIENT_H_

#include "CXIMAppConfig.h"

#include <cstdint>
#include <string>

#ifdef CXIM_IMS_CORE_SDK_VERSION
#include "ImsJson.h"
#include "ImsTypes.h"
#include <vector>
#endif // CXIM_IMS_CORE_SDK_VERSION

/** 本地 HTTP 服务接口封装
*   默认对接 127.0.0.1:8080，可通过 CXIM_Desktop_config.ini 配置主机和端口。
*/
class HttpClient
{
public:
    struct LoginResult
    {
        uint64_t userId = 0;
        std::string username;
        std::string token;
    };

#ifdef CXIM_IMS_CORE_SDK_VERSION
    /** IMS 接口统一调用的结果
    */
    struct ApiResult
    {
        bool ok = false;            // HTTP 200 且业务 code==0
        bool unauthorized = false;  // HTTP 401，token 无效或过期
        int httpStatus = 0;         // HTTP 状态码，0 表示网络失败
        std::string error;          // 失败原因（优先服务端 message 文案）
    };
#endif // CXIM_IMS_CORE_SDK_VERSION

public:
    static HttpClient& Instance();

    /** 设置服务端主机和端口 */
    void SetServer(const std::string& host, uint16_t port);

    const std::string& Host() const;
    uint16_t Port() const;

    /** 发送 JSON 格式的 POST 请求
    * @param [in] path 请求路径，例如 "/api/register"
    * @param [in] body JSON 请求体
    * @param [out] statusCode HTTP 状态码
    * @param [out] responseBody 响应体
    * @return 请求是否成功完成（不表示业务成功，业务是否成功需判断 statusCode）
    */
    bool PostJson(const std::string& path, const std::string& body,
                  int& statusCode, std::string& responseBody);

    /** 通用请求：method 为 GET/POST/PATCH/DELETE；withAuth=true 且处于 IMS 构建时
    *  携带 Authorization: Bearer <token>（token 从 ImsSession 读取）。
    */
    bool Request(const std::string& method, const std::string& path, const std::string& body,
                 bool withAuth, int& statusCode, std::string& responseBody);

    /** 注册账号：POST /api/register */
    bool Register(const std::string& username, const std::string& password,
                  int& statusCode, std::string& responseBody);

    /** 登录：POST /api/login */
    bool Login(const std::string& username, const std::string& password,
               int& statusCode, std::string& responseBody, LoginResult& loginResult);

#ifdef CXIM_IMS_CORE_SDK_VERSION
    /** 解析统一响应 {code,message,data}：HTTP 状态与业务 code 都要成功；
    *  成功时 dataJson 输出 data 节点（可能为 null）。
    */
    static ApiResult Interpret(int statusCode, const std::string& responseBody, ImsJson& dataJson);

    /** GET 并解析统一响应 */
    ApiResult Get(const std::string& path);
    /** POST 并解析统一响应 */
    ApiResult PostAuth(const std::string& path, const std::string& body, ImsJson& dataJson);
    /** PATCH 并解析统一响应 */
    ApiResult PatchAuth(const std::string& path, const std::string& body);
    /** DELETE 并解析统一响应 */
    ApiResult DeleteAuth(const std::string& path);

    // ---- BE-FRIEND-01~05 业务接口 ----
    /** GET /api/users/search?keyword=&limit= */
    ApiResult SearchUsers(const std::string& keyword, int limit, std::vector<SearchUser>& out);
    /** GET /api/friends */
    ApiResult GetFriends(std::vector<FriendInfo>& out);
    /** GET /api/friends/requests?direction=incoming|outgoing|all */
    ApiResult GetFriendRequests(const std::string& direction, std::vector<FriendRequestInfo>& out);
    /** POST /api/friends/requests */
    ApiResult SendFriendRequest(uint64_t toUserId, const std::string& message, FriendRequestInfo& out);
    /** POST /api/friends/requests/{requestId}/accept */
    ApiResult AcceptFriendRequest(uint64_t requestId);
    /** POST /api/friends/requests/{requestId}/reject */
    ApiResult RejectFriendRequest(uint64_t requestId);
    /** PATCH /api/friends/{friendId} 修改备注 */
    ApiResult SetFriendRemark(uint64_t friendId, const std::string& remark);
    /** DELETE /api/friends/{friendId} */
    ApiResult DeleteFriend(uint64_t friendId);
    /** POST /api/friends/{friendId}/block */
    ApiResult BlockFriend(uint64_t friendId);
    /** POST /api/friends/{friendId}/unblock */
    ApiResult UnblockFriend(uint64_t friendId);
#endif // CXIM_IMS_CORE_SDK_VERSION

private:
    HttpClient() = default;
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

private:
    std::string m_host = "127.0.0.1";
    uint16_t m_port = 8080;
};

#endif //CXIM_HTTP_CLIENT_H_
