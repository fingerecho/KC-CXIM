package com.example.helloworld.im;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

/**
 * HTTP 统一响应封装（协议 §2.2）。
 * ok = (httpStatus==200 && code==0)；httpStatus==0 表示网络层错误，不是业务失败。
 */
public class ApiResult {

    public final boolean ok;
    public final int httpStatus;
    public final int code;
    public final String message;
    public final JsonElement data;

    public ApiResult(boolean ok, int httpStatus, int code, String message, JsonElement data) {
        this.ok = ok;
        this.httpStatus = httpStatus;
        this.code = code;
        this.message = message == null ? "" : message;
        this.data = data;
    }

    public static ApiResult netError(String message) {
        return new ApiResult(false, 0, -1, message, null);
    }

    public static ApiResult fail(int httpStatus, String message) {
        return new ApiResult(false, httpStatus, 1, message, null);
    }

    public static ApiResult success(JsonObject data) {
        return new ApiResult(true, 200, 0, "ok", data);
    }

    /** data 当作对象读，缺失时返回空对象 */
    public JsonObject obj() {
        if (data != null && data.isJsonObject()) {
            return data.getAsJsonObject();
        }
        return new JsonObject();
    }

    public long userId() {
        return Json.num(obj(), "userId");
    }

    /**
     * 面向 UI 的错误文案：服务端 message 为空时回退到状态码描述，
     * 保证任何失败都能在界面上看到原因。
     */
    public String errorText(String fallback) {
        if (!message.isEmpty()) {
            return message;
        }
        if (httpStatus == 0) {
            return "无法连接服务器";
        }
        return fallback;
    }

    @Override
    public String toString() {
        return "ApiResult{ok=" + ok + ", http=" + httpStatus + ", code=" + code
                + ", msg=" + message + "}";
    }
}
