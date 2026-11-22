package com.example.helloworld.im;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * 登录态：access / refresh 双 token（协议 §3.2）与本地登录偏好。
 * 内存中是当前会话真值，SharedPreferences 负责跨进程重启恢复。
 */
public final class Session {

    private static final String FILE = "cxim_session";

    private long userId;
    private String username = "";
    private String accessToken = "";
    private String refreshToken = "";
    /** 绝对毫秒时刻 */
    private long accessExpireMs;
    private long refreshExpireMs;

    private final SharedPreferences sp;

    public Session(Context c) {
        sp = c.getApplicationContext().getSharedPreferences(FILE, Context.MODE_PRIVATE);
        restore();
    }

    private void restore() {
        userId = sp.getLong("userId", 0);
        username = sp.getString("username", "");
        accessToken = sp.getString("accessToken", "");
        refreshToken = sp.getString("refreshToken", "");
        accessExpireMs = sp.getLong("accessExpireMs", 0);
        refreshExpireMs = sp.getLong("refreshExpireMs", 0);
    }

    // ===== 读取 =====
    public long userId() {
        return userId;
    }

    public String username() {
        return username;
    }

    public String accessToken() {
        return accessToken;
    }

    public String refreshToken() {
        return refreshToken;
    }

    public boolean logged() {
        return userId != 0 && !accessToken.isEmpty();
    }

    /**
     * access token 是否临近过期。重连前用它决定是否先走 /api/token/refresh，
     * 避免把重连变成一次 bcrypt 登录风暴（协议 §4.3）。
     */
    public boolean accessTokenExpiring(int graceSec) {
        if (accessToken.isEmpty()) {
            return true;
        }
        return System.currentTimeMillis() >= accessExpireMs - (long) graceSec * 1000;
    }

    public boolean hasRefreshToken() {
        return !refreshToken.isEmpty();
    }

    // ===== 写入 =====
    /** 登录成功后一次性写入全部凭证 */
    public void setCredentials(long uid, String name, String access, String refresh,
                               long expiresInSec, long refreshExpiresInSec) {
        userId = uid;
        username = name == null ? "" : name;
        accessToken = access == null ? "" : access;
        refreshToken = refresh == null ? "" : refresh;
        long now = System.currentTimeMillis();
        accessExpireMs = now + expiresInSec * 1000;
        refreshExpireMs = now + refreshExpiresInSec * 1000;
        persist();
    }

    /** refresh 接口返回后轮换 token */
    public void updateTokens(String access, String refresh,
                             long expiresInSec, long refreshExpiresInSec) {
        accessToken = access == null ? "" : access;
        if (refresh != null && !refresh.isEmpty()) {
            refreshToken = refresh;
        }
        long now = System.currentTimeMillis();
        accessExpireMs = now + expiresInSec * 1000;
        refreshExpireMs = now + refreshExpiresInSec * 1000;
        persist();
    }

    private void persist() {
        sp.edit()
                .putLong("userId", userId)
                .putString("username", username)
                .putString("accessToken", accessToken)
                .putString("refreshToken", refreshToken)
                .putLong("accessExpireMs", accessExpireMs)
                .putLong("refreshExpireMs", refreshExpireMs)
                .apply();
    }

    /** 只清凭证，保留 [login] 偏好（记住密码/自动登录/账号），否则下次登录页要重新输入 */
    public void clear() {
        userId = 0;
        username = "";
        accessToken = "";
        refreshToken = "";
        accessExpireMs = 0;
        refreshExpireMs = 0;
        sp.edit()
                .remove("userId")
                .remove("username")
                .remove("accessToken")
                .remove("refreshToken")
                .remove("accessExpireMs")
                .remove("refreshExpireMs")
                .apply();
    }

    // ===== 登录偏好（与桌面端 CXIM.ini 的 [login] 段一致） =====
    public boolean remember() {
        return sp.getBoolean("remember", false);
    }

    public boolean autoLogin() {
        return sp.getBoolean("autoLogin", false) && remember();
    }

    public String savedUsername() {
        return sp.getString("username_pref", "");
    }

    public String savedPassword() {
        return sp.getString("password_pref", "");
    }

    /** 记住密码关闭时同时关掉自动登录（与桌面端联动规则一致） */
    public void setLoginPreferences(String username, String password,
                                    boolean remember, boolean autoLogin) {
        SharedPreferences.Editor e = sp.edit();
        e.putBoolean("remember", remember);
        e.putBoolean("autoLogin", autoLogin && remember);
        e.putString("username_pref", username == null ? "" : username);
        if (remember) {
            e.putString("password_pref", password == null ? "" : password);
        } else {
            e.remove("password_pref");
        }
        e.apply();
    }

    public void disableAutoLogin() {
        sp.edit().putBoolean("autoLogin", false).apply();
    }
}
