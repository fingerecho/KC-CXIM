package com.example.helloworld.ui;

import android.content.Intent;
import android.os.Bundle;
import android.view.View;
import android.widget.TextView;

import androidx.annotation.Nullable;
import androidx.appcompat.widget.PopupMenu;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentTransaction;

import com.example.helloworld.R;
import com.example.helloworld.im.ChatService;
import com.example.helloworld.im.Contacts;
import com.example.helloworld.im.TcpClient;
import com.example.helloworld.ui.widget.AvatarView;
import com.google.android.material.bottomnavigation.BottomNavigationView;
import com.google.android.material.floatingactionbutton.FloatingActionButton;

/**
 * 主界面：底部三个 tab（消息 / 通讯录 / 我的）+ 顶栏身份与连接状态 + 快捷操作 FAB。
 *
 * <p>对应桌面端 MainWindow 的 railPanel（消息 / 通讯录）与 headerBar（我的头像 + 用户名 ID），
 * 移动端把左侧图标导航换成底部导航，把「新建群聊 / 添加好友」等入口收进 FAB。
 */
public class MainActivity extends BaseActivity {

    private static final String TAG_CONV = "conv";
    private static final String TAG_CONTACTS = "contacts";
    private static final String TAG_ME = "me";

    private TextView tvTabTitle;
    private TextView tvStatus;
    private TextView tvMeName;
    private TextView tvMeId;
    private AvatarView ivMe;
    private FloatingActionButton fab;
    private BottomNavigationView nav;

    private ConversationListFragment convFragment;
    private ContactsFragment contactsFragment;
    private MeFragment meFragment;

    private final ChatService.SimpleListener chatListener = new ChatService.SimpleListener() {
        @Override
        public void onTotalUnreadChanged(int total) {
            runOnUiThread(() -> setBadge(R.id.nav_conversation, total));
        }

        @Override
        public void onConnectionStateChanged(int state) {
            runOnUiThread(() -> renderStatus(state));
        }
    };

    private final Contacts.SimpleListener contactsListener = new Contacts.SimpleListener() {
        @Override
        public void onPendingChanged(int friendPending, int groupPending) {
            runOnUiThread(() -> setBadge(R.id.nav_contacts, friendPending + groupPending));
        }
    };

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        tvTabTitle = findViewById(R.id.tv_tab_title);
        tvStatus = findViewById(R.id.tv_status);
        tvMeName = findViewById(R.id.tv_me_name);
        tvMeId = findViewById(R.id.tv_me_id);
        ivMe = findViewById(R.id.iv_me);
        fab = findViewById(R.id.fab_add);
        nav = findViewById(R.id.bottom_nav);
        protectBarAboveNavigationBar(nav);

        // 主界面顶栏不是 view_header（没有返回键），要单独躲开状态栏
        protectBarBelowStatusBar(findViewById(R.id.app_bar));

        renderIdentity();
        requestNotificationPermission();
        findViewById(R.id.box_me).setOnClickListener(v -> selectTab(R.id.nav_me));
        ivMe.setOnClickListener(v -> selectTab(R.id.nav_me));

        nav.setOnItemSelectedListener(item -> {
            switchTab(item.getItemId());
            return true;
        });

        fab.setOnClickListener(this::showQuickActions);

        if (savedInstanceState == null) {
            switchTab(R.id.nav_conversation);
        } else {
            convFragment = (ConversationListFragment) getSupportFragmentManager().findFragmentByTag(TAG_CONV);
            contactsFragment = (ContactsFragment) getSupportFragmentManager().findFragmentByTag(TAG_CONTACTS);
            meFragment = (MeFragment) getSupportFragmentManager().findFragmentByTag(TAG_ME);
            // 进程被杀重建时 FragmentManager 会自动恢复三个 Fragment，
            // 但顶栏标题与 FAB 可见性是我们自己维护的，要按当前选中的 tab 重放一次
            switchTab(nav.getSelectedItemId());
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        renderIdentity();
        if (im == null) {
            return;
        }
        im.chat().addListener(chatListener);
        im.contacts().addListener(contactsListener);
        setBadge(R.id.nav_conversation, im.chat().totalUnread());
        setBadge(R.id.nav_contacts,
                im.contacts().friendPending() + im.contacts().groupPending());
        renderStatus(im.tcp().state());
        // 进主界面就先走一次 HTTP 对齐，不等 TCP（与桌面端 singleShot(0, syncAll) 一致）
        im.syncAll();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (im != null) {
            im.chat().removeListener(chatListener);
            im.contacts().removeListener(contactsListener);
        }
    }

