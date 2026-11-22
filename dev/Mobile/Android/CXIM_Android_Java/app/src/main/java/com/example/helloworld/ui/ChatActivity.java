package com.example.helloworld.ui;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.Editable;
import android.text.TextUtils;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.widget.PopupMenu;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.example.helloworld.R;
import com.example.helloworld.im.ChatService;
import com.example.helloworld.im.FileUploader;
import com.example.helloworld.im.MsgType;
import com.example.helloworld.im.TcpClient;
import com.example.helloworld.im.model.ChatMessage;
import com.example.helloworld.im.model.GroupInfo;
import com.example.helloworld.im.model.MediaMeta;
import com.google.gson.JsonObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * 聊天页：文本 / 图片 / 文件收发、历史分页、连接状态提示、失败重发。
 *
 * <p>数据来源只有 {@link ChatService} 一处：每次事件都从 {@code chat().messages(convKey)}
 * 重新取（返回的是同一批 ChatMessage 实例，状态在服务层原地更新），
 * 页面自己只额外维护「本机已删除」黑名单与上传进度区。
 */
public class ChatActivity extends BaseActivity implements ChatAdapter.Callback {

    public static final String EXTRA_PEER_ID = "peer_id";
    public static final String EXTRA_IS_GROUP = "is_group";
    public static final String EXTRA_TITLE = "title";

    /** 距底部小于该值算「在底部附近」，新消息直接滚到底 */
    private static final int NEAR_BOTTOM_DP = 200;
    /** 滚到第几行触发加载更早历史 */
    private static final int LOAD_MORE_THRESHOLD = 2;

    private long peerId;
    private boolean isGroup;
    private String convKey = "";
    private String title = "";

    private RecyclerView rvMessages;
    private LinearLayoutManager layoutManager;
    private ChatAdapter adapter;
    private EditText etInput;
    private ImageButton btnSend;
    private TextView tvConnBar;
    private TextView tvAnnounce;
    private TextView tvHistoryTip;
    private TextView btnNewMsg;
    private LinearLayout llUpload;
    private ProgressBar pbUpload;
    private TextView tvUpload;

    /** 本机删除（仅从 adapter 移除）的消息，重建列表时跳过 */
    private final Set<ChatMessage> hidden = new HashSet<>();

    private boolean loadingHistory;
    private boolean historyRequested;
    private boolean groupDetailRequested;
    private boolean uploading;
    private int pendingNew;
    private boolean announceExpanded;

    private ActivityResultLauncher<String[]> pickImageLauncher;
    private ActivityResultLauncher<String[]> pickFileLauncher;

