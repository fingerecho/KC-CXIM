package com.example.helloworld.im;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonSyntaxException;

import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

import okhttp3.HttpUrl;
import okhttp3.Interceptor;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import okhttp3.ResponseBody;

/**
 * HTTP 控制面客户端。
 *
 * <p>要点（对应协议 §2.5 / §2.6 / §6.2）：
 * <ul>
 *   <li>除注册/登录/刷新外，全部带 {@code Authorization: Bearer <access>}</li>
 *   <li>401 时先同步刷新 access token，再重放一次；刷新失败上抛 sessionExpired</li>
 *   <li>429 / 503 按 Retry-After 退避重试（最多 2 次），不计入业务失败</li>
 *   <li>网络层异常退避重试（200/300/400ms），最多 3 次</li>
 *   <li>使用 OkHttp 而非 HttpURLConnection，因为好友备注与群资料接口是 PATCH，
 *       HttpURLConnection 不支持该方法</li>
 * </ul>
 */
public class HttpClient {

    /** 同步刷新凭证，返回是否成功 */
    public interface TokenRefresher {
        boolean refreshSync();
    }

    public interface UnauthorizedListener {
        void onUnauthorized();
    }

    private static final String USER_AGENT = "CXIM-Android/1.0";
    private static final int MAX_NET_RETRY = 2;
    private static final int MAX_RATE_RETRY = 2;

    private final Context appContext;
    private final Session session;
    private final OkHttpClient api;
    private final OkHttpClient transfer;
    private final ExecutorService pool = Executors.newCachedThreadPool();
    private final Handler main = new Handler(Looper.getMainLooper());

    private volatile TokenRefresher refresher;
    private volatile UnauthorizedListener unauthorizedListener;

    public HttpClient(Context context, Session session) {
        this.appContext = context.getApplicationContext();
        this.session = session;
        this.api = new OkHttpClient.Builder()
                .connectTimeout(15, TimeUnit.SECONDS)
                .readTimeout(20, TimeUnit.SECONDS)
                .writeTimeout(20, TimeUnit.SECONDS)
                .retryOnConnectionFailure(true)
                .build();
        // 直传/下载专用：大文件需要更长的读写超时
        this.transfer = new OkHttpClient.Builder()
                .connectTimeout(20, TimeUnit.SECONDS)
                .readTimeout(120, TimeUnit.SECONDS)
                .writeTimeout(120, TimeUnit.SECONDS)
                .retryOnConnectionFailure(true)
                .addNetworkInterceptor(stripForeignAuth())
                .build();
    }

    /**
     * 下载换取入口会 302 到对象存储/CDN 域名（协议 §4.15.3）。
     * OkHttp 跟随重定向时会沿用原请求头，把我们自己的 Bearer 一并送到第三方 host；
     * 签名直链本身已经带 token，不需要我们的凭证，所以这里在真正发请求前摘掉它。
     */
    private Interceptor stripForeignAuth() {
        return chain -> {
            Request req = chain.request();
            String auth = req.header("Authorization");
            if (auth == null) {
                return chain.proceed(req);
            }
            HttpUrl api = HttpUrl.parse(baseUrl());
            String apiHost = api == null ? "" : api.host();
            if (req.url().host().equals(apiHost)) {
                return chain.proceed(req);
            }
            return chain.proceed(req.newBuilder().header("Authorization", null).build());
        };
    }

    public void setTokenRefresher(TokenRefresher r) {
        this.refresher = r;
    }

    public void setUnauthorizedListener(UnauthorizedListener l) {
        this.unauthorizedListener = l;
    }

    public void enqueue(Runnable task) {
        pool.execute(task);
    }

    public Handler mainHandler() {
        return main;
    }

    public String baseUrl() {
        return Config.httpBaseUrl(appContext);
    }

    // ===================================================================
    //  异步门面（UI 一律用这套，回调保证在主线程）
    // ===================================================================

    public void get(String path, Map<String, String> query, Callback cb) {
        async(() -> callSync("GET", path, query, null, true), cb);
    }

    public void post(String path, JsonObject body, Callback cb) {
        async(() -> callSync("POST", path, null, body, true), cb);
    }

    /** 注册/登录/刷新：不带 Bearer，也不触发 401 刷新 */
    public void postPublic(String path, JsonObject body, Callback cb) {
        async(() -> callSync("POST", path, null, body, false), cb);
    }

