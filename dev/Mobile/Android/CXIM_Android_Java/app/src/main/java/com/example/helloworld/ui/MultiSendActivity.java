package com.example.helloworld.ui;

import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.LayoutInflater;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.example.helloworld.R;
import com.example.helloworld.im.ChatService;
import com.example.helloworld.im.model.FriendInfo;
import com.example.helloworld.im.model.GroupInfo;
import com.example.helloworld.ui.widget.AvatarView;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * 群发消息：一次编辑，逐个目标调用 {@link ChatService#sendText}。
 *
 * <p>发送放在主线程串行推进（每条之间让出一帧），这样顶部能看到「已发送 N/M」的进度，
 * 也不会因为一次点按就静默发出十几条而没有任何反馈。
 */
public class MultiSendActivity extends BaseActivity {

    /** 群发目标：好友或群 */
    private static class Target {
        final long peerId;
        final boolean isGroup;
        final String name;

        Target(long peerId, boolean isGroup, String name) {
            this.peerId = peerId;
            this.isGroup = isGroup;
            this.name = name;
        }

        String key() {
            return ChatService.convKey(peerId, isGroup);
        }
    }

    private final List<Target> friendTargets = new ArrayList<>();
    private final List<Target> groupTargets = new ArrayList<>();
    private final Set<String> selected = new LinkedHashSet<>();

    private LinearLayout containerFriends;
    private LinearLayout containerGroups;
    private TextView tvSectionFriends;
    private TextView tvSectionGroups;
    private TextView tvEmpty;
    private TextView tvProgress;
    private EditText etContent;
    private Button btnSend;

    private final Handler main = new Handler(Looper.getMainLooper());
    private boolean sending;

    /*
     * 群发队列存成字段而不是闭包参数：旋转或进程重建时旧的 postDelayed 链会被 onDestroy
     * 的 removeCallbacksAndMessages 掐断，剩余目标就会静默漏发。
     * 队列 + 已发下标 + 冻结的正文一起写进 savedInstanceState，恢复后从断点继续。
     */
    private List<Target> queue;
    private String queueText = "";
    private int queueIndex;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.chat_activity_multi_send);

        setHeaderTitle(getString(R.string.chat_multi_title));

        containerFriends = findViewById(R.id.container_friends);
        containerGroups = findViewById(R.id.container_groups);
        tvSectionFriends = findViewById(R.id.tv_section_friends);
        tvSectionGroups = findViewById(R.id.tv_section_groups);
        tvEmpty = findViewById(R.id.tv_empty_targets);
        tvProgress = findViewById(R.id.tv_multi_progress);
        etContent = findViewById(R.id.et_multi);
        btnSend = findViewById(R.id.btn_multi_send);

        // 先于列表加载恢复队列（列表加载完会重算按钮可用态）
        resumeQueue(savedInstanceState);

        etContent.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                updateSendEnabled();
            }
        });
        btnSend.setOnClickListener(v -> startSending());
        updateSendEnabled();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!sending) {
            rebuildTargets();
        }
    }

    // ================================================================
    //  目标列表
    // ================================================================

    private void rebuildTargets() {
        friendTargets.clear();
        groupTargets.clear();
        for (FriendInfo f : im.contacts().sortedFriends()) {
            friendTargets.add(new Target(f.userId, false, f.displayName()));
        }
        for (GroupInfo g : im.contacts().sortedGroups()) {
            groupTargets.add(new Target(g.groupId, true, g.displayName()));
        }

        containerFriends.removeAllViews();
        containerGroups.removeAllViews();
        for (Target t : friendTargets) {
            containerFriends.addView(makeRow(t, containerFriends));
        }
        for (Target t : groupTargets) {
            containerGroups.addView(makeRow(t, containerGroups));
        }

        boolean empty = friendTargets.isEmpty() && groupTargets.isEmpty();
        tvEmpty.setVisibility(empty ? View.VISIBLE : View.GONE);
        tvSectionFriends.setVisibility(friendTargets.isEmpty() ? View.GONE : View.VISIBLE);
        tvSectionGroups.setVisibility(groupTargets.isEmpty() ? View.GONE : View.VISIBLE);
        containerFriends.setVisibility(friendTargets.isEmpty() ? View.GONE : View.VISIBLE);
        containerGroups.setVisibility(groupTargets.isEmpty() ? View.GONE : View.VISIBLE);
        updateSelectedCount();
    }

    private View makeRow(@NonNull Target target, @NonNull LinearLayout parent) {
        View row = LayoutInflater.from(this).inflate(R.layout.chat_item_target, parent, false);
        AvatarView avatar = row.findViewById(R.id.avatar_target);
        TextView name = row.findViewById(R.id.tv_target_name);
        TextView desc = row.findViewById(R.id.tv_target_desc);
        CheckBox box = row.findViewById(R.id.cb_target);

        if (target.isGroup) {
            avatar.setGroup(target.peerId, target.name);
            desc.setText(R.string.tab_groups);
        } else {
            avatar.setPerson(target.peerId, target.name);
            desc.setText(R.string.tab_friends);
        }
        name.setText(target.name);
        box.setChecked(selected.contains(target.key()));

        // 复选框在 XML 里已设为不可点击，事件统一由整行处理
        row.setOnClickListener(v -> {
            if (sending) {
                return;
            }
            boolean now = !box.isChecked();
            box.setChecked(now);
            if (now) {
                selected.add(target.key());
            } else {
                selected.remove(target.key());
            }
            updateSelectedCount();
        });
        return row;
    }

    private void updateSelectedCount() {
        setHeaderAction(getString(R.string.chat_multi_selected, selected.size()), null);
        updateSendEnabled();
    }

    private void updateSendEnabled() {
        if (btnSend == null) {
            return;
        }
        boolean hasText = etContent.getText() != null
                && etContent.getText().toString().trim().length() > 0;
        boolean enabled = !sending && hasText && !selected.isEmpty();
        btnSend.setEnabled(enabled);
        btnSend.setAlpha(enabled ? 1f : 0.5f);
    }

    // ================================================================
    //  发送
    // ================================================================

    private void startSending() {
        if (sending) {
            return;
        }
        String text = etContent.getText() == null ? "" : etContent.getText().toString();
        if (text.trim().isEmpty()) {
            toast(getString(R.string.chat_multi_no_text));
            return;
        }
        List<Target> picked = new ArrayList<>();
        for (Target t : friendTargets) {
            if (selected.contains(t.key())) {
                picked.add(t);
            }
        }
        for (Target t : groupTargets) {
            if (selected.contains(t.key())) {
                picked.add(t);
            }
        }
        if (picked.isEmpty()) {
            toast(getString(R.string.chat_multi_no_target));
            return;
        }
        if (!im.chat().isReady()) {
            toast(getString(R.string.chat_not_connected));
        }

        sending = true;
        Ui.hideKeyboard(this);
        updateSendEnabled();
        tvProgress.setVisibility(View.VISIBLE);
        queue = picked;
        queueText = text;
        queueIndex = 0;
        tvProgress.setText(getString(R.string.chat_multi_progress, 0, queue.size()));
        pump();
    }

    /** 逐条推进；每条之间让出主线程，进度文案才刷得出来（发送本身只是入队，不会阻塞） */
    private void pump() {
        if (queue == null) {
            return;
        }
        if (queueIndex >= queue.size()) {
            sending = false;
            tvProgress.setVisibility(View.GONE);
            toast(getString(R.string.chat_multi_done));
            queue = null;
            finish();
            return;
        }
        Target t = queue.get(queueIndex);
        im.chat().sendText(t.peerId, t.isGroup, queueText);
        queueIndex++;
        tvProgress.setText(getString(R.string.chat_multi_progress, queueIndex, queue.size()));
        updateSelectedCount();
        main.postDelayed(this::pump, 40L);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        if (queue != null && queueIndex < queue.size()) {
            long[] peers = new long[queue.size()];
            boolean[] groups = new boolean[queue.size()];
            for (int i = 0; i < queue.size(); i++) {
                peers[i] = queue.get(i).peerId;
                groups[i] = queue.get(i).isGroup;
            }
            outState.putLongArray("queue_peers", peers);
            outState.putBooleanArray("queue_groups", groups);
            outState.putString("queue_text", queueText);
            outState.putInt("queue_index", queueIndex);
        }
    }

    /** 旋转重建后从断点继续，避免剩余目标被静默丢弃 */
    private void resumeQueue(@Nullable Bundle state) {
        if (state == null) {
            return;
        }
        long[] peers = state.getLongArray("queue_peers");
        boolean[] groups = state.getBooleanArray("queue_groups");
        if (peers == null || groups == null || peers.length == 0) {
            return;
        }
        List<Target> q = new ArrayList<>();
        for (int i = 0; i < peers.length; i++) {
            q.add(new Target(peers[i], groups[i], ""));
        }
        queue = q;
        queueText = state.getString("queue_text", "");
        queueIndex = state.getInt("queue_index", 0);
        sending = true;
        updateSendEnabled();
        tvProgress.setVisibility(View.VISIBLE);
        tvProgress.setText(getString(R.string.chat_multi_progress, queueIndex, queue.size()));
        main.postDelayed(this::pump, 40L);
    }

    @Override
    protected void onDestroy() {
        main.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
