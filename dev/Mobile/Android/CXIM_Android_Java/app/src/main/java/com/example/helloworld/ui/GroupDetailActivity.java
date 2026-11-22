package com.example.helloworld.ui;

import android.content.Intent;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.core.content.ContextCompat;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.example.helloworld.R;
import com.example.helloworld.im.ChatService;
import com.example.helloworld.im.Json;
import com.example.helloworld.im.model.GroupInfo;
import com.example.helloworld.im.model.GroupMember;
import com.example.helloworld.ui.widget.AvatarView;
import com.google.gson.JsonObject;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * 群聊信息与管理页（协议 §4.14）。所有按钮的可见性/可点性都按 myRole 推导，
 * 权限矩阵见 §4.14：
 * <ul>
 *   <li>群主：改资料 / 邀请 / 移成员 / 任命取消管理员 / 转让 / 解散，退群需先转让</li>
 *   <li>管理员：改资料 / 邀请 / 只移出普通成员 / 退群</li>
 *   <li>普通成员：只能看成员、发私信、退群</li>
 * </ul>
 * 成员与资料变化通过 TCP 140/141/142 事件触发重新拉详情（§5.13）。
 */
public class GroupDetailActivity extends BaseActivity {

    public static final String EXTRA_GROUP_ID = "group_id";

    /** 群名/公告字节上限，与 GroupService 的本地校验保持一致 */
    private static final int MAX_NAME_BYTES = 128;
    private static final int MAX_ANNOUNCE_BYTES = 512;

    private long groupId;
    private GroupInfo info;
    private final List<GroupMember> members = new ArrayList<>();

    private AvatarView avatar;
    private TextView tvGroupName;
    private TextView tvMeta;
    private TextView tvAnnouncement;
    private TextView tvIntro;

    private View cardEdit;
    private EditText etEditName;
    private EditText etEditAnnouncement;
    private Button btnSave;

    private TextView tvMembersTitle;
    private RecyclerView rvMembers;
    private MemberAdapter adapter;

    private Button btnInvite;
    private Button btnQuit;
    private Button btnDismiss;

    /** 编辑区草稿：事件刷新会重新 bind，草稿不能被服务端旧值冲掉 */
    private String draftName = "";
    private String draftAnnouncement = "";

