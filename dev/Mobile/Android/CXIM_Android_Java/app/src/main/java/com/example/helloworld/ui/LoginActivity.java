package com.example.helloworld.ui;

import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.TextView;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;

import com.example.helloworld.R;
import com.example.helloworld.im.Session;
import com.google.android.material.button.MaterialButton;

/**
 * 登录页。复刻桌面端 LoginWindow 的交互口径：
 * <ul>
 *   <li>行内红字提示，不弹窗打断输入</li>
 *   <li>「记住密码」取消时联动取消「自动登录」；勾选「自动登录」时联动勾上「记住密码」</li>
 *   <li>未勾选协议不允许登录</li>
 *   <li>登录中按钮禁用并显示「登录中...」</li>
 * </ul>
 */
public class LoginActivity extends BaseActivity {

    private EditText etAccount;
    private EditText etPassword;
    private CheckBox cbAutoLogin;
    private CheckBox cbRemember;
    private CheckBox cbAgree;
    private TextView tvError;
    private MaterialButton btnLogin;

    private boolean autoLoginTried;

    @Override
    protected boolean requiresLogin() {
        return false;
    }

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_login);

        // 登录页没有顶栏，但同样要整体避开状态栏：保留原有 56dp 上留白，再往下挪状态栏高度
        protectBarBelowStatusBar(findViewById(R.id.auth_root), 56, 0);

        etAccount = findViewById(R.id.et_account);
        etPassword = findViewById(R.id.et_password);
        cbAutoLogin = findViewById(R.id.cb_auto_login);
        cbRemember = findViewById(R.id.cb_remember);
        cbAgree = findViewById(R.id.cb_agree);
        tvError = findViewById(R.id.tv_error);
        btnLogin = findViewById(R.id.btn_login);

        // 已登录直接进主界面（应用被系统杀掉后重新打开的常见路径）
        if (im != null && im.isLoggedIn()) {
            goMain();
            return;
        }

        Session s = im == null ? null : im.session();
        if (s != null) {
            cbRemember.setChecked(s.remember());
            cbAutoLogin.setChecked(s.autoLogin());
            etAccount.setText(s.savedUsername());
            if (s.remember()) {
                etPassword.setText(s.savedPassword());
            }
            cbAgree.setChecked(true);
        }

        wireCheckboxLinkage();

        btnLogin.setOnClickListener(v -> doLogin());
        findViewById(R.id.tv_register).setOnClickListener(v ->
                startActivity(new Intent(this, RegisterActivity.class)));
        findViewById(R.id.tv_server).setOnClickListener(v ->
                startActivity(new Intent(this, ServerSettingsActivity.class)));
        findViewById(R.id.tv_term).setOnClickListener(v ->
                showAgreement("服务协议", "服务协议正文将在官网提供，此处为占位说明。"));
        findViewById(R.id.tv_privacy).setOnClickListener(v ->
                showAgreement("隐私政策", "隐私政策正文将在官网提供，此处为占位说明。"));

        // 自动登录：账号密码都在本地且勾了自动登录
        if (s != null && s.autoLogin() && s.remember()
                && !s.savedUsername().isEmpty() && !s.savedPassword().isEmpty()) {
            autoLoginTried = true;
            etAccount.post(this::doLogin);
        }
    }

    /** 勾选联动规则与桌面端一致 */
    private void wireCheckboxLinkage() {
        cbRemember.setOnCheckedChangeListener((b, checked) -> {
            if (!checked && cbAutoLogin.isChecked()) {
                cbAutoLogin.setChecked(false);
            }
        });
        cbAutoLogin.setOnCheckedChangeListener((b, checked) -> {
            if (checked && !cbRemember.isChecked()) {
                cbRemember.setChecked(true);
            }
        });
    }

    private void showAgreement(String title, String body) {
        new AlertDialog.Builder(this).setTitle(title).setMessage(body)
                .setPositiveButton("知道了", null).show();
    }

    private void setError(String text) {
        if (text == null || text.isEmpty()) {
            tvError.setVisibility(View.GONE);
            tvError.setText("");
        } else {
            tvError.setVisibility(View.VISIBLE);
            tvError.setText(text);
        }
    }

    private void doLogin() {
        if (im == null) {
            return;
        }
        setError("");
        if (!cbAgree.isChecked()) {
            setError("请先阅读并同意服务协议和隐私政策");
            return;
        }
        String username = etAccount.getText().toString().trim();
        String password = etPassword.getText().toString();
        if (username.isEmpty() || password.isEmpty()) {
            setError("请输入账号和密码");
            return;
        }

        btnLogin.setEnabled(false);
        btnLogin.setText(R.string.login_ing);
        Ui.hideKeyboard(this);

        im.account().login(username, password, (ok, error, userId) -> {
            btnLogin.setEnabled(true);
            btnLogin.setText(R.string.login_action);
            if (!ok) {
                setError(error == null || error.isEmpty() ? "登录失败" : error);
                if (autoLoginTried) {
                    // 自动登录失败不要清掉用户填的密码，只提示
                    autoLoginTried = false;
                }
                return;
            }
            im.session().setLoginPreferences(username, password,
                    cbRemember.isChecked(), cbAutoLogin.isChecked());
            im.startDataPlane();
            goMain();
        });
    }

    private void goMain() {
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }
}
