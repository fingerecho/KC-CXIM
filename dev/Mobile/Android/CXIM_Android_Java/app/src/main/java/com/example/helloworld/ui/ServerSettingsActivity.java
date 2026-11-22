package com.example.helloworld.ui;

import android.os.Bundle;
import android.widget.EditText;

import androidx.annotation.Nullable;

import com.example.helloworld.R;
import com.example.helloworld.im.Config;
import com.google.android.material.button.MaterialButton;

/**
 * 服务器设置：HTTP / TCP 地址与心跳、历史分页参数。
 * 默认值对齐桌面端 CXIM_Qt6_config.ini，方便在真机上直接连开发服务器。
 */
public class ServerSettingsActivity extends BaseActivity {

    private EditText etHttpHost;
    private EditText etHttpPort;
    private EditText etTcpHost;
    private EditText etTcpPort;
    private EditText etHeartbeat;
    private EditText etHistoryPage;

    @Override
    protected boolean requiresLogin() {
        return false;
    }

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_server_settings);
        setHeaderTitle(getString(R.string.server_setting));

        etHttpHost = findViewById(R.id.et_http_host);
        etHttpPort = findViewById(R.id.et_http_port);
        etTcpHost = findViewById(R.id.et_tcp_host);
        etTcpPort = findViewById(R.id.et_tcp_port);
        etHeartbeat = findViewById(R.id.et_heartbeat);
        etHistoryPage = findViewById(R.id.et_history_page);

        fill(Config.httpHost(this), Config.httpPort(this), Config.tcpHost(this), Config.tcpPort(this),
                Config.heartbeatMs(this), Config.historyPage(this));

        MaterialButton save = findViewById(R.id.btn_save);
        save.setOnClickListener(v -> save());
        MaterialButton reset = findViewById(R.id.btn_reset);
        reset.setOnClickListener(v -> fill(Config.DEF_HOST, Config.DEF_HTTP_PORT, Config.DEF_HOST,
                Config.DEF_TCP_PORT, Config.DEF_HEARTBEAT_MS, Config.DEF_HISTORY_PAGE));
    }

    private void fill(String httpHost, int httpPort, String tcpHost, int tcpPort,
                      int heartbeat, int historyPage) {
        etHttpHost.setText(httpHost);
        etHttpPort.setText(String.valueOf(httpPort));
        etTcpHost.setText(tcpHost);
        etTcpPort.setText(String.valueOf(tcpPort));
        etHeartbeat.setText(String.valueOf(heartbeat));
        etHistoryPage.setText(String.valueOf(historyPage));
    }

    private void save() {
        String httpHost = etHttpHost.getText().toString().trim();
        String tcpHost = etTcpHost.getText().toString().trim();
        if (httpHost.isEmpty() || tcpHost.isEmpty()) {
            toast("服务器地址不能为空");
            return;
        }
        int httpPort = parseInt(etHttpPort, Config.DEF_HTTP_PORT);
        int tcpPort = parseInt(etTcpPort, Config.DEF_TCP_PORT);
        int heartbeat = parseInt(etHeartbeat, Config.DEF_HEARTBEAT_MS);
        int historyPage = parseInt(etHistoryPage, Config.DEF_HISTORY_PAGE);
        if (httpPort <= 0 || httpPort > 65535 || tcpPort <= 0 || tcpPort > 65535) {
            toast("端口需在 1~65535 之间");
            return;
        }
        // 心跳必须显著小于服务端 IDLE_TIMEOUT_SEC（默认 60s），钳到 5~55 秒
        heartbeat = Math.max(5000, Math.min(heartbeat, 55000));
        historyPage = Math.max(1, Math.min(historyPage, 100));

        Config.save(this, httpHost, httpPort, tcpHost, tcpPort, heartbeat, historyPage);

        // 地址变了必须重建长连接；已登录状态下最稳妥的做法是回到登录页重新登录
        if (im != null && im.isLoggedIn()) {
            im.logout();
            toast("已保存，请重新登录");
            Ui.gotoLogin(this);
        } else {
            toast("已保存");
            finish();
        }
    }

    private static int parseInt(EditText et, int def) {
        try {
            return Integer.parseInt(et.getText().toString().trim());
        } catch (NumberFormatException e) {
            return def;
        }
    }
}