    public void patch(String path, JsonObject body, Callback cb) {
        async(() -> callSync("PATCH", path, null, body, true), cb);
    }

    public void delete(String path, Callback cb) {
        async(() -> callSync("DELETE", path, null, null, true), cb);
    }

    private interface Job {
        ApiResult work();
    }

    private void async(Job job, Callback cb) {
        pool.execute(() -> {
            ApiResult r;
            try {
                r = job.work();
            } catch (Exception e) {
                r = ApiResult.netError(e.getMessage() == null
                        ? e.getClass().getSimpleName() : e.getMessage());
            }
            final ApiResult fr = r;
            main.post(() -> cb.onResult(fr));
        });
    }

    // ===================================================================
    //  同步核心
    // ===================================================================

    /**
     * 同步发起一次控制面请求，内部处理 401 刷新、429/503 退避与网络重试。
     * 必须在非主线程调用。
     */
    public ApiResult callSync(String method, String path, Map<String, String> query,
                              JsonObject body, boolean auth) {
        int netAttempt = 0;
        int rateAttempt = 0;
        boolean refreshed = false;

        while (true) {
            String url = buildUrl(path, query);
            Request.Builder rb = new Request.Builder().url(url)
                    .header("User-Agent", USER_AGENT)
                    .header("Accept", "application/json");
            if (auth && !session.accessToken().isEmpty()) {
                rb.header("Authorization", "Bearer " + session.accessToken());
            }
            RequestBody rbBody = body == null
                    ? RequestBody.create("", okhttp3.MediaType.get("application/json; charset=utf-8"))
                    : RequestBody.create(body.toString(), okhttp3.MediaType.get("application/json; charset=utf-8"));
            rb.method(method, "GET".equals(method) || "HEAD".equals(method) ? null : rbBody);

            Response resp = null;
            try {
                resp = api.newCall(rb.build()).execute();
                int status = resp.code();

                // ---- 401：先刷新 access token 再重放一次 ----
                if (status == 401 && auth && !refreshed) {
                    TokenRefresher tr = refresher;
                    if (tr != null && tr.refreshSync()) {
                        refreshed = true;
                        continue;
                    }
                    notifyUnauthorized();
                    return new ApiResult(false, 401, 1, "invalid or expired token", null);
                }

                // ---- 429 / 503：按 Retry-After 退避，不计业务失败 ----
                if ((status == 429 || status == 503) && rateAttempt < MAX_RATE_RETRY) {
                    rateAttempt++;
                    long waitMs = clampLong(retryAfterMs(resp), 200, 5000);
                    sleep(waitMs);
                    continue;
                }

                String text = resp.body() == null ? "" : resp.body().string();
                return parse(status, text);
            } catch (IOException | RuntimeException e) {
                if (netAttempt < MAX_NET_RETRY) {
                    netAttempt++;
                    sleep(200L + netAttempt * 100L);
                    continue;
                }
                return ApiResult.netError(e.getMessage() == null ? e.getClass().getSimpleName()
                        : e.getMessage());
            } finally {
                if (resp != null) {
                    resp.close();
                }
            }
        }
    }

    private ApiResult parse(int status, String text) {
        JsonElement root;
        try {
            root = JsonParser.parseString(text == null ? "" : text);
        } catch (JsonSyntaxException e) {
            // 非 JSON（网关 HTML 页 / 空响应）
            String brief = text == null ? "" : text.trim();
            if (brief.length() > 160) {
                brief = brief.substring(0, 160);
            }
            return new ApiResult(false, status, 1,
                    status == 200 ? "响应格式错误" : ("HTTP " + status + " " + brief).trim(), null);
        }
        if (root == null || !root.isJsonObject()) {
            return new ApiResult(false, status, 1, "HTTP " + status, null);
        }
        JsonObject o = root.getAsJsonObject();
        int code = (int) Json.num(o, "code", status == 200 ? 0 : 1);
        String message = Json.str(o, "message");
        if (message.isEmpty()) {
            // 兼容旧口径：部分错误只写 error 字段
            message = Json.str(o, "error");
        }
        JsonElement data = Json.child(o, "data");
        boolean ok = status == 200 && code == 0;
        return new ApiResult(ok, status, code, message, data);
    }

    private String buildUrl(String path, Map<String, String> query) {
        HttpUrl base = HttpUrl.parse(baseUrl() + path);
        if (base == null) {
            return baseUrl() + path;
        }
        HttpUrl.Builder b = base.newBuilder();
        if (query != null) {
            for (Map.Entry<String, String> e : query.entrySet()) {
                if (e.getValue() != null) {
                    b.addQueryParameter(e.getKey(), e.getValue());
                }
            }
        }
        return b.build().toString();
    }

