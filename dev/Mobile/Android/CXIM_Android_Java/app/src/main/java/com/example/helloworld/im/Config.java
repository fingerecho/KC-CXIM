package com.example.helloworld.im;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * 服务端地址与客户端参数。默认值对齐桌面端 CXIM_Qt6_config.ini，
 * 用户可在「服务器设置」里改，改后落到 SharedPreferences。
 */
public final class Config {

    private static final String FILE = "cxim_server";

    // 桌面端 CXIM_Qt6_config.ini 的实际部署地址。
    // 注意协议文档 §1.1/§8.2 写的 8080/8888 是服务端源码默认监听端口，
    // 这台机器上部署时改成了 10002/10001，两个数不是同一回事；界面「服务器设置」可改。
    public static final String DEF_HOST = "110.40.206.253";
    public static final int DEF_HTTP_PORT = 10002;
    public static final int DEF_TCP_PORT = 10001;

    /** 心跳必须显著小于服务端 IDLE_TIMEOUT_SEC（默认 60s），桌面端取 25s */
    public static final int DEF_HEARTBEAT_MS = 25000;
    /** 单次历史拉取条数（协议有效上限 100） */
    public static final int DEF_HISTORY_PAGE = 20;
    /** 上行单帧 body 上限，协议 §5.1 规定超过服务端直接断连 */
    public static final int MAX_UPLINK_BODY = 4096;
    /** 下行帧长度防护 */
    public static final int MAX_DOWNLINK_BODY = 8 * 1024 * 1024;
    /** 表单直传 / 分片直传切换阈值 */
    public static final long FORM_UPLOAD_THRESHOLD = 4L * 1024 * 1024;
    /** 分片大小 */
    public static final int PART_SIZE = 4 * 1024 * 1024;

    private Config() {
    }

    private static SharedPreferences sp(Context c) {
        return c.getApplicationContext().getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    public static String httpHost(Context c) {
        return sp(c).getString("http_host", DEF_HOST);
    }

    public static int httpPort(Context c) {
        return sp(c).getInt("http_port", DEF_HTTP_PORT);
    }

    public static String tcpHost(Context c) {
        return sp(c).getString("tcp_host", DEF_HOST);
    }

    public static int tcpPort(Context c) {
        return sp(c).getInt("tcp_port", DEF_TCP_PORT);
    }

    public static int heartbeatMs(Context c) {
        return sp(c).getInt("heartbeat_ms", DEF_HEARTBEAT_MS);
    }

    public static int historyPage(Context c) {
        return sp(c).getInt("history_page", DEF_HISTORY_PAGE);
    }

    public static void save(Context c, String httpHost, int httpPort,
                            String tcpHost, int tcpPort, int heartbeatMs, int historyPage) {
        sp(c).edit()
                .putString("http_host", httpHost)
                .putInt("http_port", httpPort)
                .putString("tcp_host", tcpHost)
                .putInt("tcp_port", tcpPort)
                .putInt("heartbeat_ms", heartbeatMs)
                .putInt("history_page", historyPage)
                .apply();
    }

    public static String httpBaseUrl(Context c) {
        return "http://" + httpHost(c) + ":" + httpPort(c);
    }
}
