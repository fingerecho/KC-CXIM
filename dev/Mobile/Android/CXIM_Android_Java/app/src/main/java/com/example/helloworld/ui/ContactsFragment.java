package com.example.helloworld.ui;

import android.content.Intent;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import com.example.helloworld.CXIMApp;
import com.example.helloworld.R;
import com.example.helloworld.im.ChatService;
import com.example.helloworld.im.Contacts;
import com.example.helloworld.im.ImService;
import com.example.helloworld.im.model.FriendInfo;
import com.example.helloworld.im.model.GroupInfo;
import com.example.helloworld.ui.widget.AvatarView;
import com.google.android.material.button.MaterialButton;
import com.google.gson.JsonObject;

import java.util.ArrayList;
import java.util.List;

/**
 * 通讯录页（MainActivity 底部导航第二个 tab）。
 *
 * <p>结构：搜索框 + ＋ 菜单 / 好友·群聊胶囊 / 列表（首行是「新朋友」「入群申请」banner）/ 新建群聊。
 * 数据只从 {@link Contacts} 缓存读，缓存由 {@link ImService} 的 refresh* 与 TCP 事件驱动，
 * 本页面只负责「事件到了重拉 + 回调里刷新列表」。
 *
 * <p>监听器在 onResume 注册、onPause 注销，避免页面退到后台时被后台回调打进来。
 */
public class ContactsFragment extends Fragment {

    private static final int TAB_FRIENDS = 0;
    private static final int TAB_GROUPS = 1;

    private static final int TYPE_BANNER = 0;
    private static final int TYPE_FRIEND = 1;
    private static final int TYPE_GROUP = 2;

    /** 入群申请 banner 点击时传给 GroupRequestsActivity 的方向 */
    private static final String DIRECTION_INCOMING = "incoming";

    private ImService im;
    private int tab = TAB_FRIENDS;

    private View root;
    private EditText etSearch;
    private SwipeRefreshLayout swipe;
    private RecyclerView rv;
    private View emptyFriends;
    private View emptyGroups;
    private MaterialButton btnTabFriends;
    private MaterialButton btnTabGroups;
    private MaterialButton btnCreateGroup;

    private RowAdapter adapter;
    private final List<FriendInfo> friends = new ArrayList<>();
    private final List<GroupInfo> groups = new ArrayList<>();
    private boolean bannerVisible;

    /** 下拉刷新：等对应的那一类数据回调回来再收起菊花 */
    private int pullWaitingTab = TAB_FRIENDS;
    private final Runnable pullTimeout = () -> stopPull();

    // ===================================================================
    //  监听器
    // ===================================================================

    private final Contacts.SimpleListener contactsListener = new Contacts.SimpleListener() {
        @Override
        public void onFriendsChanged() {
            reload();
            if (pullWaitingTab == TAB_FRIENDS) {
                stopPull();
            }
        }

        @Override
        public void onGroupsChanged() {
            reload();
            if (pullWaitingTab == TAB_GROUPS) {
                stopPull();
            }
        }

        @Override
        public void onPendingChanged(int friendPending, int groupPending) {
            // 角标只影响 banner，不打断列表的刷新等待
            reload();
        }
    };

    /** 协议 §5.12 的好友事件 + 群事件：都不带事实，收到就重拉全量对齐 */
    private final ChatService.SimpleListener chatListener = new ChatService.SimpleListener() {
        @Override
        public void onFriendRequestEvent(JsonObject payload) {
            if (im != null) {
                im.refreshFriends();
                im.refreshPendingCounts();
            }
        }

        @Override
        public void onFriendRequestResultEvent(JsonObject payload) {
            if (im != null) {
                im.refreshFriends();
                im.refreshPendingCounts();
            }
        }

        @Override
        public void onFriendListChangedEvent(JsonObject payload) {
            if (im != null) {
                im.refreshFriends();
                im.refreshPendingCounts();
            }
        }

        @Override
        public void onGroupMemberChangedEvent(JsonObject payload) {
            refreshGroupData();
        }

        @Override
        public void onGroupDismissedEvent(JsonObject payload) {
            refreshGroupData();
        }

        @Override
        public void onGroupInfoChangedEvent(JsonObject payload) {
            refreshGroupData();
        }

        @Override
        public void onGroupRequestEvent(JsonObject payload) {
            refreshGroupData();
        }

        @Override
        public void onGroupRequestResultEvent(JsonObject payload) {
            refreshGroupData();
        }
    };