    private static long retryAfterMs(Response resp) {
        String v = resp.header("Retry-After");
        if (v == null) {
            return 1000;
        }
        try {
            return (long) (Double.parseDouble(v.trim()) * 1000);
        } catch (NumberFormatException e) {
            return 1000;
        }
    }

    private void notifyUnauthorized() {
        UnauthorizedListener l = unauthorizedListener;
        if (l != null) {
            main.post(l::onUnauthorized);
        }
    }

    // ===================================================================
    //  数据面（七牛直传 / mock 直传 / 下载）
    // ===================================================================

    /** 原始 POST：用于表单直传、mock-put、分片 complete 等 */
    public ApiResult rawPost(String url, String contentType, byte[] payload,
                             Map<String, String> headers, boolean withBearer) {
        Request.Builder rb = new Request.Builder().url(url)
                .header("User-Agent", USER_AGENT);
        if (withBearer && !session.accessToken().isEmpty()) {
            rb.header("Authorization", "Bearer " + session.accessToken());
        }
        if (headers != null) {
            for (Map.Entry<String, String> e : headers.entrySet()) {
                rb.header(e.getKey(), e.getValue());
            }
        }
        rb.post(RequestBody.create(payload, okhttp3.MediaType.get(contentType)));
        return rawExecute(rb.build());
    }

    /** 原始 POST 且自带 body（表单直传用 MultipartBody） */
    public ApiResult rawPostBody(String url, RequestBody body, Map<String, String> headers,
                                 boolean withBearer) {
        Request.Builder rb = new Request.Builder().url(url)
                .header("User-Agent", USER_AGENT);
        if (withBearer && !session.accessToken().isEmpty()) {
            rb.header("Authorization", "Bearer " + session.accessToken());
        }
        if (headers != null) {
            for (Map.Entry<String, String> e : headers.entrySet()) {
                rb.header(e.getKey(), e.getValue());
            }
        }
        rb.post(body);
        return rawExecute(rb.build());
    }

    /** 原始 PUT：分片上传的一片 */
    public ApiResult rawPut(String url, String contentType, byte[] payload,
                            Map<String, String> headers) {
        Request.Builder rb = new Request.Builder().url(url)
                .header("User-Agent", USER_AGENT);
        if (headers != null) {
            for (Map.Entry<String, String> e : headers.entrySet()) {
                rb.header(e.getKey(), e.getValue());
            }
        }
        rb.put(RequestBody.create(payload, okhttp3.MediaType.get(contentType)));
        return rawExecute(rb.build());
    }

    /** GET 一段二进制（图片缩略图 / 文件下载），自动跟随 302 到签名直链 */
    public byte[] getBytes(String url, boolean withBearer) throws IOException {
        Request.Builder rb = new Request.Builder().url(url)
                .header("User-Agent", USER_AGENT);
        if (withBearer && !session.accessToken().isEmpty()) {
            rb.header("Authorization", "Bearer " + session.accessToken());
        }
        try (Response resp = transfer.newCall(rb.build()).execute()) {
            if (!resp.isSuccessful()) {
                throw new IOException("HTTP " + resp.code());
            }
            ResponseBody b = resp.body();
            if (b == null) {
                throw new IOException("empty body");
            }
            return b.bytes();
        }
    }

    private ApiResult rawExecute(Request request) {
        try (Response resp = transfer.newCall(request).execute()) {
            int status = resp.code();
            String text = resp.body() == null ? "" : new String(resp.body().bytes(),
                    java.nio.charset.StandardCharsets.UTF_8);
            if (status / 100 != 2) {
                String brief = text.length() > 200 ? text.substring(0, 200) : text;
                return new ApiResult(false, status, 1, brief, null);
            }
            return parse(status, text);
        } catch (IOException e) {
            return ApiResult.netError(e.getMessage());
        }
    }

    public Map<String, String> query(String... kv) {
        Map<String, String> m = new LinkedHashMap<>();
        for (int i = 0; i + 1 < kv.length; i += 2) {
            m.put(kv[i], kv[i + 1]);
        }
        return m;
    }

    private static long clampLong(long v, long lo, long hi) {
        return Math.max(lo, Math.min(hi, v));
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
