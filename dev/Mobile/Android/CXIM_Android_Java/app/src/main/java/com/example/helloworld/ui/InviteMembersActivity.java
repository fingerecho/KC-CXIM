package com.example.helloworld.ui;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.example.helloworld.R;
import com.example.helloworld.im.model.FriendInfo;
import com.example.helloworld.im.model.GroupMember;
import com.example.helloworld.ui.widget.AvatarView;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

/**
 * 邀请好友入群（协议 §4.14.4）。已在群内的成员整行置灰不可选；
 * 服务端对重复邀请是静默跳过，所以这里拦一层只是让界面更好懂。
 */
public class InviteMembersActivity extends BaseActivity {

    private static final String EXTRA_GROUP_ID = "group_id";

    private long groupId;
    private final List<FriendInfo> friends = new ArrayList<>();
    private final Set<Long> inGroup = new HashSet<>();
    private final Set<Long> selected = new LinkedHashSet<>();

    private TextView tvEmpty;
    private PickAdapter adapter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.group_activity_invite);
        setHeaderTitle(getString(R.string.group_title_invite));

        groupId = getIntent().getLongExtra(EXTRA_GROUP_ID, 0L);
        tvEmpty = findViewById(R.id.group_tv_empty);
        RecyclerView rv = findViewById(R.id.group_rv_friends);
        adapter = new PickAdapter();
        rv.setLayoutManager(new LinearLayoutManager(this));
        rv.setAdapter(adapter);

        friends.addAll(im.contacts().sortedFriends());
        for (GroupMember m : im.contacts().members(groupId)) {
            inGroup.add(m.userId);
        }
        // 自己肯定在群里，省得出现在可选列表里
        inGroup.add(im.chat().selfId());

        tvEmpty.setVisibility(friends.isEmpty() ? View.VISIBLE : View.GONE);
        rv.setVisibility(friends.isEmpty() ? View.GONE : View.VISIBLE);
        refreshHeader();
    }

    /** 右上角实时显示已选人数，同时作为提交入口 */
    private void refreshHeader() {
        setHeaderAction(getString(R.string.group_header_confirm_count, selected.size()),
                v -> submit());
    }

    private void submit() {
        if (selected.isEmpty()) {
            toast(getString(R.string.group_err_no_member));
            return;
        }
        long[] ids = new long[selected.size()];
        int i = 0;
        for (Long id : selected) {
            ids[i++] = id;
        }
        setHeaderEnabled(false);
        im.group().inviteMembers(groupId, ids, (ok, error, group, members) -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            refreshHeader();
            if (!ok) {
                // all invitees already in group / member limit exceeded / not a group owner-admin …
                toast(error == null || error.isEmpty()
                        ? getString(R.string.group_err_op_failed) : error);
                return;
            }
            if (group != null) {
                im.contacts().putGroup(group);
            }
            im.contacts().setMembers(groupId, members);
            toast(getString(R.string.group_toast_invited));
            setResult(RESULT_OK);
            finish();
        });
    }

    /** 请求进行中让右上角文字失去点击与色彩，避免连点重复邀请 */
    private void setHeaderEnabled(boolean enabled) {
        setHeaderAction(getString(R.string.group_header_confirm_count, selected.size()),
                enabled ? v -> submit() : null);
        if (tvAction != null) {
            tvAction.setAlpha(enabled ? 1f : 0.5f);
        }
    }

    /** 勾选/取消勾选：整行与复选框共用 */
    private void togglePick(long userId, int position) {
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

    private class PickAdapter extends RecyclerView.Adapter<PickVh> {

        @NonNull
        @Override
        public PickVh onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new PickVh(LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.group_item_friend_pick, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull PickVh holder, int position) {
            FriendInfo f = friends.get(position);
            boolean disabled = inGroup.contains(f.userId);
            boolean picked = !disabled && selected.contains(f.userId);

            holder.avatar.setPerson(f.userId, f.displayName());
            holder.tvName.setText(f.displayName());
            String sub = getString(R.string.group_id_inline, f.userId);
            if (disabled) {
                sub += getString(R.string.group_suffix_in_group);
            }
            holder.tvSub.setText(sub);
            holder.cb.setChecked(picked);
            holder.cb.setEnabled(!disabled);
            holder.itemView.setAlpha(disabled ? 0.45f : 1f);
            holder.itemView.setClickable(!disabled);
            holder.cb.setClickable(!disabled);
            View.OnClickListener toggle = v -> togglePick(f.userId, holder.getBindingAdapterPosition());
            holder.itemView.setOnClickListener(toggle);
            holder.cb.setOnClickListener(toggle);
            if (disabled) {
                // 已在群内：清掉可能残留的勾选，避免「看不见但被邀请」
                selected.remove(f.userId);
            }
        }

        @Override
        public int getItemCount() {
            return friends.size();
        }
    }

    private static class PickVh extends RecyclerView.ViewHolder {
        final AvatarView avatar;
        final TextView tvName;
        final TextView tvSub;
        final CheckBox cb;

        PickVh(@NonNull View itemView) {
            super(itemView);
            avatar = itemView.findViewById(R.id.group_avatar);
            tvName = itemView.findViewById(R.id.group_tv_name);
            tvSub = itemView.findViewById(R.id.group_tv_sub);
            cb = itemView.findViewById(R.id.group_cb_pick);
        }
    }
}
