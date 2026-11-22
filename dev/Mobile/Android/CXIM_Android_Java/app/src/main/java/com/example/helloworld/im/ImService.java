package com.example.helloworld.im;

import android.content.Context;

import com.example.helloworld.im.model.Conversation;
import com.example.helloworld.im.model.FriendInfo;
import com.example.helloworld.im.model.GroupInfo;

import java.util.ArrayList;
import java.util.List;

/**
 * 应用级门面（对齐桌面端 ImService 的组装根）：
 * Config → Session → HttpClient → TcpClient → FileUploader → Account/Friend/Group/Chat。
 *
 * <p>职责：
 * <ul>
 *   <li>把 401 刷新钩子接到 AccountService，把 unauthorized 转成 sessionExpired</li>
 *   <li>TCP 重连前用 refresh token 换 access token（协议 §4.3：不要让重连变成 bcrypt 风暴）</li>
 *   <li>TCP 登录成功后做全量 HTTP 对齐（好友/群/待审批数），弥补不持久化的事件</li>
 *   <li>通讯录变化时回填会话标题与头像</li>
 * </ul>
 */
public class ImService {

    public interface SessionListener {
        /** token 全部失效，UI 应回登录页 */
        void onSessionExpired(String reason);

        void onConnectionStateChanged(int state);

        /** TCP 登录成功 */
        void onDataPlaneReady();
    }

    public static class SimpleSessionListener implements SessionListener {
        @Override
        public void onSessionExpired(String reason) {
        }

        @Override
        public void onConnectionStateChanged(int state) {
        }

        @Override
        public void onDataPlaneReady() {
        }
    }

    private final Context context;
    private final Session session;
    private final HttpClient http;
    private final TcpClient tcp;
    private final AccountService account;
    private final FriendService friends;
    private final GroupService groups;
    private final ChatService chat;
    private final FileUploader uploader;
    private final Contacts contacts;

    private final List<SessionListener> sessionListeners = new ArrayList<>();
    private volatile boolean expiredNotified;
    /** 连续「登录被拒但刷新成功」的次数，用于退避；登录成功后清零 */
    private volatile int consecutiveFatal;
    private final android.os.Handler reconnectHandler =
            new android.os.Handler(android.os.Looper.getMainLooper());

    public ImService(Context context) {
        this.context = context.getApplicationContext();
        this.session = new Session(this.context);
        this.http = new HttpClient(this.context, session);
        this.account = new AccountService(http, session);
        this.friends = new FriendService(http);
        this.groups = new GroupService(http);
        this.contacts = new Contacts();
        this.uploader = new FileUploader(this.context, http);
        this.tcp = new TcpClient(new TcpListener());
        this.tcp.setHeartbeatIntervalMs(Config.heartbeatMs(this.context));
        this.chat = new ChatService(tcp, session, Config.historyPage(this.context));

        // 401 → 同步刷新 → 重放；刷新失败 → sessionExpired
        http.setTokenRefresher(account::refreshSync);
        http.setUnauthorizedListener(() -> expire("登录已过期，请重新登录"));

        // 通讯录到位后回填会话标题/头像，会话列表才不会一直显示「用户1002」
        contacts.addListener(new Contacts.SimpleListener() {
            @Override
            public void onFriendsChanged() {
                applyConversationMeta();
            }

            @Override
            public void onGroupsChanged() {
                applyConversationMeta();
            }
        });
    }

    // ===== 访问器 =====

    public Context context() {
        return context;
    }

    public Session session() {
        return session;
    }

    public HttpClient http() {
        return http;
    }

    public TcpClient tcp() {
        return tcp;
    }

    public AccountService account() {
        return account;
    }

    public FriendService friend() {
        return friends;
    }

    public GroupService group() {
        return groups;
    }

    public ChatService chat() {
        return chat;
    }

    public FileUploader uploader() {
        return uploader;
    }

    public Contacts contacts() {
        return contacts;
    }

    public void addSessionListener(SessionListener l) {
        synchronized (sessionListeners) {
            if (l != null && !sessionListeners.contains(l)) {
                sessionListeners.add(l);
            }
        }
    }

    public void removeSessionListener(SessionListener l) {
        synchronized (sessionListeners) {
            sessionListeners.remove(l);
        }
    }

    private List<SessionListener> sessionSnapshot() {
        synchronized (sessionListeners) {
            return new ArrayList<>(sessionListeners);
        }
    }

    // ===== 数据面 =====

    /** 登录成功后启动长连接 */
    public void startDataPlane() {
        if (!session.logged()) {
            return;
        }
        expiredNotified = false;
        tcp.setHeartbeatIntervalMs(Config.heartbeatMs(context));
        tcp.start(Config.tcpHost(context), Config.tcpPort(context), session.accessToken());
    }

    public void shutdownDataPlane() {
        tcp.stop();
    }

    /** 退出登录：停连接 + 清凭证 + 清本地缓存（服务端无登出接口） */
    public void logout() {
        tcp.stop();
        // 重新登录后退避要从头算，否则上一个账号留下的计数会把新账号的首次重连拖到 30s
        consecutiveFatal = 0;
        reconnectHandler.removeCallbacksAndMessages(null);
        chat.clearAll();
        contacts.clear();
        session.clear();
    }

    public boolean isLoggedIn() {
        return session.logged();
    }

    /**
     * 全量对齐。TCP 登录成功后必须调一次：132/140/141 这类事件不持久化，
     * 离线期间的变更只能靠重新拉全量列表补上。
     */
    public void syncAll() {
        refreshFriends();
        refreshGroups();
        refreshPendingCounts();
    }

