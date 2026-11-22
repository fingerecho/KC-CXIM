package com.example.helloworld.ui;

import android.app.Notification;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Build;

import androidx.core.app.NotificationCompat;

import com.example.helloworld.CXIMApp;
import com.example.helloworld.R;
import com.example.helloworld.im.ChatService;
import com.example.helloworld.im.ImService;
import com.example.helloworld.im.model.ChatMessage;
import com.example.helloworld.im.model.Conversation;
import com.google.gson.JsonObject;

/**
 * 全局通知器：应用退到后台时，把新消息 / 好友申请 / 入群申请转成系统通知。
 *
 * <p>前台时不弹通知，交给界面内的角标与列表刷新，避免重复打扰。
 * 前后台判定用 Activity 计数（不引入 lifecycle-process 依赖）。
 */
public class Notifier extends ChatService.SimpleListener {

    private static final int NOTIFY_MESSAGE = 1001;
    private static final int NOTIFY_FRIEND = 1002;
    private static final int NOTIFY_GROUP = 1003;

    private final Context context;
    private final ImService im;
    private int startedActivities;

    public Notifier(Context context, ImService im) {
        this.context = context.getApplicationContext();
        this.im = im;
        im.chat().addListener(this);
    }

    /** 由 Application 的 ActivityLifecycleCallbacks 调用 */
    public void onActivityStarted() {
        startedActivities++;
    }

    public void onActivityStopped() {
        startedActivities = Math.max(0, startedActivities - 1);
    }

    private boolean inForeground() {
        return startedActivities > 0;
    }

