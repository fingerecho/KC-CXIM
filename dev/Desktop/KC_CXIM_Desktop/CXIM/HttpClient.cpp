#include "HttpClient.h"

#include <cctype>

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
    statusCode = 0;
    responseBody.clear();

#ifdef _WIN32
    const std::wstring host = ToWideString(m_host);
    const std::wstring requestPath = ToWideString(path);
    if (host.empty() || requestPath.empty()) {
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

    HINTERNET request = ::WinHttpOpenRequest(connection, L"POST", requestPath.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (request == nullptr) {
        ::WinHttpCloseHandle(connection);
        ::WinHttpCloseHandle(session);
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    bool success = ::WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
                                        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                                        static_cast<DWORD>(body.size()), 0) != FALSE;
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
    (void)path;
    (void)body;
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