    private ChatService.Listener groupListener;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.group_activity_detail);

        groupId = getIntent().getLongExtra(EXTRA_GROUP_ID, 0L);
        setHeaderTitle(getString(R.string.group_title_detail));

        avatar = findViewById(R.id.group_avatar);
        tvGroupName = findViewById(R.id.group_tv_group_name);
        tvMeta = findViewById(R.id.group_tv_meta);
        tvAnnouncement = findViewById(R.id.group_tv_announcement);
        tvIntro = findViewById(R.id.group_tv_intro);

        cardEdit = findViewById(R.id.group_card_edit);
        etEditName = findViewById(R.id.group_et_edit_name);
        etEditAnnouncement = findViewById(R.id.group_et_edit_announcement);
        btnSave = findViewById(R.id.group_btn_save);

        tvMembersTitle = findViewById(R.id.group_tv_members_title);
        rvMembers = findViewById(R.id.group_rv_members);
        adapter = new MemberAdapter();
        rvMembers.setLayoutManager(new LinearLayoutManager(this));
        rvMembers.setAdapter(adapter);

        btnInvite = findViewById(R.id.group_btn_invite);
        btnQuit = findViewById(R.id.group_btn_quit);
        btnDismiss = findViewById(R.id.group_btn_dismiss);

        etEditName.addTextChangedListener(new SimpleWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                draftName = s.toString();
            }
        });
        etEditAnnouncement.addTextChangedListener(new SimpleWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                draftAnnouncement = s.toString();
            }
        });

        btnSave.setOnClickListener(v -> saveInfo());
        btnInvite.setOnClickListener(v -> startActivity(new Intent(this, InviteMembersActivity.class)
                .putExtra(EXTRA_GROUP_ID, groupId)));
        btnQuit.setOnClickListener(v -> quitGroup());
        btnDismiss.setOnClickListener(v -> dismissGroup());

        groupListener = new ChatService.SimpleListener() {
            @Override
            public void onGroupMemberChangedEvent(JsonObject payload) {
                onGroupEvent(payload, false);
            }

            @Override
            public void onGroupInfoChangedEvent(JsonObject payload) {
                onGroupEvent(payload, false);
            }

            @Override
            public void onGroupDismissedEvent(JsonObject payload) {
                onGroupEvent(payload, true);
            }
        };
        im.chat().addListener(groupListener);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // 首次进入与从邀请页返回都重新拉一次详情（groupDetail 会顺带刷新缓存）
        loadDetail();
    }

    @Override
    protected void onDestroy() {
        if (groupListener != null) {
            im.chat().removeListener(groupListener);
        }
        super.onDestroy();
    }

    // ===== 数据 =====

    private void loadDetail() {
        if (groupId <= 0) {
            toast(getString(R.string.group_group_not_found));
            return;
        }
        im.groupDetail(groupId, (ok, error, group, ms) -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            if (!ok || group == null) {
                toast(error == null || error.isEmpty()
                        ? getString(R.string.group_group_not_found) : error);
                return;
            }
            info = group;
            members.clear();
            members.addAll(ms);
            bind();
        });
    }

    private void bind() {
        if (info == null) {
            return;
        }
        setHeaderTitle(info.displayName());
        avatar.setGroup(info.groupId, info.displayName());
        tvGroupName.setText(info.displayName());

        int count = info.memberCount > 0 ? info.memberCount : members.size();
        tvMeta.setText(getString(R.string.group_meta_line, info.groupId, count,
                GroupInfo.roleText(info.myRole)));

        if (info.announcement.trim().isEmpty()) {
            tvAnnouncement.setText(R.string.group_announce_none);
        } else {
            tvAnnouncement.setText(getString(R.string.group_announce_value, info.announcement.trim()));
        }
        if (info.intro.trim().isEmpty()) {
            tvIntro.setVisibility(View.GONE);
        } else {
            tvIntro.setVisibility(View.VISIBLE);
            tvIntro.setText(getString(R.string.group_intro_value, info.intro.trim()));
        }

        boolean canManage = info.canManage();
        cardEdit.setVisibility(canManage ? View.VISIBLE : View.GONE);
        if (canManage) {
            // 用户正在输入时不要打断光标
            if (!etEditName.hasFocus() && !etEditName.getText().toString().equals(draftName)) {
                etEditName.setText(draftName);
            }
            if (!etEditAnnouncement.hasFocus()
                    && !etEditAnnouncement.getText().toString().equals(draftAnnouncement)) {
                etEditAnnouncement.setText(draftAnnouncement);
            }
        }

        tvMembersTitle.setText(getString(R.string.group_section_members, members.size()));
        adapter.notifyDataSetChanged();

        btnInvite.setVisibility(canManage ? View.VISIBLE : View.GONE);
        btnDismiss.setVisibility(info.isOwner() ? View.VISIBLE : View.GONE);
        // 群主退群必须先在成员菜单里转让，这里保持可点但只提示，避免按钮「点了没反应」
        btnQuit.setAlpha(info.isOwner() ? 0.5f : 1f);
        btnQuit.setTextColor(ContextCompat.getColor(this,
                info.isOwner() ? R.color.text_disabled : R.color.text_primary));
    }

    // ===== 事件刷新（TCP 线程回调，统一切回主线程） =====

    private void onGroupEvent(JsonObject payload, boolean dismissed) {
        long gid = Json.num(payload, "group_id");
        if (gid != groupId) {
            return;
        }
        runOnUiThread(() -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            if (dismissed) {
                im.contacts().removeGroup(groupId);
                toast(getString(R.string.group_event_group_dismissed));
                finish();
                return;
            }
            loadDetail();
        });
    }

    // ===== 群资料 =====

    private void saveInfo() {
        if (info == null) {
            return;
        }
        String name = draftName.trim();
        String announcement = draftAnnouncement.trim();
        if (name.isEmpty() && announcement.isEmpty()) {
            toast(getString(R.string.group_err_nothing_to_update));
            return;
        }
        if (bytes(name) > MAX_NAME_BYTES) {
            toast(getString(R.string.group_err_name_too_long));
            return;
        }
        if (bytes(announcement) > MAX_ANNOUNCE_BYTES) {
            toast(getString(R.string.group_err_announcement_too_long));
            return;
        }
        btnSave.setEnabled(false);
        btnSave.setText(R.string.group_action_saving);
        // 简介本版本不提供编辑入口，传空串=不修改（服务端语义见 §4.14.3）
        im.group().updateGroup(groupId, name, announcement, "", (ok, error, data) -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            btnSave.setEnabled(true);
            btnSave.setText(R.string.group_action_save);
            if (!ok) {
                toast(error == null || error.isEmpty()
                        ? getString(R.string.group_err_op_failed) : error);
                return;
            }
            draftName = "";
            draftAnnouncement = "";
            etEditName.setText("");
            etEditAnnouncement.setText("");
            toast(getString(R.string.group_toast_saved));
            loadDetail();
        });
    }

    // ===== 底部操作 =====

    private void quitGroup() {
        if (info != null && info.isOwner()) {
            toast(getString(R.string.group_msg_owner_must_transfer));
            return;
        }
        String name = info == null ? String.valueOf(groupId) : info.displayName();
        Ui.confirm(this, getString(R.string.group_action_quit),
                getString(R.string.group_confirm_quit, name), () -> {
                    btnQuit.setEnabled(false);
                    im.group().quitGroup(groupId, (ok, error, data) -> {
                        if (isFinishing() || isDestroyed()) {
                            return;
                        }
                        btnQuit.setEnabled(true);
                        if (!ok) {
                            toast(error == null || error.isEmpty()
                                    ? getString(R.string.group_err_op_failed) : error);
                            return;
                        }
                        im.contacts().removeGroup(groupId);
                        toast(getString(R.string.group_toast_quit));
                        finish();
                    });
                });
    }

    private void dismissGroup() {
        String name = info == null ? String.valueOf(groupId) : info.displayName();
        Ui.confirm(this, getString(R.string.group_action_dismiss),
                getString(R.string.group_confirm_dismiss, name), () -> {
                    btnDismiss.setEnabled(false);
                    im.group().dismissGroup(groupId, (ok, error, data) -> {
                        if (isFinishing() || isDestroyed()) {
                            return;
                        }
                        btnDismiss.setEnabled(true);
                        if (!ok) {
                            toast(error == null || error.isEmpty()
                                    ? getString(R.string.group_err_op_failed) : error);
                            return;
                        }
                        im.contacts().removeGroup(groupId);
                        toast(getString(R.string.group_toast_dismissed));
                        finish();
                    });
                });
    }

    // ===== 成员操作 =====

    /** 按「我的角色 + 目标角色 + 是否自己」拼菜单，顺序：发私信 / 任命取消 / 转让 / 移出 */
    private void showMemberMenu(GroupMember m) {
        if (info == null) {
            return;
        }
        boolean self = m.userId == im.chat().selfId();
        final List<String> labels = new ArrayList<>();
        final List<Runnable> actions = new ArrayList<>();

        if (!self) {
            labels.add(getString(R.string.group_act_send_private_msg));
            actions.add(() -> openPrivateChat(m));
        }
        if (info.isOwner() && !self) {
            if (m.role == GroupInfo.ROLE_MEMBER) {
                labels.add(getString(R.string.group_act_set_admin));
                actions.add(() -> setAdmin(m, true));
            } else if (m.role == GroupInfo.ROLE_ADMIN) {
                labels.add(getString(R.string.group_act_unset_admin));
                actions.add(() -> setAdmin(m, false));
            }
            if (m.role != GroupInfo.ROLE_OWNER) {
                labels.add(getString(R.string.group_act_transfer_owner));
                actions.add(() -> transferOwner(m));
            }
        }
        // 移出：群主可移非群主；管理员只能移普通成员
        boolean canRemove = !self && (info.isOwner()
                ? m.role != GroupInfo.ROLE_OWNER
                : info.myRole == GroupInfo.ROLE_ADMIN && m.role == GroupInfo.ROLE_MEMBER);
        if (canRemove) {
            labels.add(getString(R.string.group_act_remove));
            actions.add(() -> removeMember(m));
        }
        if (labels.isEmpty()) {
            return;
        }
        Ui.choices(this, m.displayName(), labels.toArray(new String[0]),
                (index, text) -> actions.get(index).run());
    }

    private void openPrivateChat(GroupMember m) {
        Intent intent = new Intent(this, ChatActivity.class);
        intent.putExtra("peer_id", m.userId);
        intent.putExtra("is_group", false);
        intent.putExtra("title", m.displayName());
        startActivity(intent);
        finish();
    }

    private void setAdmin(GroupMember m, boolean admin) {
        im.group().setAdmin(groupId, m.userId, admin, (ok, error, data) -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            if (!ok) {
                toast(error == null || error.isEmpty()
                        ? getString(R.string.group_err_op_failed) : error);
                return;
            }
            toast(getString(admin ? R.string.group_toast_admin_set : R.string.group_toast_admin_unset,
                    m.displayName()));
            loadDetail();
        });
    }

    private void transferOwner(GroupMember m) {
        Ui.confirm(this, getString(R.string.group_act_transfer_owner),
                getString(R.string.group_confirm_transfer, m.displayName()), () ->
                        im.group().transferOwner(groupId, m.userId, (ok, error, data) -> {
                            if (isFinishing() || isDestroyed()) {
                                return;
                            }
                            if (!ok) {
                                toast(error == null || error.isEmpty()
                                        ? getString(R.string.group_err_op_failed) : error);
                                return;
                            }
                            toast(getString(R.string.group_toast_transferred, m.displayName()));
                            loadDetail();
                        }));
    }

    private void removeMember(GroupMember m) {
        Ui.confirm(this, getString(R.string.group_act_remove),
                getString(R.string.group_confirm_remove, m.displayName()), () ->
                        im.group().removeMember(groupId, m.userId, (ok, error, data) -> {
                            if (isFinishing() || isDestroyed()) {
                                return;
                            }
                            if (!ok) {
                                toast(error == null || error.isEmpty()
                                        ? getString(R.string.group_err_op_failed) : error);
                                return;
                            }
                            toast(getString(R.string.group_toast_removed, m.displayName()));
                            loadDetail();
                        }));
    }

    private static int bytes(String s) {
        return s == null ? 0 : s.getBytes(StandardCharsets.UTF_8).length;
    }

    // ===== 成员列表 =====

    private class MemberAdapter extends RecyclerView.Adapter<MemberVh> {

        @NonNull
        @Override
        public MemberVh onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new MemberVh(LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.group_item_member, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull MemberVh holder, int position) {
            GroupMember m = members.get(position);
            holder.avatar.setPerson(m.userId, m.displayName());
            holder.tvName.setText(m.displayName());
            holder.tvSub.setText(getString(R.string.group_id_inline, m.userId));
            if (m.role == GroupInfo.ROLE_OWNER) {
                holder.tvRole.setVisibility(View.VISIBLE);
                holder.tvRole.setBackgroundResource(R.drawable.bg_role_owner);
                holder.tvRole.setTextColor(ContextCompat.getColor(GroupDetailActivity.this,
                        R.color.owner_gold));
                holder.tvRole.setText(R.string.group_role_owner_badge);
            } else if (m.role == GroupInfo.ROLE_ADMIN) {
                holder.tvRole.setVisibility(View.VISIBLE);
                holder.tvRole.setBackgroundResource(R.drawable.bg_role_admin);
                holder.tvRole.setTextColor(ContextCompat.getColor(GroupDetailActivity.this,
                        R.color.brand_accent));
                holder.tvRole.setText(R.string.group_role_admin_badge);
            } else {
                holder.tvRole.setVisibility(View.GONE);
            }
            holder.itemView.setAlpha(m.userId == im.chat().selfId() ? 0.9f : 1f);
            holder.itemView.setOnClickListener(v -> showMemberMenu(m));
        }

        @Override
        public int getItemCount() {
            return members.size();
        }
    }

    private static class MemberVh extends RecyclerView.ViewHolder {
        final AvatarView avatar;
        final TextView tvName;
        final TextView tvSub;
        final TextView tvRole;

        MemberVh(@NonNull View itemView) {
            super(itemView);
            avatar = itemView.findViewById(R.id.group_avatar);
            tvName = itemView.findViewById(R.id.group_tv_name);
            tvSub = itemView.findViewById(R.id.group_tv_sub);
            tvRole = itemView.findViewById(R.id.group_tv_role);
        }
    }

    /** 只关心 afterTextChanged */
    private abstract static class SimpleWatcher implements TextWatcher {
        @Override
        public void beforeTextChanged(CharSequence s, int start, int count, int after) {
        }

        @Override
        public void onTextChanged(CharSequence s, int start, int before, int count) {
        }
    }
}