    // ===================================================================
    //  生命周期
    // ===================================================================

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        root = inflater.inflate(R.layout.contact_fragment_contacts, container, false);
        im = CXIMApp.service();

        etSearch = root.findViewById(R.id.et_search);
        ImageButton btnMore = root.findViewById(R.id.btn_more);
        swipe = root.findViewById(R.id.swipe_refresh);
        rv = root.findViewById(R.id.rv_contacts);
        emptyFriends = root.findViewById(R.id.empty_friends);
        emptyGroups = root.findViewById(R.id.empty_groups);
        btnTabFriends = root.findViewById(R.id.btn_tab_friends);
        btnTabGroups = root.findViewById(R.id.btn_tab_groups);
        btnCreateGroup = root.findViewById(R.id.btn_create_group);

        adapter = new RowAdapter();
        rv.setLayoutManager(new LinearLayoutManager(requireContext()));
        rv.setAdapter(adapter);

        swipe.setColorSchemeResources(R.color.brand_bottom, R.color.brand_top, R.color.brand_accent);
        swipe.setOnRefreshListener(this::startPull);

        // 回车（IME_ACTION_SEARCH）直接带关键词跳添加好友页
        etSearch.setOnEditorActionListener((v, actionId, event) -> {
            boolean enter = event != null
                    && event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                    && event.getAction() == KeyEvent.ACTION_DOWN;
            if (actionId != EditorInfo.IME_ACTION_SEARCH && !enter) {
                return false;
            }
            openAddFriend();
            return true;
        });

        btnMore.setOnClickListener(v -> showMoreMenu());
        btnTabFriends.setOnClickListener(v -> selectTab(TAB_FRIENDS));
        btnTabGroups.setOnClickListener(v -> selectTab(TAB_GROUPS));
        btnCreateGroup.setOnClickListener(v ->
                startActivity(new Intent(requireContext(), CreateGroupActivity.class)));