    private NotificationManager nm() {
        return (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
    }

    /**
     * 发一条通知。
     *
     * <p>extras 必须带全：ChatActivity 靠 peer_id/is_group/title 定位会话，
     * 缺省时 peer_id=0 会打开一个「用户0」的假会话，并向服务端发出 to=0 的历史请求
     * （换来 bad peer 错误帧）。requestCode 由会话键派生，避免 FLAG_UPDATE_CURRENT
     * 把上一个会话的 Intent 复用过来。
     */
    private void post(int baseId, String title, String text, Class<?> target, Bundle extras) {
        if (inForeground()) {
            return;
        }
        NotificationManager manager = nm();
        if (manager == null) {
            return;
        }
        Intent intent = new Intent(context, target);
        if (extras != null) {
            intent.putExtras(extras);
        }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }
        int requestCode = baseId;
        if (extras != null) {
            // 用 peer_id 区分不同会话的通知，不同会话不会互相覆盖
            requestCode = baseId * 1_000_003
                    + (int) (extras.getLong("peer_id", 0) & 0xFFFF)
                    + (extras.getBoolean("is_group", false) ? 1 : 0);
        }
        PendingIntent pi = PendingIntent.getActivity(context, requestCode, intent, flags);

        Notification n = new NotificationCompat.Builder(context, CXIMApp.CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_chat)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new NotificationCompat.BigTextStyle().bigText(text))
                .setContentIntent(pi)
                .setAutoCancel(true)
                .setDefaults(Notification.DEFAULT_ALL)
                .build();
        try {
            // 按会话分通知 id，多个会话的消息不会合并成一条
            manager.notify(requestCode, n);
        } catch (SecurityException ignored) {
            // Android 13+ 未授予 POST_NOTIFICATIONS 时静默忽略，不打断收消息
        }
    }

    /** 无需定位会话的通知（申请类事件、解散提示等） */
    private void post(int baseId, String title, String text, Class<?> target) {
        post(baseId, title, text, target, null);
    }

    private String titleOf(ChatMessage m) {
        Conversation c = im.chat().conversation(m.convKey(im.session().userId()));
        if (c != null && c.title != null && !c.title.isEmpty()) {
            return c.title;
        }
        return im.contacts().titleOf(m.peerId(im.session().userId()), m.isGroup);
    }

    // ===================================================================
    //  ChatService.Listener
    // ===================================================================

    @Override
    public void onMessageIncoming(String convKey, ChatMessage message) {
        if (message.outgoing) {
            return;
        }
        String title = titleOf(message);
        String body = message.preview();
        long peer = message.peerId(im.session().userId());
        if (message.isGroup) {
            String sender = im.contacts().senderName(message.toUid, message.fromUid);
            body = sender + ": " + body;
        }
        // 点通知要直达这个会话，所以 extras 必须带全
        Bundle extras = new Bundle();
        extras.putLong("peer_id", peer);
        extras.putBoolean("is_group", message.isGroup);
        extras.putString("title", title);
        post(NOTIFY_MESSAGE, title, body, ChatActivity.class, extras);
    }

    @Override
    public void onFriendRequestEvent(JsonObject payload) {
        String name = com.example.helloworld.im.Json.str(payload, "requester_nickname");
        if (name.isEmpty()) {
            name = com.example.helloworld.im.Json.str(payload, "requester_username");
        }
        if (name.isEmpty()) {
            name = "有人";
        }
        post(NOTIFY_FRIEND, "新的好友申请", name + " 请求添加你为好友", FriendRequestsActivity.class);
        im.refreshPendingCounts();
    }

    @Override
    public void onFriendRequestResultEvent(JsonObject payload) {
        String event = com.example.helloworld.im.Json.str(payload, "event");
        String name = com.example.helloworld.im.Json.str(payload, "target_nickname");
        if (name.isEmpty()) {
            name = com.example.helloworld.im.Json.str(payload, "target_username");
        }
        boolean accepted = "accepted".equals(event);
        post(NOTIFY_FRIEND, accepted ? "好友申请已通过" : "好友申请被拒绝",
                (name.isEmpty() ? "对方" : name) + (accepted ? " 已同意你的好友申请" : " 拒绝了你的好友申请"),
                FriendRequestsActivity.class);
        im.refreshFriends();
    }

    @Override
    public void onFriendListChangedEvent(JsonObject payload) {
        im.refreshFriends();
    }

    /**
     * 143 的 payload 口径与 130/140 不同：协议 §5.13 的示例里入群申请事件用的是
     * **camelCase**（requestId/groupId/groupName/requesterNickname），
     * 只有 130/131/132 与 140/141/142 是 snake_case。这里必须按 camelCase 读，
     * 否则通知里永远是「有人 申请加入群聊」。
     */
    @Override
    public void onGroupRequestEvent(JsonObject payload) {
        String name = com.example.helloworld.im.Json.str(payload, "requesterNickname");
        if (name.isEmpty()) {
            name = com.example.helloworld.im.Json.str(payload, "requesterUsername");
        }
        String groupName = com.example.helloworld.im.Json.str(payload, "groupName");
        post(NOTIFY_GROUP, "新的入群申请",
                (name.isEmpty() ? "有人" : name) + " 申请加入" + (groupName.isEmpty() ? "群聊" : "「" + groupName + "」"),
                GroupRequestsActivity.class);
        im.refreshPendingCounts();
    }

    @Override
    public void onGroupRequestResultEvent(JsonObject payload) {
        String event = com.example.helloworld.im.Json.str(payload, "event");
        boolean accepted = "accepted".equals(event);
        post(NOTIFY_GROUP, accepted ? "入群申请已通过" : "入群申请被拒绝",
                accepted ? "你已加入群聊" : "群主或管理员拒绝了你的入群申请",
                GroupRequestsActivity.class);
        im.refreshGroups();
    }

    @Override
    public void onGroupMemberChangedEvent(JsonObject payload) {
        im.refreshGroups();
    }

    @Override
    public void onGroupDismissedEvent(JsonObject payload) {
        long groupId = com.example.helloworld.im.Json.num(payload, "group_id");
        String name = com.example.helloworld.im.Json.str(payload, "name");
        if (groupId != 0) {
            im.chat().clearConversation(groupId, true);
            im.contacts().removeGroup(groupId);
        }
        post(NOTIFY_GROUP, "群聊已解散",
                (name.isEmpty() ? ("群" + groupId) : name) + " 已被群主解散", MainActivity.class);
    }

    @Override
    public void onGroupInfoChangedEvent(JsonObject payload) {
        im.refreshGroups();
    }

}
