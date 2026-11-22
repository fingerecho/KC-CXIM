package com.example.helloworld.im;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonSyntaxException;

import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Random;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/**
 * TCP 数据面长连接客户端（协议 §5）。
 *
 * <p>分帧：4 字节大端长度 + UTF-8 JSON body。上行单帧 body 上限 4096（超限服务端直接断连），
 * 下行不设 4096 限制（HistoryResp 可能远大于此），必须严格按长度头精确读取以处理粘包/半包。
 *
 * <p>生命周期：connect → Login(100) → LoginAck(101) → 心跳(3, 25s) → 收发业务帧；
 * 断线后按 {@code min(30s, 1s×2^n) + rand(0,1s)} 抖动退避重连，重连前先换 access token，
 * 避免重连退化成 bcrypt 登录风暴（协议 §4.3 / §6.2）。
 */
public class TcpClient {

    public static final int STATE_DISCONNECTED = 0;
    public static final int STATE_CONNECTING = 1;
    public static final int STATE_CONNECTED = 2;
    public static final int STATE_LOGGED_IN = 3;
    public static final int STATE_FATAL = 4;

    public interface Listener {
        void onStateChanged(int state);

        /** 登录阶段的结果；reason 为服务端 102 帧原文 */
        void onLoginResult(boolean success, String reason);

        /**
         * 登录阶段的致命错误（token 无效/过期）：服务端不会重连，
         * 上层应当尝试 refresh token 后重连，refresh 也失效才判会话过期。
         */
        void onLoginFatal(String reason);

        /** 帧在写入 socket 时失败（异步写线程回报），上层据此把该消息标为发送失败 */
        void onSendFailed(Envelope env, String reason);

        /** 除 LoginAck / 登录期 Error 之外的所有下行帧 */
        void onFrame(Envelope env);

        /**
         * 重连前索取一个可用的 access token。
         * 实现方应在拿到 token 后回调 {@code cb.onToken}；传 null 表示放弃本次重连。
         */
        void onNeedFreshToken(TokenCallback cb);
    }

    public interface TokenCallback {
        void onToken(String accessToken);
    }

    private static final long BACKOFF_CAP_MS = 30_000L;
    private static final long LOGIN_ACK_TIMEOUT_MS = 10_000L;