    /**
     * Android 13 起通知是运行时权限。没授予时后台来消息只会静默进列表，
     * 用户以为「收不到消息」，所以进主界面主动问一次（拒绝过就不再骚扰）。
     */
    private void requestNotificationPermission() {
        if (android.os.Build.VERSION.SDK_INT < 33) {
            return;
        }
        int state = androidx.core.content.ContextCompat.checkSelfPermission(this,
                android.Manifest.permission.POST_NOTIFICATIONS);
        if (state != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{android.Manifest.permission.POST_NOTIFICATIONS}, 201);
        }
    }

    private void renderIdentity() {
        if (im == null || im.session() == null) {
            return;
        }
        long uid = im.session().userId();
        String name = im.session().username();
        tvMeName.setText(name.isEmpty() ? ("用户" + uid) : name);
        tvMeId.setText("ID: " + uid);
        ivMe.setPerson(uid, name);
    }

    private void renderStatus(int state) {
        int text;
        switch (state) {
            case TcpClient.STATE_LOGGED_IN:
                text = R.string.status_online;
                break;
            case TcpClient.STATE_CONNECTING:
            case TcpClient.STATE_CONNECTED:
                text = R.string.status_connecting;
                break;
            case TcpClient.STATE_FATAL:
                text = R.string.status_fatal;
                break;
            default:
                text = R.string.status_offline;
                break;
        }
        tvStatus.setText(text);
        tvStatus.setAlpha(state == TcpClient.STATE_LOGGED_IN ? 1f : 0.8f);
    }

    private void setBadge(int itemId, int count) {
        if (nav == null) {
            return;
        }
        if (count > 0) {
            nav.getOrCreateBadge(itemId).setNumber(count);
        } else {
            nav.removeBadge(itemId);
        }
    }

    private void selectTab(int itemId) {
        nav.setSelectedItemId(itemId);
    }

    /** add + hide/show 保留各 tab 的滚动位置与已加载数据 */
    private void switchTab(int itemId) {
        FragmentTransaction tx = getSupportFragmentManager().beginTransaction();
        hideAll(tx);

        String tag;
        int titleRes;
        boolean showFab;
        if (itemId == R.id.nav_contacts) {
            tag = TAG_CONTACTS;
            titleRes = R.string.nav_contacts;
            showFab = true;
            if (contactsFragment == null) {
                contactsFragment = new ContactsFragment();
                tx.add(R.id.fragment_container, contactsFragment, tag);
            } else {
                tx.show(contactsFragment);
            }
        } else if (itemId == R.id.nav_me) {
            tag = TAG_ME;
            titleRes = R.string.nav_me;
            showFab = false;
            if (meFragment == null) {
                meFragment = new MeFragment();
                tx.add(R.id.fragment_container, meFragment, tag);
            } else {
                tx.show(meFragment);
            }
        } else {
            tag = TAG_CONV;
            titleRes = R.string.nav_conversation;
            showFab = true;
            if (convFragment == null) {
                convFragment = new ConversationListFragment();
                tx.add(R.id.fragment_container, convFragment, tag);
            } else {
                tx.show(convFragment);
            }
        }
        tx.commitAllowingStateLoss();
        tvTabTitle.setText(titleRes);
        fab.setVisibility(showFab ? View.VISIBLE : View.GONE);
    }

    private void hideAll(FragmentTransaction tx) {
        if (convFragment != null) {
            tx.hide(convFragment);
        }
        if (contactsFragment != null) {
            tx.hide(contactsFragment);
        }
        if (meFragment != null) {
            tx.hide(meFragment);
        }
    }

    private void showQuickActions(View anchor) {
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenuInflater().inflate(R.menu.menu_quick_actions, menu.getMenu());
        menu.setOnMenuItemClickListener(item -> {
            int id = item.getItemId();
            if (id == R.id.act_add_friend) {
                startActivity(new Intent(this, AddFriendActivity.class));
            } else if (id == R.id.act_create_group) {
                startActivity(new Intent(this, CreateGroupActivity.class));
            } else if (id == R.id.act_join_group) {
                startActivity(new Intent(this, GroupJoinActivity.class));
            } else if (id == R.id.act_multi_send) {
                startActivity(new Intent(this, MultiSendActivity.class));
            }
            return true;
        });
        menu.show();
    }
}
