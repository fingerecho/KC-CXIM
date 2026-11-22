package com.example.helloworld.im;

import com.example.helloworld.im.model.ChatMessage;
import com.example.helloworld.im.model.Conversation;
import com.example.helloworld.im.model.MediaMeta;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.google.gson.JsonSyntaxException;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Deque;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicLong;

/**
 * 聊天数据面服务：消息收发、会话聚合、历史分页、回执、事件分发。
 *
 * <p>关键协议约束：
 * <ul>
 *   <li>{@code msg_id} 是客户端去重键，重试必须保持不变；服务端按 (会话, 发送者, msg_id) 去重，
 *       命中时 SendAck 的 content 为 "duplicate"</li>
 *   <li>SendAck(113) 不带客户端 msg_id，只能按 FIFO 与待确认项配对（与桌面端一致）</li>
 *   <li>HistoryResp(111) 的 content 是「JSON 数组的字符串」，必须二次解析</li>
 *   <li>事件 130~144 的 content 同样是 JSON 字符串，只承载事件，事实要回到 HTTP 全量对齐</li>
 *   <li>收到在线消息立即回 delivered 回执；打开会话时回已读回执（content 非 "delivered" 即视为已读）</li>
 * </ul>
 *
 * <p>所有 listener 回调都在主线程触发。
 */
public class ChatService {

    public interface Listener {
        /** TCP 登录成功，UI 应做全量 HTTP 对齐（好友/群列表） */
        void onTcpLoggedIn();

        /** 102 错误帧原文，UI 轻提示 */
        void onTcpError(String reason);

        void onConnectionStateChanged(int state);

        /** 下行消息（在线推送或登录补推） */
        void onMessageIncoming(String convKey, ChatMessage message);

        /** 本地发送产生的占位消息 */
        void onMessageLocalAdded(String convKey, ChatMessage message);

        void onMessageStatusChanged(String convKey, long clientMsgId,
                                    ChatMessage.Status status, String errorText);

        void onHistoryLoaded(String convKey, List<ChatMessage> items, long oldestSeq, boolean reachedEnd);

        void onConversationChanged(String convKey, Conversation conversation);

        void onTotalUnreadChanged(int total);

        void onFriendRequestEvent(JsonObject payload);

        void onFriendRequestResultEvent(JsonObject payload);

        void onFriendListChangedEvent(JsonObject payload);

        void onGroupMemberChangedEvent(JsonObject payload);

        void onGroupDismissedEvent(JsonObject payload);

        void onGroupInfoChangedEvent(JsonObject payload);

        void onGroupRequestEvent(JsonObject payload);

        void onGroupRequestResultEvent(JsonObject payload);
    }

    /** 空实现，便于只关心部分回调的调用方 */
    public static class SimpleListener implements Listener {
        @Override
        public void onTcpLoggedIn() {
        }

        @Override
        public void onTcpError(String reason) {
        }

        @Override
        public void onConnectionStateChanged(int state) {
        }

        @Override
        public void onMessageIncoming(String convKey, ChatMessage message) {
        }

        @Override
        public void onMessageLocalAdded(String convKey, ChatMessage message) {
        }

        @Override
        public void onMessageStatusChanged(String convKey, long clientMsgId,
                                           ChatMessage.Status status, String errorText) {
        }

        @Override
        public void onHistoryLoaded(String convKey, List<ChatMessage> items, long oldestSeq,
                                    boolean reachedEnd) {
        }

        @Override
        public void onConversationChanged(String convKey, Conversation conversation) {
        }

        @Override
        public void onTotalUnreadChanged(int total) {
        }

        @Override
        public void onFriendRequestEvent(JsonObject payload) {
        }

        @Override
        public void onFriendRequestResultEvent(JsonObject payload) {
        }

        @Override
        public void onFriendListChangedEvent(JsonObject payload) {
        }

        @Override
        public void onGroupMemberChangedEvent(JsonObject payload) {
        }

        @Override
        public void onGroupDismissedEvent(JsonObject payload) {
        }

        @Override
        public void onGroupInfoChangedEvent(JsonObject payload) {
        }

        @Override
        public void onGroupRequestEvent(JsonObject payload) {
        }

        @Override
        public void onGroupRequestResultEvent(JsonObject payload) {
        }
    }

