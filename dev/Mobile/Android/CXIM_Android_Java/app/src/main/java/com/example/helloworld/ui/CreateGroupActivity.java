package com.example.helloworld.ui;

import android.content.Intent;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.example.helloworld.R;
import com.example.helloworld.im.model.FriendInfo;
import com.example.helloworld.ui.widget.AvatarView;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * 新建群聊（协议 §4.14.1）：群名 + 好友多选，初始成员必须都是好友。
 *
 * <p>字节上限与服务端一致（群名 128 字节，按 UTF-8 计），本地先拦一层，
 * 免得把明显非法的请求发出去再等一个 400。
 */
public class CreateGroupActivity extends BaseActivity {

    /** 群名最大字节数 */
    private static final int MAX_NAME_BYTES = 128;

    private EditText etName;
    private TextView tvNameCount;
    private TextView tvEmpty;
    private RecyclerView rvFriends;
    private Button btnCreate;

    private final List<FriendInfo> friends = new ArrayList<>();
    /** 用 LinkedHashSet 保证「已选 N 人」的顺序与点击顺序一致，提交时才转 long[] */
    private final Set<Long> selected = new LinkedHashSet<>();

    private FriendAdapter adapter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.group_activity_create);
        setHeaderTitle(getString(R.string.group_title_create));

        etName = findViewById(R.id.group_et_name);
        tvNameCount = findViewById(R.id.group_tv_name_count);
        tvEmpty = findViewById(R.id.group_tv_empty);
        rvFriends = findViewById(R.id.group_rv_friends);
        btnCreate = findViewById(R.id.group_btn_create);

        friends.addAll(im.contacts().sortedFriends());

        adapter = new FriendAdapter();
        rvFriends.setLayoutManager(new LinearLayoutManager(this));
        rvFriends.setAdapter(adapter);

        updateNameCount();
        etName.addTextChangedListener(new SimpleWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                updateNameCount();
            }
        });

        refreshHeader();
        updateCreateEnabled();

        btnCreate.setOnClickListener(v -> submit());
    }

    // ===== 校验与提交 =====

    private void submit() {
        String name = etName.getText().toString().trim();
        if (name.isEmpty()) {
            toast(getString(R.string.group_err_name_empty));
            return;
        }
        if (bytes(name) > MAX_NAME_BYTES) {
            toast(getString(R.string.group_err_name_too_long));
            return;
        }
        if (selected.isEmpty()) {
            toast(getString(R.string.group_err_no_member));
            return;
        }
        long[] ids = new long[selected.size()];
        int i = 0;
        for (Long id : selected) {
            ids[i++] = id;
        }

        Ui.hideKeyboard(this);
        setBusy(true);
        im.group().createGroup(name, ids, (ok, error, group, members) -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            setBusy(false);
            if (!ok || group == null) {
                // 服务端 message 原样展示（invalid group name / some member user not found 等）
                toast(error == null || error.isEmpty() ? getString(R.string.group_err_op_failed) : error);
                return;
            }
            // 写缓存，返回群聊列表时不用等下一次全量对齐就能看到新群
            im.contacts().putGroup(group);
            im.contacts().setMembers(group.groupId, members);
            toast(getString(R.string.group_toast_created));

            Intent intent = new Intent(this, ChatActivity.class);
            intent.putExtra("peer_id", group.groupId);
            intent.putExtra("is_group", true);
            intent.putExtra("title", group.displayName());
            startActivity(intent);
            finish();
        });
    }

    /** 提交期间禁用按钮并改文案，防止连点建出两个群 */
    private void setBusy(boolean busy) {
        btnCreate.setEnabled(!busy);
        btnCreate.setText(busy
                ? getString(R.string.group_action_creating) : getString(R.string.group_action_create));
        btnCreate.setAlpha(busy ? 0.6f : 1f);
    }

    private void updateCreateEnabled() {
        boolean hasFriends = !friends.isEmpty();
        btnCreate.setEnabled(hasFriends);
        btnCreate.setAlpha(hasFriends ? 1f : 0.5f);
        if (!hasFriends) {
            tvEmpty.setVisibility(View.VISIBLE);
            rvFriends.setVisibility(View.GONE);
            toast(getString(R.string.group_need_friend));
        } else {
            tvEmpty.setVisibility(View.GONE);
            rvFriends.setVisibility(View.VISIBLE);
        }
    }

    private void updateNameCount() {
        tvNameCount.setText(getString(R.string.group_byte_count,
                bytes(etName.getText().toString()), MAX_NAME_BYTES));
    }

    private void refreshHeader() {
        setHeaderAction(getString(R.string.group_header_selected, selected.size()), null);
    }

    private static int bytes(String s) {
        return s == null ? 0 : s.getBytes(StandardCharsets.UTF_8).length;
    }

    // ===== 好友多选列表 =====

    /** 勾选/取消勾选：整行点击与复选框点击都走这里，position 越界时只更新头部计数 */
    private void toggle(long userId, int position) {
        if (selected.contains(userId)) {
            selected.remove(userId);
        } else {
            selected.add(userId);
        }
        if (position >= 0) {
            adapter.notifyItemChanged(position);
        }
        refreshHeader();
    }


    private class FriendAdapter extends RecyclerView.Adapter<FriendVh> {

        @NonNull
        @Override
        public FriendVh onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new FriendVh(LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.group_item_friend_pick, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull FriendVh holder, int position) {
            FriendInfo f = friends.get(position);
            holder.avatar.setPerson(f.userId, f.displayName());
            holder.tvName.setText(f.displayName());
            holder.tvSub.setText(getString(R.string.group_id_inline, f.userId));
            boolean picked = selected.contains(f.userId);
            holder.cb.setChecked(picked);
            holder.itemView.setAlpha(picked ? 1f : 0.92f);
            View.OnClickListener handler = v -> toggle(f.userId, holder.getBindingAdapterPosition());
            holder.itemView.setOnClickListener(handler);
            holder.cb.setOnClickListener(handler);
        }

        @Override
        public int getItemCount() {
            return friends.size();
        }
    }

    /** 文本框只关心 afterTextChanged 时的公共适配器 */
    private abstract static class SimpleWatcher implements TextWatcher {
        @Override
        public void beforeTextChanged(CharSequence s, int start, int count, int after) {
        }

        @Override
        public void onTextChanged(CharSequence s, int start, int before, int count) {
        }
    }

    private static class FriendVh extends RecyclerView.ViewHolder {
        final AvatarView avatar;
        final TextView tvName;
        final TextView tvSub;
        final CheckBox cb;

        FriendVh(@NonNull View itemView) {
            super(itemView);
            avatar = itemView.findViewById(R.id.group_avatar);
            tvName = itemView.findViewById(R.id.group_tv_name);
            tvSub = itemView.findViewById(R.id.group_tv_sub);
            cb = itemView.findViewById(R.id.group_cb_pick);
        }
    }
}
