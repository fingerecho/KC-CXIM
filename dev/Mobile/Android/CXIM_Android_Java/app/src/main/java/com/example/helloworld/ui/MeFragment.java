package com.example.helloworld.ui;

import android.content.Intent;
import android.graphics.Color;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import com.example.helloworld.CXIMApp;
import com.example.helloworld.R;
import com.example.helloworld.im.ChatService;
import com.example.helloworld.im.Config;
import com.example.helloworld.im.Contacts;
import com.example.helloworld.im.ImService;
import com.example.helloworld.im.TcpClient;
import com.example.helloworld.ui.widget.AvatarView;

/**
 * 我的 tab：身份信息、连接状态、服务器地址、通讯录统计与退出登录。
 */
public class MeFragment extends Fragment {

    private ImService im;
    private TextView tvName;
    private TextView tvId;
    private TextView tvStatus;
    private TextView tvServer;
    private TextView tvFriendCount;
    private TextView tvGroupCount;
    private TextView tvPendingCount;
    private View dotStatus;
    private AvatarView avatar;

    private final ChatService.SimpleListener chatListener = new ChatService.SimpleListener() {
        @Override
        public void onConnectionStateChanged(int state) {
            if (getActivity() != null) {
                getActivity().runOnUiThread(() -> renderStatus(state));
            }
        }
    };

    private final Contacts.SimpleListener contactsListener = new Contacts.SimpleListener() {
        @Override
        public void onFriendsChanged() {
            renderCounts();
        }

        @Override
        public void onGroupsChanged() {
            renderCounts();
        }

        @Override
        public void onPendingChanged(int friendPending, int groupPending) {
            renderCounts();
        }
    };

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_me, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        im = CXIMApp.service();

        tvName = view.findViewById(R.id.tv_name);
        tvId = view.findViewById(R.id.tv_id);
        tvStatus = view.findViewById(R.id.tv_status);
        tvServer = view.findViewById(R.id.tv_server);
        tvFriendCount = view.findViewById(R.id.tv_friend_count);
        tvGroupCount = view.findViewById(R.id.tv_group_count);
        tvPendingCount = view.findViewById(R.id.tv_pending_count);
        dotStatus = view.findViewById(R.id.dot_status);
        avatar = view.findViewById(R.id.iv_avatar);

        view.findViewById(R.id.row_server).setOnClickListener(v ->
                startActivity(new Intent(requireContext(), ServerSettingsActivity.class)));
        view.findViewById(R.id.row_status).setOnClickListener(v -> {
            if (im != null) {
                im.startDataPlane();
                Ui.toast(requireContext(), "正在重新连接…");
            }
        });
        view.findViewById(R.id.btn_logout).setOnClickListener(v ->
                Ui.confirm(requireContext(), getString(R.string.me_logout),
                        getString(R.string.me_logout_confirm), this::doLogout));
    }

    @Override
    public void onResume() {
        super.onResume();
        if (im != null) {
            im.chat().addListener(chatListener);
            im.contacts().addListener(contactsListener);
            renderIdentity();
            renderStatus(im.tcp().state());
            renderCounts();
        }
        tvServer.setText(Config.httpHost(requireContext()) + ":" + Config.httpPort(requireContext()));
    }

    @Override
    public void onPause() {
        super.onPause();
        if (im != null) {
            im.chat().removeListener(chatListener);
            im.contacts().removeListener(contactsListener);
        }
    }

    private void renderIdentity() {
        long uid = im.session().userId();
        String name = im.session().username();
        tvName.setText(name.isEmpty() ? ("用户" + uid) : name);
        tvId.setText("用户 ID：" + uid);
        avatar.setPerson(uid, name);
    }

    private void renderStatus(int state) {
        String text;
        int color;
        switch (state) {
            case TcpClient.STATE_LOGGED_IN:
                text = getString(R.string.status_online);
                color = Color.parseColor("#2ECC71");
                break;
            case TcpClient.STATE_CONNECTING:
            case TcpClient.STATE_CONNECTED:
                text = getString(R.string.status_connecting);
                color = Color.parseColor("#E8A25C");
                break;
            case TcpClient.STATE_FATAL:
                text = getString(R.string.status_fatal);
                color = Color.parseColor("#E04A4A");
                break;
            default:
                text = getString(R.string.status_offline);
                color = Color.parseColor("#B0B5BB");
                break;
        }
        tvStatus.setText(text);
        // 用 tint 而不是 setBackgroundColor，否则会把圆形 shape 换成方形 ColorDrawable
        android.graphics.drawable.Drawable dot = dotStatus.getBackground();
        if (dot != null) {
            dot.mutate().setTint(color);
        }
    }

    private void renderCounts() {
        if (im == null) {
            return;
        }
        tvFriendCount.setText(String.valueOf(im.contacts().friends().size()));
        tvGroupCount.setText(String.valueOf(im.contacts().groups().size()));
        tvPendingCount.setText(String.valueOf(
                im.contacts().friendPending() + im.contacts().groupPending()));
    }

    private void doLogout() {
        if (im == null) {
            return;
        }
        im.logout();
        Ui.gotoLogin(requireContext());
    }
}