    // ===================================================================
    //  内部状态
    // ===================================================================

    private static class PeerState {
        final long peerId;
        final boolean isGroup;
        /** seq 升序；本地发送中（seq=0）排最后 */
        final List<ChatMessage> items = new ArrayList<>();
        /** 去重键 "conversation_id:seq" */
        final Map<String, ChatMessage> seen = new HashMap<>();
        final Conversation conv;
        boolean firstHistoryLoaded;
        boolean reachedEnd;
        long lastSeq;
        long convId;
        long oldestSeq = Long.MAX_VALUE;

        PeerState(long peerId, boolean isGroup) {
            this.peerId = peerId;
            this.isGroup = isGroup;
            this.conv = new Conversation(peerId, isGroup);
        }
    }

    private static class PendingSend {
        final long clientMsgId;
        final String key;
        final boolean isGroup;

        PendingSend(long clientMsgId, String key, boolean isGroup) {
            this.clientMsgId = clientMsgId;
            this.key = key;
            this.isGroup = isGroup;
        }
    }

    private static class HistoryReq {
        final long peerId;
        final boolean isGroup;
        final long beforeSeq;
        final int count;

        HistoryReq(long peerId, boolean isGroup, long beforeSeq, int count) {
            this.peerId = peerId;
            this.isGroup = isGroup;
            this.beforeSeq = beforeSeq;
            this.count = count;
        }
    }

    private static final Comparator<ChatMessage> BY_SEQ = (a, b) -> {
        long sa = a.seq > 0 ? a.seq : Long.MAX_VALUE;
        long sb = b.seq > 0 ? b.seq : Long.MAX_VALUE;
        int c = Long.compare(sa, sb);
        if (c != 0) {
            return c;
        }
        return Long.compare(a.clientMsgId, b.clientMsgId);
    };

    private final TcpClient tcp;
    private final Session session;
    private final int historyPage;

    private final Map<String, PeerState> peers = new HashMap<>();
    /** SendAck / Error 的 FIFO 配对队列 */
    private final Deque<PendingSend> pendingSends = new ArrayDeque<>();
    /** clientMsgId -> 本地消息，重发时复用同一条 */
    private final Map<Long, ChatMessage> pendingById = new HashMap<>();
    private final Deque<HistoryReq> historyFifo = new ArrayDeque<>();
    private HistoryReq historyInFlight;

    private final List<Listener> listeners = new ArrayList<>();
    private final AtomicLong clientMsgCounter = new AtomicLong(0);

    /** server busy 的自动重试上限（协议要求降速重试，但绝不能无限重试） */
    private static final int MAX_BUSY_RETRY = 2;
    /** clientMsgId → 已重试次数 */
    private final Map<Long, Integer> busyRetries = new HashMap<>();
    private final android.os.Handler mainHandler =
            new android.os.Handler(android.os.Looper.getMainLooper());

    /** 当前打开的会话键：新消息不计未读，且立刻补已读回执 */
    private volatile String activeConvKey = "";

    public ChatService(TcpClient tcp, Session session, int historyPage) {
        this.tcp = tcp;
        this.session = session;
        this.historyPage = historyPage > 0 ? Math.min(historyPage, 100) : Config.DEF_HISTORY_PAGE;
    }

    // ===================================================================
    //  Listener
    // ===================================================================

    public void addListener(Listener l) {
        synchronized (listeners) {
            if (l != null && !listeners.contains(l)) {
                listeners.add(l);
            }
        }
    }

    public void removeListener(Listener l) {
        synchronized (listeners) {
            listeners.remove(l);
        }
    }

    private List<Listener> snapshot() {
        synchronized (listeners) {
            return new ArrayList<>(listeners);
        }
    }

    public void setActiveConversation(String convKey) {
        activeConvKey = convKey == null ? "" : convKey;
    }

    // ===================================================================
    //  对外读取
    // ===================================================================

    public long selfId() {
        return session.userId();
    }

    public static String convKey(long peerId, boolean isGroup) {
        return Conversation.keyOf(peerId, isGroup);
    }

    /** 会话列表：只含有最后一条消息的会话，按 ts 降序 */
    public List<Conversation> conversations() {
        List<Conversation> out = new ArrayList<>();
        synchronized (peers) {
            for (PeerState st : peers.values()) {
                if (st.conv.hasLast) {
                    out.add(st.conv);
                }
            }
        }
        Collections.sort(out, (a, b) -> Long.compare(b.lastTs(), a.lastTs()));
        return out;
    }

