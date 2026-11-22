package com.example.helloworld.im;

/**
 * 统一的异步回调（所有实现都保证在主线程触发，UI 可直接刷新控件）。
 */
public interface Callback {
    void onResult(ApiResult r);
}
