package com.example.helloworld.im;

import com.google.gson.JsonObject;

/**
 * 账号服务：注册 / 登录 / 刷新 access token（协议 §4.1 ~ §4.3）。
 *
 * <p>刷新做成**同步**方法供 {@link HttpClient#setTokenRefresher} 在 worker 线程里调用：
 * 一次 401 只发一次 refresh，其余请求在锁上等结果，避免并发刷新把 refreshToken 轮换打乱。
 */
public class AccountService {

    public interface LoginCb {
        /** @param userId 注册时为新分配的 userId，登录时为当前账号 */
        void onResult(boolean ok, String error, long userId);
    }

    private final HttpClient http;
    private final Session session;
    private final Object refreshLock = new Object();

    public AccountService(HttpClient http, Session session) {
        this.http = http;
        this.session = session;
    }

    public Session session() {
        return session;
    }

    // ===== 注册 =====

    public void register(String username, String password, LoginCb cb) {
        JsonObject body = new JsonObject();
        body.addProperty("username", username);
        body.addProperty("password", password);
        http.postPublic("/api/register", body, r ->
                cb.onResult(r.ok, r.errorText("注册失败"), r.userId()));
    }

    // ===== 登录 =====

    public void login(String username, String password, LoginCb cb) {
        JsonObject body = new JsonObject();
        body.addProperty("username", username);
        body.addProperty("password", password);
        http.postPublic("/api/login", body, r -> {
            if (!r.ok) {
                cb.onResult(false, r.errorText("登录失败"), 0);
                return;
            }
            JsonObject d = r.obj();
            long uid = Json.num(d, "userId");
            String name = Json.str(d, "username");
            if (name.isEmpty()) {
                name = username;
            }
            String access = Json.str(d, "token");
            String refresh = Json.str(d, "refreshToken");
            long expiresIn = Json.num(d, "expiresIn", 86400);
            long refreshExpiresIn = Json.num(d, "refreshExpiresIn", 2592000);
            if (access.isEmpty()) {
                cb.onResult(false, "服务端未返回 token", 0);
                return;
            }
            session.setCredentials(uid, name, access, refresh, expiresIn, refreshExpiresIn);
            cb.onResult(true, "", uid);
        });
    }

    // ===== token 刷新 =====

    /** 同步刷新（HTTP 401 自动重放用：只有临近过期才真的轮换） */
    public boolean refreshSync() {
        return refreshSync(false);
    }

    /**
     * 同步刷新。
     *
     * @param force true=无视「还没临近过期」直接轮换。TCP 登录收到
     *              {@code invalid or expired token} 时必须 force：服务端已经明确拒绝过手上
     *              这个 access，短路返回 true 会让客户端拿同一个被拒的 token 无限重连。
     */
    public boolean refreshSync(boolean force) {
        synchronized (refreshLock) {
            if (!session.hasRefreshToken()) {
                return false;
            }
            // 另一个线程刚刷新过就直接复用
            if (!force && !session.accessTokenExpiring(60)) {
                return true;
            }
            JsonObject body = new JsonObject();
            body.addProperty("refreshToken", session.refreshToken());
            ApiResult r = http.callSync("POST", "/api/token/refresh", null, body, false);
            if (!r.ok) {
                return false;
            }
            JsonObject d = r.obj();
            String access = Json.str(d, "token");
            if (access.isEmpty()) {
                return false;
            }
            session.updateTokens(access, Json.str(d, "refreshToken"),
                    Json.num(d, "expiresIn", 86400),
                    Json.num(d, "refreshExpiresIn", 2592000));
            return true;
        }
    }

    /** 异步刷新（UI 主动重连时用） */
    public void refreshAsync(Callback cb) {
        refreshAsync(false, cb);
    }

    /** @param force 见 {@link #refreshSync(boolean)} */
    public void refreshAsync(boolean force, Callback cb) {
        http.enqueue(() -> {
            boolean ok;
            try {
                ok = refreshSync(force);
            } catch (Exception e) {
                ok = false;
            }
            final boolean f = ok;
            http.mainHandler().post(() -> cb.onResult(f
                    ? ApiResult.success(new JsonObject())
                    : ApiResult.fail(401, "invalid or expired refresh token")));
        });
    }

    /** 退出登录：只清凭证与长连接，服务端没有登出接口 */
    public void logout() {
        session.clear();
    }
}