    public Conversation conversation(String key) {
        synchronized (peers) {
            PeerState st = peers.get(key);
            return st == null ? null : st.conv;
        }
    }

    public List<ChatMessage> messages(String key) {
        synchronized (peers) {
            PeerState st = peers.get(key);
            return st == null ? new ArrayList<>() : new ArrayList<>(st.items);
        }
    }

    public int totalUnread() {
        int n = 0;
        synchronized (peers) {
            for (PeerState st : peers.values()) {
                n += st.conv.unread;
            }
        }
        return n;
    }

    public boolean historyLoadedOnce(String key) {
        synchronized (peers) {
            PeerState st = peers.get(key);
            return st != null && st.firstHistoryLoaded;
        }
    }

    public boolean reachedEnd(String key) {
        synchronized (peers) {
            PeerState st = peers.get(key);
            return st != null && st.reachedEnd;
        }
    }

    public boolean isReady() {
        return tcp.isLoggedIn();
    }

    /** 单次历史拉取条数（协议有效上限 100） */
    public int historyPageSize() {
        return historyPage;
    }

    /** 由 ImService 在 TCP 登录成功后调用 */
    public void notifyTcpLoggedIn() {
        for (Listener l : snapshot()) {
            l.onTcpLoggedIn();
        }
    }

    /** 由 ImService 在连接状态变化时调用 */
    public void notifyConnectionStateChanged(int state) {
        for (Listener l : snapshot()) {
            l.onConnectionStateChanged(state);
        }
    }

    // ===================================================================
    //  发送
    // ===================================================================

    /** 发送文本，返回客户端消息号；文本为空返回 0 */
    public long sendText(long peerId, boolean isGroup, String text) {
        String t = text == null ? "" : text.trim();
        if (t.isEmpty()) {
            return 0;
        }
        long cmid = nextClientMsgId();
        ChatMessage m = localMessage(cmid, peerId, isGroup, MsgType.TEXT, t, null);
        Envelope env = Envelope.uplink(MsgType.TEXT, peerId, cmid, 0, t, isGroup);
        dispatchSend(m, env);
        return cmid;
    }

    /** 发送媒体：上行 content 只带 file_id，服务端负责权属校验与元数据重建 */
    public long sendMedia(long peerId, boolean isGroup, int type, MediaMeta meta) {
        if (meta == null || meta.fileId.isEmpty()) {
            return 0;
        }
        long cmid = nextClientMsgId();
        ChatMessage m = localMessage(cmid, peerId, isGroup, type,
                MediaMeta.uploadContent(meta.fileId), meta);
        Envelope env = Envelope.uplink(type, peerId, cmid, 0,
                MediaMeta.uploadContent(meta.fileId), isGroup);
        dispatchSend(m, env);
        return cmid;
    }

    private void dispatchSend(ChatMessage m, Envelope env) {
        String key = m.convKey(selfId());
        PeerState st = stateFor(m.toUid, m.isGroup);
        synchronized (peers) {
            upsert(st, m);
        }
        if (TcpClient.envelopeBytes(env) > Config.MAX_UPLINK_BODY) {
            markFailed(key, m.clientMsgId, "消息过长，请分段发送");
            for (Listener l : snapshot()) {
                l.onMessageLocalAdded(key, m);
                l.onMessageStatusChanged(key, m.clientMsgId, ChatMessage.Status.FAILED,
                        "消息过长，请分段发送");
            }
            return;
        }
        pendingSends.addLast(new PendingSend(m.clientMsgId, key, m.isGroup));
        pendingById.put(m.clientMsgId, m);
        for (Listener l : snapshot()) {
            l.onMessageLocalAdded(key, m);
        }
        touchConversation(key, m, false);
        if (!tcp.sendEnvelope(env)) {
            markFailed(key, m.clientMsgId, "连接已断开");
            for (Listener l : snapshot()) {
                l.onMessageStatusChanged(key, m.clientMsgId, ChatMessage.Status.FAILED, "连接已断开");
            }
        }
    }

