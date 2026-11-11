#ifndef CXIM_HTTP_CLIENT_H_
#define CXIM_HTTP_CLIENT_H_

#include <cstdint>
#include <string>

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

    /** 注册账号：POST /api/register */
    bool Register(const std::string& username, const std::string& password,
                  int& statusCode, std::string& responseBody);

    /** 登录：POST /api/login */
    bool Login(const std::string& username, const std::string& password,
               int& statusCode, std::string& responseBody, LoginResult& loginResult);

private:
    HttpClient() = default;
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

private:
    std::string m_host = "127.0.0.1";
    uint16_t m_port = 8080;
};

#endif //CXIM_HTTP_CLIENT_H_
