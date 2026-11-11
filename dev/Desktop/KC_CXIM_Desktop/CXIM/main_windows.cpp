#if defined (_WIN32) || defined (_WIN64)

#include "duilib/duilib_config_windows.h"
#include "TestApplication.h"
#include "TestTencentCloudSDKApp.h"
#include "HttpClient.h"
#include "Resource.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h> 
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string TrimConfigValue(const std::string& value)
{
    std::string result = value;
    const auto notSpace = [](unsigned char ch) {
        return std::isspace(ch) == 0;
    };
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), notSpace));
    result.erase(std::find_if(result.rbegin(), result.rend(), notSpace).base(), result.end());
    if ((result.size() >= 2) && (result.front() == '"') && (result.back() == '"')) {
        result = result.substr(1, result.size() - 2);
    }
    return result;
}

std::filesystem::path GetExecutableDirectory()
{
    wchar_t buffer[MAX_PATH] = { 0 };
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0) {
        return std::filesystem::path();
    }
    return std::filesystem::path(std::wstring(buffer, length)).parent_path();
}

void LoadClientHttpConfig()
{
    const std::filesystem::path configPath = GetExecutableDirectory() / L"CXIM_Desktop_config.ini";
    std::ifstream file(configPath);
    if (!file.is_open()) {
        auto logger = spdlog::get("im_file_logger");
        if (logger) {
            logger->warn("未找到配置文件: {}", configPath.string());
        }
        return;
    }

    std::string host = HttpClient::Instance().Host();
    uint16_t port = HttpClient::Instance().Port();
    std::string line;
    while (std::getline(file, line)) {
        line = TrimConfigValue(line);
        if (line.empty() || (line[0] == ';') || (line[0] == '#')) {
            continue;
        }
        if ((line.front() == '[') && (line.back() == ']')) {
            continue;
        }
        const size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) {
            continue;
        }
        const std::string key = TrimConfigValue(line.substr(0, equalPos));
        const std::string value = TrimConfigValue(line.substr(equalPos + 1));
        if (key == "host") {
            if (!value.empty()) {
                host = value;
            }
        }
        else if (key == "port") {
            try {
                const int valuePort = std::stoi(value);
                if ((valuePort > 0) && (valuePort <= 65535)) {
                    port = static_cast<uint16_t>(valuePort);
                }
            }
            catch (...) {
            }
        }
    }

    HttpClient::Instance().SetServer(host, port);
    auto logger = spdlog::get("im_file_logger");
    if (logger) {
        logger->info("HTTP 服务配置加载完成，host: {}, port: {}", host, port);
    }
}

} // namespace

void initSpdLog() {
    try {
        // 参数：日志器名称, 文件路径, 单文件最大字节数, 最大备份文件数
        // 1048576 * 5 = 5MB
        auto file_logger = spdlog::rotating_logger_mt("im_file_logger", "logs/im_client.log", 1048576 * 5, 3);

        // 设置输出格式：[年-月-日 时:分:秒.毫秒] [级别] 消息
        file_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

        // 设置日志级别为 debug，方便排查 IM SDK 的问题
        file_logger->set_level(spdlog::level::debug);

        // 立即刷新（可选，确保崩溃时日志不丢失，但会影响性能）
        // file_logger->flush_on(spdlog::level::err); 

    }
    catch (const spdlog::spdlog_ex& ex) {
        // 处理创建失败（比如目录权限不足）
    }
    /***  usage:
    * auto logger = spdlog::get("im_file_logger");
    logger->info("登录成功，UserID: {}", user_id);
    logger->error("TIMLogin 失败，错误码: {}", ret);
    */
}
//定义应用程序的入口点
#if defined (__MINGW32__) || defined (__MINGW64__)
//使用MinGW-w64编译
int APIENTRY WinMain(_In_ HINSTANCE /*hInstance*/,
                     _In_opt_ HINSTANCE /*hPrevInstance*/,
                     _In_ LPSTR    /*lpCmdLine*/,
                     _In_ int       /*nCmdShow*/)
#else
int APIENTRY wWinMain(_In_ HINSTANCE /*hInstance*/,
                      _In_opt_ HINSTANCE /*hPrevInstance*/,
                      _In_ LPWSTR    /*lpCmdLine*/,
                      _In_ int       /*nCmdShow*/)
#endif
{
    initSpdLog();
    LoadClientHttpConfig();
#ifdef TENCENT_CLOUD_IM_SDK_VERSION
    TestTencentCloudSDKApp app;
    app.Run();
#else 
    TestApplication app;
    app.Run();
#endif // TENCENT_CLOUD_IM_SDK_VERSION
    return 0;
}

#endif
