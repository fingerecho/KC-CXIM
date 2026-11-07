#pragma once
#include "CXIMAppConfig.h"

#ifdef TENCENT_CLOUD_IM_SDK_VERSION

/** 主程序入口
*/
class TestTencentCloudSDKApp
{
public:
    TestTencentCloudSDKApp();
    ~TestTencentCloudSDKApp();

public:
    /** 运行程序功能
    */
    void Run();
};
#endif // TENCENT_CLOUD_IM_SDK_VERSION