        selectTab(tab);
        return root;
    }

    @Override
    public void onResume() {
        super.onResume();
        if (im == null) {
            return;
        }
        im.contacts().addListener(contactsListener);
        im.chat().addListener(chatListener);
        // 缓存为空时（首次进入 / 进程被杀后重建）补拉一次
        if (im.contacts().friends().isEmpty()) {
            im.refreshFriends();
        }
        if (im.contacts().groups().isEmpty()) {
            im.refreshGroups();
        }
        im.refreshPendingCounts();
        reload();
    }

    @Override
    public void onPause() {
        super.onPause();
        if (im != null) {
            im.contacts().removeListener(contactsListener);
            im.chat().removeListener(chatListener);
        }
        stopPull();
    }

    @Override
    public void onDestroyView() {
        super.onDestroyView();
        if (swipe != null) {
            swipe.removeCallbacks(pullTimeout);
            swipe.setOnRefreshListener(null);
        }
        if (rv != null) {
            rv.setAdapter(null);
        }
        adapter = null;
        root = null;
        swipe = null;
        rv = null;
    }

    // ===================================================================
    //  数据装载
    // ===================================================================

    /** 把缓存里的通讯录搬到页面内的列表上 */
    private void reload() {
        if (adapter == null || im == null) {
            return;
        }
        friends.clear();
        groups.clear();
        if (tab == TAB_FRIENDS) {
            friends.addAll(im.contacts().sortedFriends());
            bannerVisible = true;
        } else {
            groups.addAll(im.contacts().sortedGroups());
            bannerVisible = im.contacts().groupPending() > 0;
        }
        adapter.notifyDataSetChanged();

        boolean noFriendRow = tab == TAB_FRIENDS && friends.isEmpty();
        boolean noGroupRow = tab == TAB_GROUPS && groups.isEmpty();
        emptyFriends.setVisibility(noFriendRow ? View.VISIBLE : View.GONE);
        // 群聊页只剩一个「入群申请」banner 时也算空态，提示语更完整
        emptyGroups.setVisibility(noGroupRow && !bannerVisible ? View.VISIBLE : View.GONE);
    }

    private void selectTab(int which) {
        tab = which;
        if (btnTabFriends != null) {
            btnTabFriends.setChecked(which == TAB_FRIENDS);
        }
        if (btnTabGroups != null) {
            btnTabGroups.setChecked(which == TAB_GROUPS);
        }
        if (btnCreateGroup != null) {
            btnCreateGroup.setVisibility(which == TAB_GROUPS ? View.VISIBLE : View.GONE);
        }
        if (rv != null) {
            rv.scrollToPosition(0);
        }
        reload();
    }

    private void startPull() {
        if (im == null || root == null) {
            stopPull();
            return;
        }
        pullWaitingTab = tab;
        if (tab == TAB_FRIENDS) {
            im.refreshFriends();
        } else {
            im.refreshGroups();
        }
        im.refreshPendingCounts();
        // 兜底：请求失败时不会有任何缓存回调，5 秒后强制收起
        swipe.postDelayed(pullTimeout, 5000L);
    }

    private void stopPull() {
        if (swipe != null) {
            swipe.removeCallbacks(pullTimeout);
            swipe.setRefreshing(false);
        }
    }

    private void refreshFriendData() {
        if (im != null) {
            im.refreshFriends();
            im.refreshPendingCounts();
        }
    }

    private void refreshGroupData() {
        if (im != null) {
            im.refreshGroups();
            im.refreshPendingCounts();
        }
    }

    // ===================================================================
    //  交互
    // ===================================================================

    /** ＋ 按钮的快捷菜单 */
    private void showMoreMenu() {
        String[] items = new String[]{
                getString(R.string.friend_menu_add_friend),
                getString(R.string.friend_menu_create_group),
                getString(R.string.friend_menu_join_group),
                getString(R.string.friend_menu_multi_send)
        };
        Ui.choices(requireContext(), getString(R.string.friend_menu_title), items, (index, text) -> {
            switch (index) {
                case 0:
                    openAddFriend();
                    break;
                case 1:
                    startActivity(new Intent(requireContext(), CreateGroupActivity.class));
                    break;
                case 2:
                    startActivity(new Intent(requireContext(), GroupJoinActivity.class));
                    break;
                default:
                    startActivity(new Intent(requireContext(), MultiSendActivity.class));
                    break;
            }
        });
    }

    /** 搜索框回车：带关键词跳添加好友页 */
    private void openAddFriend() {
        String keyword = etSearch == null ? "" : etSearch.getText().toString().trim();
        Ui.hideKeyboard(requireActivity());
        Intent i = new Intent(requireContext(), AddFriendActivity.class);
        i.putExtra("keyword", keyword);
        startActivity(i);
    }

    private void openChat(long peerId, boolean isGroup, String title) {
        Intent i = new Intent(requireContext(), ChatActivity.class);
        i.putExtra("peer_id", peerId);
        i.putExtra("is_group", isGroup);
        i.putExtra("title", title);
        startActivity(i);
    }

    private void openFriendRequests() {
        startActivity(new Intent(requireContext(), FriendRequestsActivity.class));
    }

    private void openGroupRequests() {
        Intent i = new Intent(requireContext(), GroupRequestsActivity.class);
        i.putExtra("direction", DIRECTION_INCOMING);
        startActivity(i);
    }

    /** 好友长按菜单 */
    private void showFriendMenu(FriendInfo f) {
        String[] items = new String[]{
                getString(R.string.friend_action_send_msg),
                getString(R.string.friend_action_remark),
                getString(R.string.friend_action_remove),
                getString(R.string.friend_action_block)
        };
        Ui.choices(requireContext(), getString(R.string.friend_action_title), items, (index, text) -> {
            switch (index) {
                case 0:
                    openChat(f.userId, false, f.displayName());
                    break;
                case 1:
                    showRemarkDialog(f);
                    break;
                case 2:
                    confirmRemoveFriend(f);
                    break;
                default:
                    confirmBlockFriend(f);
                    break;
            }
        });
    }

    /** 群长按菜单 */
    private void showGroupMenu(GroupInfo g) {
        String[] items = new String[]{getString(R.string.friend_group_manage)};
        Ui.choices(requireContext(), g.displayName(), items, (index, text) -> {
            Intent i = new Intent(requireContext(), GroupDetailActivity.class);
            i.putExtra("group_id", g.groupId);
            startActivity(i);
        });
    }

    /** 设置备注：§4.10，成功后重拉好友列表让新备注落到缓存里 */
    private void showRemarkDialog(FriendInfo f) {
        EditText input = new EditText(requireContext());
        input.setHint(R.string.friend_remark_hint);
        input.setSingleLine(true);
        input.setText(f.remark == null ? "" : f.remark);
        input.setSelection(input.getText().length());
        int pad = Ui.dp(requireContext(), 20);
        FrameLayout wrap = new FrameLayout(requireContext());
        wrap.setPadding(pad, pad / 2, pad, 0);
        wrap.addView(input);

        new AlertDialog.Builder(requireContext())
                .setTitle(R.string.friend_remark_title)
                .setView(wrap)
                .setNegativeButton(R.string.cancel, null)
                .setPositiveButton(R.string.ok, (d, w) -> {
                    String remark = input.getText().toString().trim();
                    im.friend().setRemark(f.userId, remark, r -> {
                        if (r.ok) {
                            im.refreshFriends();
                            toast(getString(R.string.friend_remark_saved));
                        } else {
                            toast(r.errorText(getString(R.string.friend_remark_fail)));
                        }
                    });
                })
                .show();
    }

    private void confirmRemoveFriend(FriendInfo f) {
        Ui.confirm(requireContext(), getString(R.string.friend_remove_title),
                getString(R.string.friend_remove_confirm_fmt, f.displayName()),
                () -> im.friend().removeFriend(f.userId, r -> {
                    if (r.ok) {
                        im.refreshFriends();
                        toast(getString(R.string.friend_remove_done));
                    } else {
                        toast(r.errorText(getString(R.string.friend_remove_fail)));
                    }
                }));
    }

    private void confirmBlockFriend(FriendInfo f) {
        Ui.confirm(requireContext(), getString(R.string.friend_block_title),
                getString(R.string.friend_block_confirm),
                () -> im.friend().blockFriend(f.userId, r -> {
                    if (r.ok) {
                        im.refreshFriends();
                        im.refreshPendingCounts();
                        toast(getString(R.string.friend_block_done));
                    } else {
                        toast(r.errorText(getString(R.string.friend_block_fail)));
                    }
                }));
    }

    private void toast(String text) {
        if (isAdded()) {
            Ui.toast(requireContext(), text);
        }
    }

    // ===================================================================
    //  Adapter
    // ===================================================================

    private class RowAdapter extends RecyclerView.Adapter<RecyclerView.ViewHolder> {

        @NonNull
        @Override
        public RecyclerView.ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            LayoutInflater lf = LayoutInflater.from(parent.getContext());
            if (viewType == TYPE_BANNER) {
                return new BannerHolder(lf.inflate(R.layout.contact_item_banner, parent, false));
            }
            if (viewType == TYPE_GROUP) {
                return new GroupHolder(lf.inflate(R.layout.contact_item_group, parent, false));
            }
            return new FriendHolder(lf.inflate(R.layout.contact_item_friend, parent, false));
        }

        @Override
        public int getItemViewType(int position) {
            if (bannerVisible && position == 0) {
                return TYPE_BANNER;
            }
            return tab == TAB_FRIENDS ? TYPE_FRIEND : TYPE_GROUP;
        }

        @Override
        public int getItemCount() {
            int n = tab == TAB_FRIENDS ? friends.size() : groups.size();
            return n + (bannerVisible ? 1 : 0);
        }

        @Override
        public void onBindViewHolder(@NonNull RecyclerView.ViewHolder holder, int position) {
            int idx = bannerVisible ? position - 1 : position;
            if (holder instanceof BannerHolder) {
                ((BannerHolder) holder).bind();
            } else if (holder instanceof GroupHolder) {
                ((GroupHolder) holder).bind(groups.get(Math.max(idx, 0)));
            } else if (holder instanceof FriendHolder) {
                ((FriendHolder) holder).bind(friends.get(Math.max(idx, 0)));
            }
        }
    }

    /** 「新朋友」/「入群申请」两用的 banner 行 */
    private class BannerHolder extends RecyclerView.ViewHolder {
        final ImageView icon;
        final TextView title;
        final TextView sub;
        final TextView badge;

        BannerHolder(@NonNull View itemView) {
            super(itemView);
            icon = itemView.findViewById(R.id.iv_banner_icon);
            title = itemView.findViewById(R.id.tv_banner_title);
            sub = itemView.findViewById(R.id.tv_banner_sub);
            badge = itemView.findViewById(R.id.tv_badge);
        }

        void bind() {
            if (tab == TAB_FRIENDS) {
                int pending = im == null ? 0 : im.contacts().friendPending();
                icon.setImageResource(R.drawable.ic_person_add);
                title.setText(R.string.new_friends);
                sub.setText(pending > 0
                        ? getString(R.string.friend_pending_fmt, pending)
                        : getString(R.string.friend_no_pending));
                setBadge(pending);
                itemView.setOnClickListener(v -> openFriendRequests());
            } else {
                int pending = im == null ? 0 : im.contacts().groupPending();
                icon.setImageResource(R.drawable.ic_mail_outline);
                title.setText(R.string.group_requests_title);
                sub.setText(pending > 0
                        ? getString(R.string.friend_group_pending_fmt, pending)
                        : getString(R.string.friend_group_no_pending));
                setBadge(pending);
                itemView.setOnClickListener(v -> openGroupRequests());
            }
        }

        private void setBadge(int n) {
            if (n > 0) {
                badge.setVisibility(View.VISIBLE);
                badge.setText(String.valueOf(n));
            } else {
                badge.setVisibility(View.GONE);
            }
        }
    }

    private class FriendHolder extends RecyclerView.ViewHolder {
        final AvatarView avatar;
        final TextView name;
        final TextView sub;

        FriendHolder(@NonNull View itemView) {
            super(itemView);
            avatar = itemView.findViewById(R.id.avatar);
            name = itemView.findViewById(R.id.tv_name);
            sub = itemView.findViewById(R.id.tv_sub);
        }

        void bind(FriendInfo f) {
            String shown = f.displayName();
            avatar.setPerson(f.userId, shown);
            name.setText(shown);
            sub.setText(getString(R.string.friend_id_fmt, f.userId));
            itemView.setOnClickListener(v -> openChat(f.userId, false, shown));
            itemView.setOnLongClickListener(v -> {
                showFriendMenu(f);
                return true;
            });
        }
    }

    private class GroupHolder extends RecyclerView.ViewHolder {
        final TextView name;
        final TextView sub;

        GroupHolder(@NonNull View itemView) {
            super(itemView);
            name = itemView.findViewById(R.id.tv_name);
            sub = itemView.findViewById(R.id.tv_sub);
        }

        void bind(GroupInfo g) {
            String shown = g.displayName();
            name.setText(shown);
            sub.setText(getString(R.string.friend_member_count_fmt, g.memberCount));
            itemView.setOnClickListener(v -> openChat(g.groupId, true, shown));
            itemView.setOnLongClickListener(v -> {
                showGroupMenu(g);
                return true;
            });
        }
    }
}