    /** 用户手动重发：复用同一 clientMsgId，并重新给满自动重试预算 */
    public void resend(long clientMsgId) {
        busyRetries.remove(clientMsgId);
        resendInternal(clientMsgId);
    }

    /** server busy 的自动重发：不动计数，避免把自己重试成无限循环 */
    private void resendAuto(long clientMsgId) {
        resendInternal(clientMsgId);
    }

    private void resendInternal(long clientMsgId) {
        ChatMessage m;
        synchronized (peers) {
            m = pendingById.get(clientMsgId);
        }
        if (m == null) {
            return;
        }
        m.status = ChatMessage.Status.SENDING;
        m.errorText = "";
        String key = m.convKey(selfId());
        int type = m.type;
        Envelope env;
        if (type == MsgType.TEXT) {
            env = Envelope.uplink(type, m.toUid, clientMsgId, 0, m.content, m.isGroup);
        } else {
            MediaMeta mm = m.meta();
            if (mm == null || mm.fileId.isEmpty()) {
                return;
            }
            env = Envelope.uplink(type, m.toUid, clientMsgId, 0,
                    MediaMeta.uploadContent(mm.fileId), m.isGroup);
        }
        pendingSends.addLast(new PendingSend(clientMsgId, key, m.isGroup));
        for (Listener l : snapshot()) {
            l.onMessageStatusChanged(key, clientMsgId, ChatMessage.Status.SENDING, "");
        }
        if (!tcp.sendEnvelope(env)) {
            markFailed(key, clientMsgId, "连接已断开");
            for (Listener l : snapshot()) {
                l.onMessageStatusChanged(key, clientMsgId, ChatMessage.Status.FAILED, "连接已断开");
            }
        }
    }

    /** 生成稳定且非 0 的客户端消息号 */
    private long nextClientMsgId() {
        return System.currentTimeMillis() * 16 + (clientMsgCounter.getAndIncrement() % 16);
    }

    private ChatMessage localMessage(long cmid, long peerId, boolean isGroup, int type,
                                     String content, MediaMeta meta) {
        ChatMessage m = new ChatMessage();
        m.clientMsgId = cmid;
        m.msgId = 0;
        m.seq = 0;
        m.conversationId = 0;
        m.fromUid = selfId();
        m.toUid = peerId;
        m.type = type;
        m.content = content;
        m.isGroup = isGroup;
        m.outgoing = true;
        m.status = ChatMessage.Status.SENDING;
        m.ts = System.currentTimeMillis();
        if (meta != null) {
            // 本地先把元数据补上，气泡立刻能显示文件名/缩略图，不用等 SendAck
            MediaMeta local = new MediaMeta();
            local.fileId = meta.fileId;
            local.name = meta.name;
            local.size = meta.size;
            local.mime = meta.mime;
            local.url = meta.url;
            local.qetag = meta.qetag;
            m.content = local.toJson().toString();
            m.attachLocalMeta(local);
        }
        return m;
    }

    // ===================================================================
    //  历史消息
    // ===================================================================

    /** 拉历史。串行化：同一时刻只允许一个 HistoryReq 在途，其余排队 */
    public void requestHistory(long peerId, boolean isGroup, long beforeSeq, int count) {
        int n = Math.max(1, Math.min(count, 100));
        if (!tcp.isLoggedIn()) {
            return;
        }
        HistoryReq req = new HistoryReq(peerId, isGroup, beforeSeq, n);
        historyFifo.addLast(req);
        pumpHistory();
    }

    private void pumpHistory() {
        if (historyInFlight != null || historyFifo.isEmpty()) {
            return;
        }
        historyInFlight = historyFifo.pollFirst();
        if (historyInFlight == null) {
            return;
        }
        Envelope env = Envelope.uplink(MsgType.HISTORY_REQ, historyInFlight.peerId, 0,
                historyInFlight.beforeSeq, String.valueOf(historyInFlight.count),
                historyInFlight.isGroup);
        if (!tcp.sendEnvelope(env)) {
            historyInFlight = null;
            pumpHistory();
        }
    }

