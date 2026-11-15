#include "HttpClient.h"

#include <cctype>

#ifdef CXIM_IMS_CORE_SDK_VERSION
#include "ImsSession.h"
#endif

#ifdef _WIN32
#include <Windows.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif
#endif

namespace {

std::string EscapeJsonString(const std::string& value)
{
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += ch;
            break;
        }
    }
    return result;
}

#ifdef _WIN32
std::wstring ToWideString(const std::string& value)
{
    if (value.empty()) {
        return std::wstring();
    }
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 1) {
        return std::wstring();
    }
    std::wstring result(static_cast<size_t>(length - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], length);
    return result;
}
#endif

std::string ExtractJsonString(const std::string& json, const std::string& key)
{
    const std::string keyToken = "\"" + key + "\"";
    const size_t keyPos = json.find(keyToken);
    if (keyPos == std::string::npos) {
        return std::string();
    }
    const size_t colonPos = json.find(':', keyPos + keyToken.size());
    if (colonPos == std::string::npos) {
        return std::string();
    }
    const size_t quotePos = json.find('"', colonPos + 1);
    if (quotePos == std::string::npos) {
        return std::string();
    }

    std::string result;
    for (size_t i = quotePos + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (ch == '\\' && (i + 1) < json.size()) {
            const char next = json[++i];
            switch (next) {
            case 'n':
                result += '\n';
                break;
            case 'r':
                result += '\r';
                break;
            case 't':
                result += '\t';
                break;
            default:
                result += next;
                break;
            }
            continue;
        }
        if (ch == '"') {
            break;
        }
        result += ch;
    }
    return result;
}

uint64_t ExtractJsonUInt64(const std::string& json, const std::string& key)
{
    const std::string keyToken = "\"" + key + "\"";
    const size_t keyPos = json.find(keyToken);
    if (keyPos == std::string::npos) {
        return 0;
    }
    const size_t colonPos = json.find(':', keyPos + keyToken.size());
    if (colonPos == std::string::npos) {
        return 0;
    }
    size_t valuePos = colonPos + 1;
    while ((valuePos < json.size()) && std::isspace(static_cast<unsigned char>(json[valuePos])) != 0) {
        ++valuePos;
    }
    if ((valuePos >= json.size()) || (std::isdigit(static_cast<unsigned char>(json[valuePos])) == 0)) {
        return 0;
    }
    try {
        return std::stoull(json.substr(valuePos));
    }
    catch (...) {
        return 0;
    }
}

} // namespace

HttpClient& HttpClient::Instance()
{
    static HttpClient instance;
    return instance;
}

void HttpClient::SetServer(const std::string& host, uint16_t port)
{
    if (!host.empty()) {
        m_host = host;
    }
    if (port != 0) {
        m_port = port;
    }
}

const std::string& HttpClient::Host() const
{
    return m_host;
}

uint16_t HttpClient::Port() const
{
    return m_port;
}

bool HttpClient::PostJson(const std::string& path, const std::string& body,
                          int& statusCode, std::string& responseBody)
{
    return Request("POST", path, body, false, statusCode, responseBody);
}