    private final Listener listener;
    /** 单线程写队列：保证帧顺序，同时把 socket 写移动出主线程 */
    private final ExecutorService writer = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "cxim-tcp-writer");
        t.setDaemon(true);
        return t;
    });
    private final ScheduledExecutorService timer = Executors.newScheduledThreadPool(1, r -> {
        Thread t = new Thread(r, "cxim-tcp-timer");
        t.setDaemon(true);
        return t;
    });
    private final Random random = new Random();

    private volatile String host;
    private volatile int port;
    private volatile String token;
    private volatile int heartbeatMs = Config.DEF_HEARTBEAT_MS;

    private volatile Socket socket;
    private volatile OutputStream out;
    private volatile Thread worker;
    private volatile boolean stopped = true;
    private volatile int state = STATE_DISCONNECTED;
    private volatile boolean loggedIn;

    private long backoffMs = 1000L;
    private ScheduledFuture<?> heartbeatTask;
    private ScheduledFuture<?> loginAckTask;
    private ScheduledFuture<?> reconnectTask;

    public TcpClient(Listener listener) {
        this.listener = listener;
    }

    public void setHeartbeatIntervalMs(int ms) {
        this.heartbeatMs = Math.max(5000, ms);
    }

    public int state() {
        return state;
    }

    public boolean isLoggedIn() {
        return loggedIn;
    }

    // ===================================================================
    //  连接控制
    // ===================================================================

    public void start(String host, int port, String accessToken) {
        stop();
        this.host = host;
        this.port = port;
        this.token = accessToken;
        this.stopped = false;
        this.backoffMs = 1000L;
        connectNow();
    }

    /** token 轮换后用新值重连（协议：refresh 得到的新 access 才能用于 TCP 登录） */
    public void updateToken(String accessToken) {
        this.token = accessToken;
        abortCurrent();
        if (!stopped) {
            connectNow();
        }
    }

    public void stop() {
        stopped = true;
        loggedIn = false;
        cancel(heartbeatTask);
        cancel(loginAckTask);
        cancel(reconnectTask);
        abortCurrent();
        setState(STATE_DISCONNECTED);
    }

    private void connectNow() {
        setState(STATE_CONNECTING);
        worker = new Thread(this::runConnection, "cxim-tcp-" + System.nanoTime());
        worker.setDaemon(true);
        worker.start();
    }

    private void runConnection() {
        Socket s = null;
        try {
            s = new Socket();
            s.setTcpNoDelay(true);      // 协议 P1-5：降低小包延迟
            s.setKeepAlive(true);       // 依赖 TCP 层做死链检测
            s.setSoTimeout(0);
            s.connect(new InetSocketAddress(host, port), 10_000);
            socket = s;
            out = s.getOutputStream();

            setState(STATE_CONNECTED);

            // 登录帧：content 放 access token，from 由服务端以 JWT 的 sub 为准
            Envelope login = Envelope.uplink(MsgType.LOGIN, 0, 0, 0, token, false);
            if (!sendEnvelope(login)) {
                throw new IOException("send login failed");
            }
            armLoginAckTimeout();

            readLoop(new DataInputStream(s.getInputStream()));
        } catch (IOException e) {
            // 连接失败或读循环异常，统一走断线处理
        } finally {
            closeQuietly(s);
            socket = null;
            out = null;
            onConnectionLost();
        }
    }

    private void readLoop(DataInputStream in) throws IOException {
        byte[] header = new byte[4];
        while (!stopped) {
            readFully(in, header, 4);
            int len = ((header[0] & 0xFF) << 24)
                    | ((header[1] & 0xFF) << 16)
                    | ((header[2] & 0xFF) << 8)
                    | (header[3] & 0xFF);
            if (len <= 0 || len > Config.MAX_DOWNLINK_BODY) {
                // 下行合法帧不受 4096 限制，这里只做恶意长度防护
                throw new IOException("bad frame length " + len);
            }
            byte[] body = new byte[len];
            readFully(in, body, len);
            handleBody(new String(body, StandardCharsets.UTF_8));
        }
    }

    private static void readFully(DataInputStream in, byte[] buf, int n) throws IOException {
        int got = 0;
        while (got < n) {
            int r = in.read(buf, got, n - got);
            if (r < 0) {
                throw new EOFException("connection closed by peer");
            }
            got += r;
        }
    }

    private void handleBody(String json) {
        JsonObject o;
        try {
            o = JsonParser.parseString(json).getAsJsonObject();
        } catch (JsonSyntaxException | IllegalStateException | ClassCastException e) {
            // 空或非法 JSON：忽略该帧，不断连接
            return;
        }
        Envelope env = Envelope.fromJson(o);

        if (env.type == MsgType.LOGIN_ACK) {
            cancel(loginAckTask);
            loggedIn = true;
            backoffMs = 1000L;
            setState(STATE_LOGGED_IN);
            startHeartbeat();
            mainPost(() -> listener.onLoginResult(true, ""));
            return;
        }
        if (env.type == MsgType.ERROR && !loggedIn) {
            // 登录阶段的 Error 属于致命错误：不再自动重连，交由上层决定是否刷新后重试
            loggedIn = false;
            setState(STATE_FATAL);
            stopped = true;
            cancel(loginAckTask);
            final String reason = env.content;
            mainPost(() -> listener.onLoginFatal(reason));
            abortCurrent();
            return;
        }

        mainPost(() -> listener.onFrame(env));
    }

    // ===================================================================
    //  发送
    // ===================================================================

    /**
     * 发送一帧：同步做参数校验，真正的 socket 写入交给单线程写队列。
     *
     * <p>必须异步：UI 线程调 sendText / markRead / 回执，而 Android 从 API 11 起
     * 禁止主线程读写 socket，直接写会抛 NetworkOnMainThreadException 让应用崩溃。
     * 返回值只表示「参数合法且当前有连接、已入队」；写入阶段的失败通过
     * {@link Listener#onSendFailed} 回报。
     */
    public boolean sendEnvelope(Envelope env) {
        JsonObject o = env.toUplinkJson();
        final String json = o.toString();
        final byte[] body = json.getBytes(StandardCharsets.UTF_8);
        if (body.length == 0 || body.length > Config.MAX_UPLINK_BODY) {
            // 协议规定 0 或超 4096 会被服务端直接断连，本地先拒发
            return false;
        }
        Socket s = socket;
        if (s == null || s.isClosed()) {
            return false;
        }
        writer.execute(() -> writeFrame(env, body));
        return true;
    }

    private void writeFrame(Envelope env, byte[] body) {
        OutputStream os = out;
        Socket s = socket;
        if (os == null || s == null || s.isClosed()) {
            notifySendFailed(env, "连接已断开");
            return;
        }
        try {
            synchronized (os) {
                os.write((body.length >>> 24) & 0xFF);
                os.write((body.length >>> 16) & 0xFF);
                os.write((body.length >>> 8) & 0xFF);
                os.write(body.length & 0xFF);
                os.write(body);
                os.flush();
            }
        } catch (IOException | RuntimeException e) {
            notifySendFailed(env, "发送失败");
            abortCurrent();
        }
    }

    private void notifySendFailed(Envelope env, String reason) {
        mainPost(() -> listener.onSendFailed(env, reason));
    }

    // ===================================================================
    //  心跳 / 重连
    // ===================================================================

    private void startHeartbeat() {
        cancel(heartbeatTask);
        heartbeatTask = timer.scheduleWithFixedDelay(() -> {
            if (loggedIn && !stopped) {
                sendEnvelope(Envelope.uplink(MsgType.HEARTBEAT, 0, 0, 0, "", false));
            }
        }, heartbeatMs, heartbeatMs, TimeUnit.MILLISECONDS);
    }

    private void armLoginAckTimeout() {
        cancel(loginAckTask);
        loginAckTask = timer.schedule(() -> {
            if (!loggedIn) {
                abortCurrent();
            }
        }, LOGIN_ACK_TIMEOUT_MS, TimeUnit.MILLISECONDS);
    }

    private void onConnectionLost() {
        boolean wasLoggedIn = loggedIn;
        boolean fatal = state == STATE_FATAL;
        loggedIn = false;
        cancel(heartbeatTask);
        cancel(loginAckTask);
        if (stopped) {
            // 登录致命错误要保留 FATAL 状态，否则上层无法区分「不重连」与「待重连」
            if (!fatal) {
                setState(STATE_DISCONNECTED);
            }
            return;
        }
        setState(STATE_DISCONNECTED);
        if (wasLoggedIn) {
            mainPost(() -> listener.onLoginResult(false, "连接已断开"));
        }
        scheduleReconnect();
    }

    /** 抖动退避：delay = min(30s, 1s×2^n) + rand(0,1s) */
    private void scheduleReconnect() {
        if (stopped || token == null || token.isEmpty()) {
            return;
        }
        long delay = backoffMs + random.nextInt(1000);
        backoffMs = Math.min(backoffMs * 2, BACKOFF_CAP_MS);
        cancel(reconnectTask);
        reconnectTask = timer.schedule(() -> listener.onNeedFreshToken(this::onFreshToken),
                delay, TimeUnit.MILLISECONDS);
    }

    private void onFreshToken(String freshToken) {
        if (stopped) {
            return;
        }
        if (freshToken == null || freshToken.isEmpty()) {
            stopped = true;
            setState(STATE_FATAL);
            mainPost(() -> listener.onLoginFatal("登录已过期"));
            return;
        }
        token = freshToken;
        connectNow();
    }

    // ===================================================================
    //  工具
    // ===================================================================

    private final android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());

    private void mainPost(Runnable r) {
        main.post(r);
    }

    private void setState(int s) {
        if (state == s) {
            return;
        }
        state = s;
        mainPost(() -> listener.onStateChanged(s));
    }

    private void abortCurrent() {
        Socket s = socket;
        if (s != null) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
        socket = null;
        out = null;
    }

    private static void closeQuietly(Socket s) {
        if (s != null) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
    }

    private static void cancel(ScheduledFuture<?> f) {
        if (f != null) {
            f.cancel(false);
        }
    }

    public void shutdown() {
        stop();
        timer.shutdownNow();
        writer.shutdownNow();
    }

    /** 供 sendText 之类在帧超限时回退使用：估算信封 JSON 字节数 */
    public static int envelopeBytes(Envelope env) {
        return env.toUplinkJson().toString().getBytes(StandardCharsets.UTF_8).length;
    }

}