    private void onHistoryResp(Envelope env) {
        HistoryReq req = historyInFlight;
        historyInFlight = null;
        try {
            List<ChatMessage> loaded = new ArrayList<>();
            long oldestSeq = Long.MAX_VALUE;
            if (req != null) {
                String key = convKey(req.peerId, req.isGroup);
                PeerState st = stateFor(req.peerId, req.isGroup);
                int parsed = 0;
                synchronized (peers) {
                    // content 是「JSON 数组的字符串」，必须二次解析
                    for (JsonObject o : Json.objList(parseArray(env.content))) {
                        ChatMessage m = ChatMessage.from(Envelope.fromJson(o), selfId());
                        // 判「翻到底」要看服务端返回的条数，去重掉的也要算进去：
                        // 先收到在线消息再拉历史时，若只看新增条数会被误判成没有更多
                        parsed++;
                        if (upsert(st, m)) {
                            loaded.add(m);
                        }
                        if (m.seq > 0 && m.seq < oldestSeq) {
                            oldestSeq = m.seq;
                        }
                        if (m.conversationId != 0) {
                            st.convId = m.conversationId;
                        }
                        if (m.seq > st.lastSeq) {
                            st.lastSeq = m.seq;
                        }
                    }
                    st.firstHistoryLoaded = true;
                    st.reachedEnd = parsed < req.count;
                    if (oldestSeq == Long.MAX_VALUE) {
                        oldestSeq = st.oldestSeq;
                    } else {
                        st.oldestSeq = Math.min(st.oldestSeq, oldestSeq);
                    }
                }
                Collections.sort(loaded, BY_SEQ);
                final long finalOldest = oldestSeq == Long.MAX_VALUE ? 0 : oldestSeq;
                final boolean end = st.reachedEnd;
                for (Listener l : snapshot()) {
                    l.onHistoryLoaded(key, messages(key), finalOldest, end);
                }
                touchConversation(key, null, false);
            }
        } finally {
            pumpHistory();
        }
    }

    private static com.google.gson.JsonArray parseArray(String content) {
        if (content == null || content.isEmpty()) {
            return new com.google.gson.JsonArray();
        }
        try {
            return JsonParser.parseString(content).getAsJsonArray();
        } catch (JsonSyntaxException | IllegalStateException e) {
            return new com.google.gson.JsonArray();
        }
    }

    // ===================================================================
    //  回执
    // ===================================================================

    /** 已读回执：content 非 "delivered" 即视为已读，只增不减、幂等 */
    public void markRead(long peerId, boolean isGroup) {
        PeerState st;
        synchronized (peers) {
            st = peers.get(convKey(peerId, isGroup));
            if (st == null || st.convId == 0 || st.lastSeq == 0) {
                if (st != null) {
                    st.conv.unread = 0;
                }
                return;
            }
        }
        tcp.sendEnvelope(Envelope.uplink(MsgType.ACK, 0, 0, st.lastSeq, "", isGroup)
                .withConversation(st.convId));
        st.conv.unread = 0;
        String key = st.conv.key();
        for (Listener l : snapshot()) {
            l.onConversationChanged(key, st.conv);
            l.onTotalUnreadChanged(totalUnread());
        }
    }

    /** 投递回执：收到在线消息立刻回 */
    private void sendDelivered(Envelope env) {
        if (env.conversationId == 0 || env.seq == 0) {
            return;
        }
        Envelope ack = Envelope.uplink(MsgType.ACK, 0, 0, env.seq, "delivered", env.isGroup);
        ack.conversationId = env.conversationId;
        tcp.sendEnvelope(ack);
    }

    // ===================================================================
    //  下行帧分发
    // ===================================================================

    public void onFrame(Envelope env) {
        switch (env.type) {
            case MsgType.TEXT:
            case MsgType.IMAGE:
            case MsgType.FILE:
                onIncomingMessage(env);
                break;
            case MsgType.SEND_ACK:
                onSendAck(env);
                break;
            case MsgType.HISTORY_RESP:
                onHistoryResp(env);
                break;
            case MsgType.ERROR:
                onError(env);
                break;
            case MsgType.FRIEND_REQUEST:
                dispatchEvent(env, 130);
                break;
            case MsgType.FRIEND_REQUEST_RESULT:
                dispatchEvent(env, 131);
                break;
            case MsgType.FRIEND_LIST_CHANGED:
                dispatchEvent(env, 132);
                break;
            case MsgType.GROUP_MEMBER_CHANGED:
                dispatchEvent(env, 140);
                break;
            case MsgType.GROUP_DISMISSED:
                dispatchEvent(env, 141);
                break;
            case MsgType.GROUP_INFO_CHANGED:
                dispatchEvent(env, 142);
                break;
            case MsgType.GROUP_REQUEST:
                dispatchEvent(env, 143);
                break;
            case MsgType.GROUP_REQUEST_RESULT:
                dispatchEvent(env, 144);
                break;
            default:
                // 未知帧忽略，不断连接
                break;
        }
    }

