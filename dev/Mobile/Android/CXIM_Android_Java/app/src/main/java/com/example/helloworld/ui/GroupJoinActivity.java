package com.example.helloworld.ui;

import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;

import com.example.helloworld.R;

import java.nio.charset.StandardCharsets;

/**
 * 按群号申请入群（协议 §4.14.7）。服务端没有群搜索接口，群号要向群主索取；
 * 申请状态由群主/管理员在「入群申请」页审批。
 */
public class GroupJoinActivity extends BaseActivity {

    /** 附言最大字节数，与 GroupService.requestJoin 的校验一致 */
    private static final int MAX_MESSAGE_BYTES = 255;

    private EditText etGroupId;
    private EditText etMessage;
    private TextView tvMsgCount;
    private Button btnApply;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.group_activity_join);
        setHeaderTitle(getString(R.string.group_title_join));

        etGroupId = findViewById(R.id.group_et_group_id);
        etMessage = findViewById(R.id.group_et_message);
        tvMsgCount = findViewById(R.id.group_tv_msg_count);
        btnApply = findViewById(R.id.group_btn_apply);

        updateCount();
        etMessage.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                updateCount();
            }
        });

        btnApply.setOnClickListener(v -> submit());

        // 从群信息页/成员卡片带过来的群号直接填好
        long preset = getIntent().getLongExtra(GroupDetailActivity.EXTRA_GROUP_ID, 0L);
        if (preset > 0) {
            etGroupId.setText(String.valueOf(preset));
        }
    }

    private void submit() {
        String raw = etGroupId.getText().toString().trim();
        long gid;
        try {
            gid = Long.parseLong(raw);
        } catch (NumberFormatException e) {
            gid = 0;
        }
        if (gid <= 0) {
            toast(getString(R.string.group_err_group_id_invalid));
            return;
        }
        String message = etMessage.getText().toString().trim();
        if (message.getBytes(StandardCharsets.UTF_8).length > MAX_MESSAGE_BYTES) {
            toast(getString(R.string.group_err_message_too_long));
            return;
        }

        Ui.hideKeyboard(this);
        setBusy(true);
        im.group().requestJoin(gid, message, (ok, error, data) -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            setBusy(false);
            if (!ok) {
                // 原样展示服务端原因：group not found or dismissed / already in group / request already pending
                toast(error == null || error.isEmpty()
                        ? getString(R.string.group_err_op_failed) : error);
                return;
            }
            toast(getString(R.string.group_toast_applied));
            setResult(RESULT_OK);
            finish();
        });
    }

    private void setBusy(boolean busy) {
        btnApply.setEnabled(!busy);
        btnApply.setAlpha(busy ? 0.6f : 1f);
        btnApply.setText(busy
                ? getString(R.string.group_action_applying) : getString(R.string.group_action_apply));
    }

    private void updateCount() {
        tvMsgCount.setText(getString(R.string.group_byte_count,
                etMessage.getText().toString().getBytes(StandardCharsets.UTF_8).length,
                MAX_MESSAGE_BYTES));
    }
}