bool HttpClient::Request(const std::string& method, const std::string& path, const std::string& body,
                         bool withAuth, int& statusCode, std::string& responseBody)
{
    statusCode = 0;
    responseBody.clear();

#ifdef _WIN32
    const std::wstring host = ToWideString(m_host);
    const std::wstring requestPath = ToWideString(path);
    const std::wstring verb = ToWideString(method);
    if (host.empty() || requestPath.empty() || verb.empty()) {
        return false;
    }

    HINTERNET session = ::WinHttpOpen(L"CXIM-Desktop/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return false;
    }
    ::WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);

    HINTERNET connection = ::WinHttpConnect(session, host.c_str(), m_port, 0);
    if (connection == nullptr) {
        ::WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = ::WinHttpOpenRequest(connection, verb.c_str(), requestPath.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request == nullptr) {
        ::WinHttpCloseHandle(connection);
        ::WinHttpCloseHandle(session);
        return false;
    }

    std::wstring headers = L"Content-Type: application/json\r\nConnection: close\r\n";
#ifdef CXIM_IMS_CORE_SDK_VERSION
    if (withAuth) {
        headers += L"Authorization: Bearer ";
        headers += ToWideString(ImsSession::Instance().Token());
        headers += L"\r\n";
    }
#endif

    const bool hasBody = !body.empty();
    bool success = ::WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1),
                                        hasBody ? const_cast<char*>(body.data()) : nullptr,
                                        static_cast<DWORD>(hasBody ? body.size() : 0),
                                        static_cast<DWORD>(hasBody ? body.size() : 0), 0) != FALSE;
    if (success) {
        success = ::WinHttpReceiveResponse(request, nullptr) != FALSE;
    }
    if (success) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                  WINHTTP_NO_HEADER_INDEX) != FALSE) {
            statusCode = static_cast<int>(status);
        }

        DWORD available = 0;
        while (::WinHttpQueryDataAvailable(request, &available) != FALSE && (available > 0)) {
            std::string buffer(available, '\0');
            DWORD bytesRead = 0;
            if (::WinHttpReadData(request, buffer.data(), available, &bytesRead) == FALSE) {
                break;
            }
            if (bytesRead == 0) {
                break;
            }
            responseBody.append(buffer.data(), bytesRead);
        }
    }

    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connection);
    ::WinHttpCloseHandle(session);
    return success;
#else
    (void)method;
    (void)path;
    (void)body;
    (void)withAuth;
    return false;
#endif
}

bool HttpClient::Register(const std::string& username, const std::string& password,
                          int& statusCode, std::string& responseBody)
{
    const std::string body = "{\"username\":\"" + EscapeJsonString(username) +
                             "\",\"password\":\"" + EscapeJsonString(password) + "\"}";
    return PostJson("/api/register", body, statusCode, responseBody);
}

bool HttpClient::Login(const std::string& username, const std::string& password,
                       int& statusCode, std::string& responseBody, LoginResult& loginResult)
{
    loginResult = LoginResult();

    const std::string body = "{\"username\":\"" + EscapeJsonString(username) +
                             "\",\"password\":\"" + EscapeJsonString(password) + "\"}";
    if (!PostJson("/api/login", body, statusCode, responseBody)) {
        return false;
    }

    loginResult.userId = ExtractJsonUInt64(responseBody, "userId");
    loginResult.username = ExtractJsonString(responseBody, "username");
    loginResult.token = ExtractJsonString(responseBody, "token");
    return true;
}

#ifdef CXIM_IMS_CORE_SDK_VERSION

namespace {

const std::string kEmptyBody;

HttpClient::ApiResult DoAuthRequest(const std::string& method, const std::string& path,
                                    const std::string& body, ImsJson& dataJson)
{
    int statusCode = 0;
    std::string responseBody;
    const bool transportOk = HttpClient::Instance().Request(method, path, body, true, statusCode, responseBody);
    (void)transportOk;
    return HttpClient::Interpret(statusCode, responseBody, dataJson);
}

HttpClient::ApiResult DoAuthGet(const std::string& path, ImsJson& dataJson)
{
    return DoAuthRequest("GET", path, kEmptyBody, dataJson);
}

} // namespace

HttpClient::ApiResult HttpClient::Interpret(int statusCode, const std::string& responseBody, ImsJson& dataJson)
{
    ApiResult result;
    result.httpStatus = statusCode;

    if (statusCode == 401) {
        result.unauthorized = true;
        result.error = "登录已过期，请重新登录";
        return result;
    }
    if (statusCode == 0) {
        result.error = "网络请求失败，请检查服务端连接";
        return result;
    }

    ImsJson root;
    if (!ImsJson::Parse(responseBody, root) || !root.IsObject()) {
        result.error = "响应解析失败 (HTTP " + std::to_string(statusCode) + ")";
        return result;
    }

    const int code = root["code"].AsInt(-1);
    const std::string message = root["message"].AsStringOr(std::string());
    if ((statusCode == 200) && (code == 0)) {
        result.ok = true;
        dataJson = root["data"];
        return result;
    }

    result.error = message.empty() ? ("请求失败 (HTTP " + std::to_string(statusCode) + ")") : message;
    return result;
}