    // ================================================================
    //  生命周期
    // ================================================================

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.chat_activity_chat);

        Intent intent = getIntent();
        peerId = intent != null ? intent.getLongExtra(EXTRA_PEER_ID, 0L) : 0L;
        isGroup = intent != null && intent.getBooleanExtra(EXTRA_IS_GROUP, false);
        String extraTitle = intent == null ? null : intent.getStringExtra(EXTRA_TITLE);
        title = extraTitle == null ? "" : extraTitle;
        if (title.isEmpty()) {
            title = im.contacts().titleOf(peerId, isGroup);
        }
        // peer_id 为 0 说明调用方没带会话参数（例如旧版通知的 Intent）。
        // 这种会话既不能收发也不能拉历史（服务端会回 bad peer / bad group id 并可能断连），
        // 直接退回主界面，不构造这个假会话。
        if (peerId == 0) {
            finish();
            startActivity(new Intent(this, MainActivity.class));
            return;
        }

        convKey = ChatService.convKey(peerId, isGroup);

        setHeaderTitle(title);
        if (isGroup) {
            setHeaderIcon(R.drawable.ic_more, getString(R.string.chat_group_detail_title),
                    v -> openGroupDetail());
        }

        tvConnBar = findViewById(R.id.tv_conn_bar);
        tvAnnounce = findViewById(R.id.tv_announce);
        tvHistoryTip = findViewById(R.id.tv_history_tip);
        btnNewMsg = findViewById(R.id.btn_new_msg);
        llUpload = findViewById(R.id.ll_upload);
        pbUpload = findViewById(R.id.pb_upload);
        tvUpload = findViewById(R.id.tv_upload);
        etInput = findViewById(R.id.et_input);
        btnSend = findViewById(R.id.btn_send);
        ImageButton btnAdd = findViewById(R.id.btn_add);
        rvMessages = findViewById(R.id.rv_messages);

        adapter = new ChatAdapter(this, im, this);
        adapter.setConversation(peerId, isGroup);
        layoutManager = new LinearLayoutManager(this);
        rvMessages.setLayoutManager(layoutManager);
        rvMessages.setAdapter(adapter);
        rvMessages.addOnScrollListener(new RecyclerView.OnScrollListener() {
            @Override
            public void onScrolled(@NonNull RecyclerView recyclerView, int dx, int dy) {
                if (dy < 0 && layoutManager.findFirstVisibleItemPosition() <= LOAD_MORE_THRESHOLD) {
                    loadMoreHistory();
                }
                if (pendingNew > 0 && isNearBottom()) {
                    clearNewMsgBadge();
                }
            }
        });

        btnNewMsg.setOnClickListener(v -> {
            clearNewMsgBadge();
            scrollToBottom(true);
        });
        tvAnnounce.setOnClickListener(v -> toggleAnnounce());

        btnAdd.setOnClickListener(this::showAttachMenu);
        btnSend.setOnClickListener(v -> sendCurrentText());
        updateSendEnabled();

        etInput.addTextChangedListener(new TextWatcher() {
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
        etInput.setOnEditorActionListener((v, actionId, event) -> {
            // 软键盘的发送键 = 发送；多行输入框里的回车仍然是换行
            if (actionId == EditorInfo.IME_ACTION_SEND) {
                sendCurrentText();
                return true;
            }
            // 实体键盘没有「发送」动作，回车直接发送，Shift+回车换行
            if (event != null && isPhysicalEnter(event)) {
                sendCurrentText();
                return true;
            }
            return false;
        });

        pickImageLauncher = registerForActivityResult(
                new ActivityResultContracts.OpenDocument(), this::onPicked);
        pickFileLauncher = registerForActivityResult(
                new ActivityResultContracts.OpenDocument(), this::onPicked);
    }

    private boolean isPhysicalEnter(KeyEvent event) {
        return event.getKeyCode() == KeyEvent.KEYCODE_ENTER
                && event.getAction() == KeyEvent.ACTION_DOWN
                && event.getDeviceId() != -1
                && (event.getFlags() & KeyEvent.FLAG_VIRTUAL_HARD_KEY) == 0
                && (event.getMetaState() & KeyEvent.META_SHIFT_ON) == 0;
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (im == null) {
            return;
        }
        im.chat().addListener(chatListener);
        im.chat().setActiveConversation(convKey);
        refresh();
        updateConnBar(im.tcp().state());
        updateAnnounce();
        loadGroupDetailOnce();
        requestHistoryOnce();
        im.chat().markRead(peerId, isGroup);
    }

    @Override
    protected void onPause() {
        if (im != null) {
            im.chat().setActiveConversation("");
            im.chat().removeListener(chatListener);
        }
        super.onPause();
    }

    // ================================================================
    //  ChatService 事件：只处理当前会话
    // ================================================================

    private final ChatService.Listener chatListener = new ChatService.SimpleListener() {

        @Override
        public void onTcpLoggedIn() {
            // 首次进入时 TCP 可能还没登录，requestHistory 会被服务层吞掉，这里补一次
            resetHistoryRequest();
        }

        @Override
        public void onTcpError(String reason) {
            if (reason != null && !reason.isEmpty()) {
                toast(reason);
            }
        }

        @Override
        public void onConnectionStateChanged(int state) {
            if (state == TcpClient.STATE_LOGGED_IN) {
                loadingHistory = false;
                resetHistoryRequest();
            } else {
                // 断线时服务层会清空历史请求队列，本地别把「加载中」一直挂着
                loadingHistory = false;
                historyRequested = false;
            }
            updateConnBar(state);
        }

        @Override
        public void onMessageIncoming(String key, ChatMessage message) {
            if (!convKey.equals(key)) {
                return;
            }
            boolean atBottom = isNearBottom();
            refresh();
            if (atBottom) {
                scrollToBottom(false);
            } else {
                pendingNew++;
                btnNewMsg.setVisibility(View.VISIBLE);
                btnNewMsg.setText(getString(R.string.chat_new_messages, pendingNew));
            }
            im.chat().markRead(peerId, isGroup);
        }

        @Override
        public void onMessageLocalAdded(String key, ChatMessage message) {
            if (!convKey.equals(key)) {
                return;
            }
            refresh();
            scrollToBottom(false);
        }

        @Override
        public void onMessageStatusChanged(String key, long clientMsgId,
                                           ChatMessage.Status status, String errorText) {
            if (!convKey.equals(key)) {
                return;
            }
            adapter.updateByClientMsgId(clientMsgId);
        }

        @Override
        public void onHistoryLoaded(String key, List<ChatMessage> items, long oldestSeq,
                                    boolean reachedEnd) {
            if (!convKey.equals(key)) {
                return;
            }
            loadingHistory = false;
            int oldFirst = layoutManager.findFirstVisibleItemPosition();
            View oldTop = oldFirst < 0 ? null : layoutManager.findViewByPosition(oldFirst);
            final int oldOffset = oldTop == null ? 0 : oldTop.getTop();
            boolean wasEmpty = adapter.getItemCount() == 0;

            int shift = refresh();
            if (wasEmpty) {
                scrollToBottom(false);
            } else if (shift > 0 && oldFirst >= 0) {
                // 顶部插入了更早的消息：补偿滚动，保持原来看到的那条不动
                rvMessages.post(
                        () -> layoutManager.scrollToPositionWithOffset(oldFirst + shift, oldOffset));
            }
            updateHistoryTip();
        }

        @Override
        public void onGroupMemberChangedEvent(JsonObject payload) {
            if (!isGroup || peerId != optLong(payload, "group_id")) {
                return;
            }
            adapter.appendSystemTip(memberChangeText(payload));
            // 成员名可能刚变，重拉一次群详情刷新气泡上的昵称
            groupDetailRequested = false;
            loadGroupDetailOnce();
        }

        @Override
        public void onGroupInfoChangedEvent(JsonObject payload) {
            if (!isGroup || peerId != optLong(payload, "group_id")) {
                return;
            }
            groupDetailRequested = false;
            loadGroupDetailOnce();
        }

        @Override
        public void onGroupDismissedEvent(JsonObject payload) {
            if (!isGroup || peerId != optLong(payload, "group_id")) {
                return;
            }
            adapter.appendSystemTip(getString(R.string.chat_sys_group_dismissed));
            toast(getString(R.string.chat_sys_group_dismissed));
        }
    };

    private void resetHistoryRequest() {
        historyRequested = false;
        requestHistoryOnce();
    }

    private static long optLong(JsonObject o, String key) {
        try {
            return o != null && o.has(key) && !o.get(key).isJsonNull()
                    ? o.get(key).getAsLong() : 0L;
        } catch (RuntimeException e) {
            return 0L;
        }
    }

    private static String optString(JsonObject o, String key) {
        try {
            return o != null && o.has(key) && !o.get(key).isJsonNull()
                    ? o.get(key).getAsString() : "";
        } catch (RuntimeException e) {
            return "";
        }
    }

    private String memberChangeText(JsonObject payload) {
        String action = optString(payload, "action");
        long uid = optLong(payload, "user_id");
        String name = im.contacts().senderName(peerId, uid);
        if ("removed".equals(action) || "quit".equals(action)) {
            return getString(R.string.chat_sys_member_removed, name);
        }
        if ("added".equals(action)) {
            return getString(R.string.chat_sys_member_added, name);
        }
        return getString(R.string.chat_sys_member_changed);
    }

    // ================================================================
    //  列表刷新与滚动
    // ================================================================

    /** 重新从服务层取消息，返回顶部插入造成的行偏移 */
    private int refresh() {
        List<ChatMessage> msgs = new ArrayList<>();
        for (ChatMessage m : im.chat().messages(convKey)) {
            if (m != null && !hidden.contains(m)) {
                msgs.add(m);
            }
        }
        int shift = adapter.setData(msgs);
        updateHistoryTip();
        return shift;
    }

    private boolean isNearBottom() {
        int last = adapter.getItemCount() - 1;
        if (last < 0) {
            return true;
        }
        if (layoutManager.findLastVisibleItemPosition() < last) {
            return false;
        }
        View bottom = layoutManager.findViewByPosition(last);
        if (bottom == null) {
            return false;
        }
        int gap = rvMessages.getHeight() - rvMessages.getPaddingBottom() - bottom.getBottom();
        return gap <= Ui.dp(this, NEAR_BOTTOM_DP);
    }

    private void scrollToBottom(boolean smooth) {
        int count = adapter.getItemCount();
        if (count <= 0) {
            return;
        }
        if (smooth) {
            rvMessages.smoothScrollToPosition(count - 1);
        } else {
            rvMessages.scrollToPosition(count - 1);
            rvMessages.post(() -> {
                View last = layoutManager.findViewByPosition(count - 1);
                if (last == null) {
                    return;
                }
                int delta = last.getBottom()
                        - (rvMessages.getHeight() - rvMessages.getPaddingBottom());
                if (delta > 0) {
                    rvMessages.scrollBy(0, delta);
                }
            });
        }
        clearNewMsgBadge();
    }

    private void clearNewMsgBadge() {
        pendingNew = 0;
        btnNewMsg.setVisibility(View.GONE);
    }

    private void updateHistoryTip() {
        if (loadingHistory) {
            tvHistoryTip.setVisibility(View.VISIBLE);
            tvHistoryTip.setText(R.string.chat_history_loading);
            return;
        }
        boolean hasMessages = adapter.getItemCount() > 0;
        if (hasMessages && im.chat().reachedEnd(convKey)) {
            tvHistoryTip.setVisibility(View.VISIBLE);
            tvHistoryTip.setText(R.string.chat_history_end);
            return;
        }
        if (!hasMessages && historyRequested && im.chat().historyLoadedOnce(convKey)) {
            tvHistoryTip.setVisibility(View.VISIBLE);
            tvHistoryTip.setText(R.string.chat_empty_hint);
            return;
        }
        tvHistoryTip.setVisibility(View.GONE);
    }

    // ================================================================
    //  历史分页
    // ================================================================

    private void requestHistoryOnce() {
        if (historyRequested) {
            return;
        }
        if (im.chat().historyLoadedOnce(convKey)) {
            historyRequested = true;
            updateHistoryTip();
            return;
        }
        if (!im.chat().isReady()) {
            updateHistoryTip();
            return;
        }
        historyRequested = true;
        loadingHistory = true;
        im.chat().requestHistory(peerId, isGroup, 0L, im.chat().historyPageSize());
        updateHistoryTip();
    }

    /** 滚到顶部自动加载更早：beforeSeq = 当前最旧的 seq（本地发送中的 seq=0 要跳过） */
    private void loadMoreHistory() {
        if (loadingHistory || uploading) {
            return;
        }
        if (!im.chat().historyLoadedOnce(convKey) || im.chat().reachedEnd(convKey)
                || !im.chat().isReady()) {
            return;
        }
        long beforeSeq = 0L;
        for (ChatMessage m : im.chat().messages(convKey)) {
            if (m.seq > 0 && (beforeSeq == 0L || m.seq < beforeSeq)) {
                beforeSeq = m.seq;
            }
        }
        if (beforeSeq <= 0L) {
            return;
        }
        loadingHistory = true;
        im.chat().requestHistory(peerId, isGroup, beforeSeq, im.chat().historyPageSize());
        updateHistoryTip();
    }

    // ================================================================
    //  顶栏 / 公告 / 连接状态
    // ================================================================

    private void updateConnBar(int state) {
        if (tvConnBar == null) {
            return;
        }
        if (state == TcpClient.STATE_LOGGED_IN) {
            tvConnBar.setVisibility(View.GONE);
            tvConnBar.setOnClickListener(null);
            return;
        }
        int textRes;
        switch (state) {
            case TcpClient.STATE_CONNECTING:
                textRes = R.string.chat_conn_connecting;
                break;
            case TcpClient.STATE_CONNECTED:
                textRes = R.string.chat_conn_connected;
                break;
            case TcpClient.STATE_FATAL:
                textRes = R.string.chat_conn_fatal;
                break;
            case TcpClient.STATE_DISCONNECTED:
            default:
                textRes = R.string.chat_conn_disconnected;
                break;
        }
        tvConnBar.setVisibility(View.VISIBLE);
        tvConnBar.setText(textRes);
        tvConnBar.setOnClickListener(state == TcpClient.STATE_FATAL ? v -> ensureConnected() : null);
    }

    private void toggleAnnounce() {
        announceExpanded = !announceExpanded;
        tvAnnounce.setMaxLines(announceExpanded ? Integer.MAX_VALUE : 1);
        tvAnnounce.setEllipsize(announceExpanded ? null : TextUtils.TruncateAt.END);
    }

    private void updateAnnounce() {
        GroupInfo g = isGroup ? im.contacts().group(peerId) : null;
        if (g == null || g.announcement == null || g.announcement.isEmpty()) {
            tvAnnounce.setVisibility(View.GONE);
            return;
        }
        announceExpanded = false;
        tvAnnounce.setMaxLines(1);
        tvAnnounce.setEllipsize(TextUtils.TruncateAt.END);
        tvAnnounce.setText(getString(R.string.chat_announce, g.announcement));
        tvAnnounce.setVisibility(View.VISIBLE);
    }

    /** 群聊需要成员列表才能显示发送者名，顺带把群公告/群名拉齐 */
    private void loadGroupDetailOnce() {
        if (!isGroup || groupDetailRequested) {
            return;
        }
        groupDetailRequested = true;
        im.groupDetail(peerId, (ok, error, group, members) -> {
            if (!ok) {
                groupDetailRequested = false;
                return;
            }
            if (group != null && !group.displayName().isEmpty()) {
                title = group.displayName();
                setHeaderTitle(title);
            }
            updateAnnounce();
            // 成员名到位后刷新气泡昵称
            adapter.notifyDataSetChanged();
        });
    }

    /** 跳群设置页（GroupDetailActivity 由通讯录模块提供，extra 为 group_id） */
    private void openGroupDetail() {
        try {
            Intent i = new Intent(this, GroupDetailActivity.class);
            i.putExtra(GroupDetailActivity.EXTRA_GROUP_ID, peerId);
            startActivity(i);
        } catch (RuntimeException e) {
            toast(getString(R.string.chat_group_detail_missing));
        }
    }

    // ================================================================
    //  发送文本
    // ================================================================

    private void updateSendEnabled() {
        if (btnSend == null) {
            return;
        }
        boolean hasText = etInput.getText() != null
                && etInput.getText().toString().trim().length() > 0;
        btnSend.setEnabled(hasText);
        btnSend.setAlpha(hasText ? 1f : 0.35f);
    }

    private void sendCurrentText() {
        String text = etInput.getText() == null ? "" : etInput.getText().toString();
        if (text.trim().isEmpty()) {
            return;
        }
        long cmid = im.chat().sendText(peerId, isGroup, text);
        if (cmid == 0) {
            return;
        }
        etInput.setText("");
        etInput.setSelection(0);
        updateSendEnabled();
        if (!im.chat().isReady()) {
            toast(getString(R.string.chat_not_connected));
        }
    }

    // ================================================================
    //  发图片 / 文件
    // ================================================================

    private void showAttachMenu(View anchor) {
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenu().add(0, 1, 0, R.string.chat_pick_image);
        menu.getMenu().add(0, 2, 1, R.string.chat_pick_file);
        menu.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == 1) {
                pickImageLauncher.launch(new String[]{"image/*"});
            } else {
                pickFileLauncher.launch(new String[]{"*/*"});
            }
            return true;
        });
        menu.show();
    }

    private void onPicked(@Nullable Uri uri) {
        if (uri == null) {
            return;
        }
        if (uploading) {
            toast(getString(R.string.chat_upload_wait));
            return;
        }
        uploading = true;
        showUpload(0);
        // content:// 未必能直接读，先复制到 cacheDir 拿到真实 File 与显示名
        im.http().enqueue(() -> {
            final File file;
            final String name;
            try {
                name = queryDisplayName(uri);
                file = copyToCache(uri, name);
            } catch (IOException | RuntimeException e) {
                final String err = e.getMessage() == null
                        ? getString(R.string.chat_read_failed) : e.getMessage();
                im.http().mainHandler().post(() -> {
                    uploading = false;
                    hideUpload();
                    toast(err);
                });
                return;
            }
            im.http().mainHandler().post(() -> startUpload(file, name));
        });
    }

    private File copyToCache(Uri uri, String displayName) throws IOException {
        File dir = new File(getCacheDir(), "outbox");
        if (!dir.exists() && !dir.mkdirs()) {
            throw new IOException(getString(R.string.chat_read_failed));
        }
        File dest = new File(dir, System.currentTimeMillis() + "_" + sanitizeName(displayName));
        try (InputStream in = getContentResolver().openInputStream(uri);
             OutputStream out = new FileOutputStream(dest)) {
            if (in == null) {
                throw new IOException(getString(R.string.chat_read_failed));
            }
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            out.flush();
        }
        return dest;
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri,
                new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0 && !c.isNull(idx)) {
                    String name = c.getString(idx);
                    if (name != null && !name.isEmpty()) {
                        return name;
                    }
                }
            }
        } catch (RuntimeException ignore) {
            // 少数文档提供器不支持 query，回退到 uri 末段
        }
        String last = uri.getLastPathSegment();
        return last == null || last.isEmpty() ? "image.png" : last;
    }

    private static String sanitizeName(String name) {
        if (name == null || name.trim().isEmpty()) {
            return "file";
        }
        return name.trim().replaceAll("[\\\\/:*?\"<>|\\r\\n]", "_");
    }

    /** 扩展名白名单由 FileUploader 本地校验，失败信息直接从 onDone 的 error 里展示 */
    private void startUpload(File file, String name) {
        boolean isImage = FileUploader.isImage(name);
        int type = isImage ? MsgType.IMAGE : MsgType.FILE;
        String mime = FileUploader.guessMime(name);
        im.uploader().upload(file, mime, name, new FileUploader.UploadListener() {
            @Override
            public void onProgress(long sent, long total) {
                int pct = total <= 0 ? 0 : (int) Math.min(99L, sent * 100L / total);
                showUpload(pct);
            }

            @Override
            public void onDone(boolean ok, String error, MediaMeta meta) {
                uploading = false;
                hideUpload();
                file.delete();
                if (!ok) {
                    toast(error == null || error.isEmpty()
                            ? getString(R.string.chat_status_failed) : error);
                    return;
                }
                long cmid = im.chat().sendMedia(peerId, isGroup, type, meta);
                if (cmid == 0) {
                    toast(getString(R.string.chat_status_failed));
                } else if (!im.chat().isReady()) {
                    toast(getString(R.string.chat_not_connected));
                }
            }
        });
    }

    private void showUpload(int pct) {
        llUpload.setVisibility(View.VISIBLE);
        pbUpload.setProgress(pct);
        tvUpload.setText(getString(R.string.chat_uploading, pct));
    }

    private void hideUpload() {
        llUpload.setVisibility(View.GONE);
        pbUpload.setProgress(0);
    }

    // ================================================================
    //  ChatAdapter.Callback
    // ================================================================

    @Override
    public void onBubbleLongClick(View anchor, ChatMessage msg) {
        if (msg == null) {
            return;
        }
        List<String> items = new ArrayList<>();
        List<Runnable> actions = new ArrayList<>();

        if (msg.type == MsgType.TEXT && msg.content != null && !msg.content.isEmpty()) {
            items.add(getString(R.string.chat_menu_copy));
            actions.add(() -> copyText(msg.content));
        }
        if (msg.outgoing && msg.status == ChatMessage.Status.FAILED) {
            items.add(getString(R.string.chat_menu_resend));
            actions.add(() -> im.chat().resend(msg.clientMsgId));
        }
        items.add(getString(R.string.chat_menu_delete));
        actions.add(() -> {
            hidden.add(msg);
            adapter.removeMessage(msg);
            updateHistoryTip();
            toast(getString(R.string.chat_deleted_local));
        });

        Runnable[] actionArr = actions.toArray(new Runnable[0]);
        Ui.choices(this, getString(R.string.chat_menu_title),
                items.toArray(new String[0]), (index, text) -> {
                    if (index >= 0 && index < actionArr.length) {
                        actionArr[index].run();
                    }
                });
    }

    private void copyText(String text) {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null) {
            return;
        }
        cm.setPrimaryClip(ClipData.newPlainText("cxim", text));
        toast(getString(R.string.chat_copied));
    }

    @Override
    public void onResend(ChatMessage msg) {
        if (msg == null) {
            return;
        }
        if (!im.chat().isReady()) {
            toast(getString(R.string.chat_conn_disconnected));
        }
        im.chat().resend(msg.clientMsgId);
    }

    @Override
    public void onOpenMedia(ChatMessage msg, MediaMeta meta, boolean isImage) {
        if (meta == null || meta.url.isEmpty()) {
            toast(getString(isImage ? R.string.chat_image_no_url : R.string.chat_file_no_url));
            return;
        }
        Intent i = new Intent(this, FileViewerActivity.class);
        i.putExtra(FileViewerActivity.EXTRA_URL, meta.url);
        i.putExtra(FileViewerActivity.EXTRA_NAME, meta.name);
        i.putExtra(FileViewerActivity.EXTRA_SIZE, meta.size);
        i.putExtra(FileViewerActivity.EXTRA_IS_IMAGE, isImage);
        startActivity(i);
    }
}
