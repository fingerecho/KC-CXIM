#if defined (_WIN32) || defined (_WIN64)

#include "duilib/duilib_config_windows.h"
#include "TestApplication.h"
#include "TestTencentCloudSDKApp.h"
#include "Resource.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h> 

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