    private void onIncomingMessage(Envelope env) {
        long self = selfId();
        long peerId = env.peerId(self);
        if (peerId == 0) {
            return;
        }
        ChatMessage m = ChatMessage.from(env, self);
        m.outgoing = false;
        String key = convKey(peerId, env.isGroup);
        PeerState st = stateFor(peerId, env.isGroup);
        boolean added;
        synchronized (peers) {
            added = upsert(st, m);
            if (env.conversationId != 0) {
                st.convId = env.conversationId;
            }
            if (env.seq > st.lastSeq) {
                st.lastSeq = env.seq;
            }
        }
        if (!added) {
            // 离线补推与历史重叠导致的重复帧，直接丢弃
            return;
        }
        sendDelivered(env);
        boolean isActive = key.equals(activeConvKey);
        touchConversation(key, m, !isActive);
        for (Listener l : snapshot()) {
            l.onMessageIncoming(key, m);
        }
        if (isActive) {
            markRead(peerId, env.isGroup);
        }
    }

    private void onSendAck(Envelope env) {
        /*
         * SendAck 不回带客户端 msg_id，配对只能推断。
         * 优先用 conversation_id 精确匹配：只要该会话的 convId 已知（收到过下行消息或拉过历史），
         * 就能定位到真正的那条待确认消息；推断不出来时才退回「同 is_group 的队首」。
         * 这样同时给多个好友发消息时，seq / conversation_id 不会记错会话。
         */
        PendingSend p = null;
        List<PendingSend> candidates;
        synchronized (pendingSends) {
            candidates = new ArrayList<>(pendingSends);
        }
        // 只持一把锁做匹配，避免与 peers 形成嵌套（其他地方都是 peers → pendingSends 的顺序）
        synchronized (peers) {
            if (env.conversationId != 0) {
                for (PendingSend cand : candidates) {
                    PeerState candState = peers.get(cand.key);
                    if (candState != null && candState.convId == env.conversationId
                            && candState.isGroup == env.isGroup) {
                        p = cand;
                        break;
                    }
                }
            }
            if (p == null) {
                // 退回桌面端口径：同一 is_group 的队首
                for (PendingSend cand : candidates) {
                    if (cand.isGroup == env.isGroup) {
                        p = cand;
                        break;
                    }
                }
            }
        }
        if (p != null) {
            synchronized (pendingSends) {
                pendingSends.remove(p);
            }
            busyRetries.remove(p.clientMsgId);
        }
        if (p == null) {
            return;
        }
        boolean duplicate = "duplicate".equals(env.content);
        ChatMessage.Status status = duplicate ? ChatMessage.Status.DUPLICATE : ChatMessage.Status.SENT;
        ChatMessage local = pendingById.remove(p.clientMsgId);
        String key = p.key;
        PeerState st;
        synchronized (peers) {
            st = peers.get(key);
            if (st != null && local != null) {
                local.msgId = env.msgId;
                local.seq = env.seq;
                local.conversationId = env.conversationId;
                if (env.ts != 0) {
                    local.ts = env.ts;
                }
                local.status = status;
                local.errorText = "";
                if (env.conversationId != 0) {
                    st.convId = env.conversationId;
                }
                if (env.seq > st.lastSeq) {
                    st.lastSeq = env.seq;
                }
                // seq 从 0 变成真值，需要重新排序
                Collections.sort(st.items, BY_SEQ);
                if (env.seq > 0) {
                    st.seen.put(env.conversationId + ":" + env.seq, local);
                }
            }
        }
        if (st != null) {
            touchConversation(key, null, false);
        }
        for (Listener l : snapshot()) {
            l.onMessageStatusChanged(key, p.clientMsgId, status, "");
        }
    }