    public void refreshFriends() {
        friends.listFriends(r -> {
            if (r.ok) {
                contacts.setFriends(FriendService.parseFriends(r));
            }
        });
    }

    public void refreshGroups() {
        groups.listGroups(r -> {
            if (r.ok) {
                contacts.setGroups(GroupService.parseGroups(r));
            }
        });
    }

    /** 待我处理的好友申请数 + 待我审批的入群申请数 */
    public void refreshPendingCounts() {
        friends.listRequests("incoming", fr -> {
            int fp = fr.ok ? FriendService.countPending(fr) : contacts.friendPending();
            groups.listJoinRequests("incoming", gr -> {
                int gp = gr.ok ? GroupService.countPending(gr) : contacts.groupPending();
                contacts.setPending(fp, gp);
            });
        });
    }

    /** 群详情并写入缓存 */
    public void groupDetail(long groupId, GroupService.DetailCb cb) {
        groups.groupDetail(groupId, (ok, error, group, members) -> {
            if (ok) {
                contacts.putGroup(group);
                contacts.setMembers(groupId, members);
            }
            cb.onResult(ok, error, group, members);
        });
    }

    /** 把通讯录里的名字/头像回填到会话上 */
    private void applyConversationMeta() {
        for (Conversation c : chat.conversations()) {
            String title = contacts.titleOf(c.peerId, c.isGroup);
            String avatar = c.isGroup
                    ? (contacts.group(c.peerId) == null ? "" : contacts.group(c.peerId).avatarUrl)
                    : (contacts.friend(c.peerId) == null ? "" : contacts.friend(c.peerId).avatarUrl);
            chat.updateConversationMeta(c.peerId, c.isGroup, title, avatar);
        }
    }

    private void expire(String reason) {
        if (expiredNotified) {
            return;
        }
        expiredNotified = true;
        tcp.stop();
        for (SessionListener l : sessionSnapshot()) {
            l.onSessionExpired(reason);
        }
    }

    // ===================================================================
    //  TCP 回调
    // ===================================================================

    private class TcpListener implements TcpClient.Listener {

        @Override
        public void onStateChanged(int state) {
            chat.notifyConnectionStateChanged(state);
            for (SessionListener l : sessionSnapshot()) {
                l.onConnectionStateChanged(state);
            }
        }

        @Override
        public void onLoginResult(boolean success, String reason) {
            if (success) {
                expiredNotified = false;
                consecutiveFatal = 0;
                chat.notifyTcpLoggedIn();
                for (SessionListener l : sessionSnapshot()) {
                    l.onDataPlaneReady();
                }
                // 登录成功即全量对齐，UI 不必各自实现
                syncAll();
                return;
            }
            // 到这里只代表「登录成功后的连接断开」，退避重连由 TcpClient 自己负责；
            // 待确认消息全部标失败，用户重发时复用同一 clientMsgId（协议 §6.2）
            chat.onDisconnected(reason == null || reason.isEmpty() ? "连接已断开" : reason);
        }

        /**
         * 登录阶段的 token 失效：先试 refresh token，成功就重连，
         * 失败才判会话过期（协议 §4.3 的客户端策略）。
         */
        @Override
        public void onLoginFatal(String reason) {
            chat.onDisconnected(reason == null || reason.isEmpty() ? "登录失效" : reason);
            if (!session.hasRefreshToken()) {
                expire(reason == null || reason.isEmpty() ? "登录已过期，请重新登录" : reason);
                return;
            }
            // force=true：服务端已经明确拒绝当前 access，
            // 不能因为「本地看还没过期」而拿同一个被拒的 token 反复重连
            account.refreshAsync(true, r -> {
                if (!r.ok) {
                    expire("登录已过期，请重新登录");
                    return;
                }
                // 刷新成功但仍被拒时必须退避，否则会变成每秒一次的 refresh + 连接风暴
                // （协议 §3.3 第 4 条：重连一律带抖动退避）
                reconnectHandler.postDelayed(() -> tcp.start(
                        Config.tcpHost(context), Config.tcpPort(context),
                        session.accessToken()), fatalDelayMs());
            });
        }

        /** 第 n 次连续登录致命失败的重连延时：min(30s, 1s×2^n) + rand(0,1s) */
        private long fatalDelayMs() {
            int n = consecutiveFatal++;
            long base = 1000L;
            for (int i = 0; i < n && base < 30_000L; i++) {
                base *= 2;
            }
            return Math.min(base, 30_000L) + (long) (Math.random() * 1000);
        }

        @Override
        public void onSendFailed(Envelope env, String reason) {
            chat.onSendFailed(env, reason);
        }

        @Override
        public void onFrame(Envelope env) {
            chat.onFrame(env);
        }

        @Override
        public void onNeedFreshToken(TcpClient.TokenCallback cb) {
            String current = session.accessToken();
            if (current.isEmpty()) {
                cb.onToken(null);
                return;
            }
            // 快过期才刷新，避免每次重连都多一次往返
            if (session.hasRefreshToken() && session.accessTokenExpiring(120)) {
                account.refreshAsync(r -> cb.onToken(r.ok ? session.accessToken() : current));
            } else {
                cb.onToken(current);
            }
        }
    }

    /** 便捷方法：给 UI 拿当前账号的好友展示名 */
    public String displayName(long userId) {
        FriendInfo f = contacts.friend(userId);
        return f == null ? ("用户" + userId) : f.displayName();
    }

    /** 便捷方法：群名 */
    public String groupName(long groupId) {
        GroupInfo g = contacts.group(groupId);
        return g == null ? ("群" + groupId) : g.displayName();
    }
}
