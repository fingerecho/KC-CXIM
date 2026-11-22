package com.example.helloworld.ui;

import android.os.Bundle;
import android.view.View;
import android.widget.EditText;
import android.widget.TextView;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;

import com.example.helloworld.R;
import com.google.android.material.button.MaterialButton;

import java.nio.charset.StandardCharsets;

/**
 * 注册页。本地校验口径与桌面端 RegisterDialog 完全一致（按 UTF-8 字节数计）：
 * 用户名非空且 ≤64 字节、密码 6~72 字节、两次密码一致。
 */
public class RegisterActivity extends BaseActivity {

    private EditText etUsername;
    private EditText etPassword;
    private EditText etConfirm;
    private TextView tvError;
    private MaterialButton btnDone;

    @Override
    protected boolean requiresLogin() {
        return false;
    }

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_register);

        // 与登录页一致：整页往下挪出状态栏高度，原来的 56dp 留白保持不变
        protectBarBelowStatusBar(findViewById(R.id.auth_root), 56, 0);

        etUsername = findViewById(R.id.et_username);
        etPassword = findViewById(R.id.et_password);
        etConfirm = findViewById(R.id.et_confirm);
        tvError = findViewById(R.id.tv_error);
        btnDone = findViewById(R.id.btn_done);

        findViewById(R.id.btn_cancel).setOnClickListener(v -> finish());
        btnDone.setOnClickListener(v -> submit());
    }

    private void setError(String text) {
        if (text == null || text.isEmpty()) {
            tvError.setVisibility(View.GONE);
        } else {
            tvError.setVisibility(View.VISIBLE);
            tvError.setText(text);
        }
    }

    private static int bytes(String s) {
        return s == null ? 0 : s.getBytes(StandardCharsets.UTF_8).length;
    }

    private void submit() {
        setError("");
        String username = etUsername.getText().toString().trim();
        String password = etPassword.getText().toString();
        String confirm = etConfirm.getText().toString();

        if (username.isEmpty()) {
            setError("用户名不能为空");
            return;
        }
        if (bytes(username) > 64) {
            setError("用户名长度不能超过 64 个字符");
            return;
        }
        if (bytes(password) < 6 || bytes(password) > 72) {
            setError("密码长度需为 6~72 字节（当前 " + bytes(password) + " 字节）");
            return;
        }
        if (!password.equals(confirm)) {
            setError("两次输入的密码不一致");
            return;
        }

        btnDone.setEnabled(false);
        btnDone.setText(R.string.submitting);
        Ui.hideKeyboard(this);

        im.account().register(username, password, (ok, error, userId) -> {
            // 等响应期间用户可能已经返回，对已销毁的 Activity 弹窗会抛 BadTokenException
            if (isFinishing() || isDestroyed()) {
                return;
            }
            btnDone.setEnabled(true);
            btnDone.setText(R.string.register_done);
            if (!ok) {
                setError(error == null || error.isEmpty() ? "注册失败" : error);
                return;
            }
            new AlertDialog.Builder(this)
                    .setTitle("注册成功")
                    .setMessage("注册成功，用户ID: " + userId)
                    .setCancelable(false)
                    .setPositiveButton("去登录", (d, w) -> finish())
                    .show();
        });
    }
}