    private void onError(Envelope env) {
        String reason = env.content == null ? "" : env.content;

        // 102 并不必然对应一条待确认消息：拉历史失败（bad peer / history failed /
        // 群历史非成员 / bad group id）同样走这个帧。这里先分流，否则一条无关消息会被误标失败，
        // 而在途的 historyInFlight 又永远清不掉——此后 pumpHistory 见它非空就直接返回，
        // 所有翻页请求只排队不发送，直到断线重连才复位。
        boolean historyErr = reason.contains("history") || reason.contains("bad peer")
                || reason.contains("not a group member") || reason.contains("bad group id");
        if (historyErr && historyInFlight != null) {
            historyInFlight = null;
            pumpHistory();
            for (Listener l : snapshot()) {
                l.onTcpError(reason);
            }
            return;
        }

        PendingSend p;
        synchronized (pendingSends) {
            p = pendingSends.pollFirst();
        }
        for (Listener l : snapshot()) {
            l.onTcpError(reason);
        }
        if (p == null) {
            return;
        }

        // 协议 §6.2 第 2 条 / §9.2 第 8 条：server busy 要「降速 + 同一 msg_id 重试」，
        // 而不是直接判失败——服务端按 client_msg_id 去重，重发命中时回 duplicate，是幂等的。
        if (reason.contains("server busy")) {
            int tried = busyRetries.merge(p.clientMsgId, 1, Integer::sum);
            if (tried <= MAX_BUSY_RETRY) {
                long delay = 500L * tried + (long) (Math.random() * 300);
                mainHandler.postDelayed(() -> resendAuto(p.clientMsgId), delay);
                return;
            }
            reason = "服务器繁忙，请稍后重发";
        }
        busyRetries.remove(p.clientMsgId);
        // 先标失败再通知，markFailed 要靠 pendingById 找到本地消息
        markFailed(p.key, p.clientMsgId, reason);
        for (Listener l : snapshot()) {
            l.onMessageStatusChanged(p.key, p.clientMsgId, ChatMessage.Status.FAILED, reason);
        }
    }

    private void dispatchEvent(Envelope env, int type) {
        JsonObject payload;
        try {
            payload = JsonParser.parseString(env.content).getAsJsonObject();
        } catch (JsonSyntaxException | IllegalStateException | ClassCastException e) {
            payload = new JsonObject();
        }
        for (Listener l : snapshot()) {
            switch (type) {
                case 130:
                    l.onFriendRequestEvent(payload);
                    break;
                case 131:
                    l.onFriendRequestResultEvent(payload);
                    break;
                case 132:
                    l.onFriendListChangedEvent(payload);
                    break;
                case 140:
                    l.onGroupMemberChangedEvent(payload);
                    break;
                case 141:
                    l.onGroupDismissedEvent(payload);
                    break;
                case 142:
                    l.onGroupInfoChangedEvent(payload);
                    break;
                case 143:
                    l.onGroupRequestEvent(payload);
                    break;
                case 144:
                    l.onGroupRequestResultEvent(payload);
                    break;
                default:
                    break;
            }
        }
    }

    /**
     * socket 写入失败（异步写线程回报）。
     * 我们上行帧里的 msg_id 就是 clientMsgId，因此能精确定位到那条本地消息。
     */
    public void onSendFailed(Envelope env, String reason) {
        long cmid = env.msgId;
        if (cmid == 0) {
            return;
        }
        PendingSend matched = null;
        for (PendingSend p : pendingSends) {
            if (p.clientMsgId == cmid) {
                matched = p;
                break;
            }
        }
        if (matched == null) {
            return;
        }
        pendingSends.remove(matched);
        String text = reason == null || reason.isEmpty() ? "发送失败" : reason;
        // pendingById 故意保留：用户点重发时要复用同一 clientMsgId（服务端按它去重）
        markFailed(matched.key, cmid, text);
        for (Listener l : snapshot()) {
            l.onMessageStatusChanged(matched.key, cmid, ChatMessage.Status.FAILED, text);
        }
    }

    /** 断线时把所有待确认消息标失败，重发要复用同一 clientMsgId */
    public void onDisconnected(String reason) {
        List<PendingSend> failed;
        synchronized (pendingSends) {
            failed = new ArrayList<>(pendingSends);
            pendingSends.clear();
        }
        historyFifo.clear();
        historyInFlight = null;
        busyRetries.clear();
        mainHandler.removeCallbacksAndMessages(null);
        for (PendingSend p : failed) {
            markFailed(p.key, p.clientMsgId, reason);
            for (Listener l : snapshot()) {
                l.onMessageStatusChanged(p.key, p.clientMsgId, ChatMessage.Status.FAILED, reason);
            }
        }
    }

