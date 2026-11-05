#include "TestTencentCloudSDKApp.h"

#ifdef TENCENT_CLOUD_IM_SDK_VERSION

#include "MainThread.h"

TestTencentCloudSDKApp::TestTencentCloudSDKApp()
{
}

TestTencentCloudSDKApp::~TestTencentCloudSDKApp()
{
}

void TestTencentCloudSDKApp::Run()
{
    // 创建主线程
    MainThread thread;

    // 执行主线程循环
    thread.RunMessageLoop();
}


#endif // TENCENT_CLOUD_IM_SDK_VERSION