HttpClient::ApiResult HttpClient::Get(const std::string& path)
{
    ImsJson data;
    return DoAuthGet(path, data);
}

HttpClient::ApiResult HttpClient::PostAuth(const std::string& path, const std::string& body, ImsJson& dataJson)
{
    return DoAuthRequest("POST", path, body, dataJson);
}

HttpClient::ApiResult HttpClient::PatchAuth(const std::string& path, const std::string& body)
{
    ImsJson data;
    return DoAuthRequest("PATCH", path, body, data);
}

HttpClient::ApiResult HttpClient::DeleteAuth(const std::string& path)
{
    ImsJson data;
    return DoAuthRequest("DELETE", path, kEmptyBody, data);
}

HttpClient::ApiResult HttpClient::SearchUsers(const std::string& keyword, int limit, std::vector<SearchUser>& out)
{
    ImsJson data;
    const std::string path = "/api/users/search?keyword=" + UrlEncode(keyword) +
                             "&limit=" + std::to_string(limit);
    const ApiResult result = DoAuthGet(path, data);
    if (result.ok) {
        ParseSearchUsers(data, out);
    }
    return result;
}

HttpClient::ApiResult HttpClient::GetFriends(std::vector<FriendInfo>& out)
{
    ImsJson data;
    const ApiResult result = DoAuthGet("/api/friends", data);
    if (result.ok) {
        ParseFriendList(data, out);
    }
    return result;
}

HttpClient::ApiResult HttpClient::GetFriendRequests(const std::string& direction, std::vector<FriendRequestInfo>& out)
{
    ImsJson data;
    const ApiResult result = DoAuthGet("/api/friends/requests?direction=" + UrlEncode(direction), data);
    if (result.ok) {
        ParseFriendRequests(data, out);
    }
    return result;
}

HttpClient::ApiResult HttpClient::SendFriendRequest(uint64_t toUserId, const std::string& message, FriendRequestInfo& out)
{
    ImsJson data;
    const std::string body = "{\"toUserId\":" + std::to_string(toUserId) +
                             ",\"message\":" + ImsJsonEscape(message) + "}";
    const ApiResult result = PostAuth("/api/friends/requests", body, data);
    if (result.ok) {
        ParseOneFriendRequest(data["request"], out);
    }
    return result;
}

HttpClient::ApiResult HttpClient::AcceptFriendRequest(uint64_t requestId)
{
    ImsJson data;
    return PostAuth("/api/friends/requests/" + std::to_string(requestId) + "/accept", "{}", data);
}

HttpClient::ApiResult HttpClient::RejectFriendRequest(uint64_t requestId)
{
    ImsJson data;
    return PostAuth("/api/friends/requests/" + std::to_string(requestId) + "/reject", "{}", data);
}

HttpClient::ApiResult HttpClient::SetFriendRemark(uint64_t friendId, const std::string& remark)
{
    const std::string body = "{\"remark\":" + ImsJsonEscape(remark) + "}";
    return PatchAuth("/api/friends/" + std::to_string(friendId), body);
}

HttpClient::ApiResult HttpClient::DeleteFriend(uint64_t friendId)
{
    return DeleteAuth("/api/friends/" + std::to_string(friendId));
}

HttpClient::ApiResult HttpClient::BlockFriend(uint64_t friendId)
{
    ImsJson data;
    return PostAuth("/api/friends/" + std::to_string(friendId) + "/block", "{}", data);
}

HttpClient::ApiResult HttpClient::UnblockFriend(uint64_t friendId)
{
    ImsJson data;
    return PostAuth("/api/friends/" + std::to_string(friendId) + "/unblock", "{}", data);
}

#endif // CXIM_IMS_CORE_SDK_VERSION