    // ===================================================================
    //  内部工具
    // ===================================================================

    private PeerState stateFor(long peerId, boolean isGroup) {
        String key = convKey(peerId, isGroup);
        synchronized (peers) {
            PeerState st = peers.get(key);
            if (st == null) {
                st = new PeerState(peerId, isGroup);
                peers.put(key, st);
            }
            return st;
        }
    }

    /** 入库一条消息，返回是否真的新增（false 表示去重命中） */
    private boolean upsert(PeerState st, ChatMessage m) {
        if (m.seq > 0 && m.conversationId != 0) {
            String k = m.conversationId + ":" + m.seq;
            if (st.seen.containsKey(k)) {
                return false;
            }
            st.seen.put(k, m);
        }
        insertSorted(st.items, m);
        return true;
    }

    private static void insertSorted(List<ChatMessage> list, ChatMessage m) {
        int idx = Collections.binarySearch(list, m, BY_SEQ);
        if (idx < 0) {
            list.add(-idx - 1, m);
        } else {
            list.add(idx, m);
        }
    }

    private void markFailed(String key, long clientMsgId, String reason) {
        ChatMessage m = pendingById.get(clientMsgId);
        if (m != null) {
            m.status = ChatMessage.Status.FAILED;
            m.errorText = reason == null ? "" : reason;
        }
    }

    /** 更新会话的最后一条消息、未读数并通知 */
    private void touchConversation(String key, ChatMessage latest, boolean bumpUnread) {
        PeerState st;
        synchronized (peers) {
            st = peers.get(key);
        }
        if (st == null) {
            return;
        }
        ChatMessage last = st.items.isEmpty() ? null : st.items.get(st.items.size() - 1);
        if (last != null) {
            // 本地发送中的消息 seq=0 排在最后，取“有 seq 的最新一条”做摘要更稳定
            ChatMessage best = null;
            for (ChatMessage m : st.items) {
                if (best == null || compareForLast(m, best) > 0) {
                    best = m;
                }
            }
            st.conv.lastMessage = best == null ? last : best;
            st.conv.hasLast = true;
        }
        if (latest != null && bumpUnread) {
            st.conv.unread++;
        }
        for (Listener l : snapshot()) {
            l.onConversationChanged(key, st.conv);
            l.onTotalUnreadChanged(totalUnread());
        }
    }

    private static int compareForLast(ChatMessage a, ChatMessage b) {
        int c = Long.compare(a.ts, b.ts);
        return c != 0 ? c : Long.compare(a.seq, b.seq);
    }

    /** 群解散时清掉该会话，避免继续显示可发送的聊天 */
    public void clearConversation(long peerId, boolean isGroup) {
        String key = convKey(peerId, isGroup);
        synchronized (peers) {
            peers.remove(key);
        }
        for (Listener l : snapshot()) {
            l.onTotalUnreadChanged(totalUnread());
        }
    }

    /** 清空全部会话（退出登录时） */
    public void clearAll() {
        synchronized (peers) {
            peers.clear();
        }
        synchronized (pendingSends) {
            pendingSends.clear();
        }
        pendingById.clear();
        historyFifo.clear();
        historyInFlight = null;
        busyRetries.clear();
        mainHandler.removeCallbacksAndMessages(null);
    }

    /** 供 UI 在通讯录加载后回填会话标题与头像 */
    public void updateConversationMeta(long peerId, boolean isGroup, String title, String avatarUrl) {
        PeerState st = stateFor(peerId, isGroup);
        boolean changed = false;
        if (title != null && !title.equals(st.conv.title)) {
            st.conv.title = title;
            changed = true;
        }
        if (avatarUrl != null && !avatarUrl.equals(st.conv.avatarUrl)) {
            st.conv.avatarUrl = avatarUrl;
            changed = true;
        }
        if (changed) {
            for (Listener l : snapshot()) {
                l.onConversationChanged(st.conv.key(), st.conv);
            }
        }
    }
}
