package com.example.helloworld.ui;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.LruCache;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.recyclerview.widget.RecyclerView;

import com.example.helloworld.R;
import com.example.helloworld.im.FileUploader;
import com.example.helloworld.im.ImService;
import com.example.helloworld.im.MsgType;
import com.example.helloworld.im.model.ChatMessage;
import com.example.helloworld.im.model.MediaMeta;
import com.example.helloworld.ui.widget.AvatarView;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * 聊天列表适配器：时间分隔 / 系统提示 / 左右气泡三种形态。
 *
 * <p>图片气泡的取图链路：{@code im.uploader().download(url)} 在
 * {@code im.http().enqueue(...)} 的后台线程里取字节 → BitmapFactory 按 200dp 上限缩放
 * → {@code im.http().mainHandler().post} 回主线程 setImageBitmap；
 * 结果按 fileId 放进进程级 LruCache，滑出屏幕再滑回来不重复下载。
 * ViewHolder 复用会错乱，所以赋值前必须用 tag 校验 fileId 是否还是当前请求。
 */
public class ChatAdapter extends RecyclerView.Adapter<RecyclerView.ViewHolder> {

    /** 行类型 */
    static final int KIND_TIME = 0;
    static final int KIND_SYSTEM = 1;
    static final int KIND_IN = 2;
    static final int KIND_OUT = 3;

    /** 图片气泡最长边（dp） */
    private static final float IMAGE_MAX_DP = 200f;

    public interface Callback {
        /** 长按气泡：弹复制 / 重发 / 删除本地菜单 */
        void onBubbleLongClick(View anchor, ChatMessage msg);

        /** 点击红色感叹号重发 */
        void onResend(ChatMessage msg);

        /** 点击图片或文件气泡：打开 FileViewerActivity */
        void onOpenMedia(ChatMessage msg, MediaMeta meta, boolean isImage);
    }

    // ================= 图片缓存（进程级共享） =================

    private static final int CACHE_KB = (int) (Runtime.getRuntime().maxMemory() / 1024 / 6);

    private static final LruCache<String, Bitmap> IMAGE_CACHE = new LruCache<String, Bitmap>(CACHE_KB) {
        @Override
        protected int sizeOf(String key, Bitmap value) {
            return value.getByteCount() / 1024;
        }
    };

    /** 正在下载中的 fileId，避免同一张图被并发请求多次 */
    private static final Set<String> LOADING = Collections.synchronizedSet(new HashSet<>());

    // ================= 行模型 =================

    private static class Row {
        final int kind;
        @Nullable
        final ChatMessage msg;
        @Nullable
        final String text;

        Row(int kind, @Nullable ChatMessage msg, @Nullable String text) {
            this.kind = kind;
            this.msg = msg;
            this.text = text;
        }
    }

    // ================= 字段 =================

    private final Context context;
    private final ImService im;
    private final Callback callback;
    private final List<Row> rows = new ArrayList<>();

    private final int imageMaxPx;

    private long peerId;
    private boolean isGroup;

    public ChatAdapter(@NonNull Context context, @NonNull ImService im, @NonNull Callback callback) {
        this.context = context;
        this.im = im;
        this.callback = callback;
        this.imageMaxPx = Ui.dp(context, IMAGE_MAX_DP);
    }

    /** 当前会话：群聊时气泡上方要显示发送者名，头像也按发送者取色 */
    public void setConversation(long peerId, boolean isGroup) {
        this.peerId = peerId;
        this.isGroup = isGroup;
    }

    /**
     * 系统提示按时间保存，而不是直接塞进 rows：
     * rows 每次刷新都由消息列表整体重建，直接 add 进去的提示会被下一次 setData 抹掉
     * ——「张三 加入群聊」刚出现，下一条消息到来就不见了。
     * 存成 (ts, text) 后由 setData 按时间归并回相应位置。
     */
    private static class Tip {
        final long ts;
        final String text;

        Tip(long ts, String text) {
            this.ts = ts;
            this.text = text;
        }
    }

    private final List<Tip> tips = new ArrayList<>();
    private List<ChatMessage> lastMessages = new ArrayList<>();

    /** 追加一条系统提示（群成员变更、群解散等） */
    public void appendSystemTip(String text) {
        if (text == null || text.isEmpty()) {
            return;
        }
        tips.add(new Tip(System.currentTimeMillis(), text));
        // 用上一次的消息快照重建，提示才会落在正确的时间位置并且活过下一次刷新
        setData(lastMessages);
    }

    /** 切换会话时清掉上一个会话的提示 */
    public void clearTips() {
        tips.clear();
    }

    /**
     * 用最新消息重建列表。
     *
     * @return 之前第一条消息所在行的下标偏移（加载更早历史时 &gt; 0，供页面补偿滚动位置）
     */
    public int setData(@NonNull List<ChatMessage> messages) {
        ChatMessage anchor = firstMessage();
        int oldIndex = indexOfMessage(anchor);

        lastMessages = new ArrayList<>(messages);
        List<Tip> sorted = new ArrayList<>(tips);
        Collections.sort(sorted, (a, b) -> Long.compare(a.ts, b.ts));

        rows.clear();
        long prevTs = 0;
        int tipIndex = 0;
        for (ChatMessage m : messages) {
            // 提示插在它之后第一条消息前面，视觉上落在正确的时间点
            while (tipIndex < sorted.size() && sorted.get(tipIndex).ts <= m.ts) {
                rows.add(new Row(KIND_SYSTEM, null, sorted.get(tipIndex).text));
                tipIndex++;
            }
            if (Ui.needTimeDivider(prevTs, m.ts)) {
                rows.add(new Row(KIND_TIME, null, Ui.chatTime(m.ts)));
            }
            rows.add(new Row(m.outgoing ? KIND_OUT : KIND_IN, m, null));
            prevTs = m.ts;
        }
        while (tipIndex < sorted.size()) {
            rows.add(new Row(KIND_SYSTEM, null, sorted.get(tipIndex).text));
            tipIndex++;
        }
        notifyDataSetChanged();

        int newIndex = indexOfMessage(anchor);
        if (anchor == null || oldIndex < 0 || newIndex < 0) {
            return 0;
        }
        return newIndex - oldIndex;
    }

    /** 状态变化时只刷某一行，避免整表重绘打断滚动 */
    public void updateByClientMsgId(long clientMsgId) {
        for (int i = 0; i < rows.size(); i++) {
            Row r = rows.get(i);
            if (r.msg != null && r.msg.clientMsgId == clientMsgId && r.msg.clientMsgId != 0) {
                notifyItemChanged(i);
                return;
            }
        }
    }

    public void updateMessage(@Nullable ChatMessage msg) {
        int idx = indexOfMessage(msg);
        if (idx >= 0) {
            notifyItemChanged(idx);
        }
    }

    /** 本地删除：只影响 adapter，服务端与 ChatService 的数据不动 */
    public void removeMessage(@Nullable ChatMessage msg) {
        int idx = indexOfMessage(msg);
        if (idx >= 0) {
            rows.remove(idx);
            notifyItemRemoved(idx);
        }
    }

    @Nullable
    private ChatMessage firstMessage() {
        for (Row r : rows) {
            if (r.msg != null) {
                return r.msg;
            }
        }
        return null;
    }

    private int indexOfMessage(@Nullable ChatMessage msg) {
        if (msg == null) {
            return -1;
        }
        for (int i = 0; i < rows.size(); i++) {
            if (rows.get(i).msg == msg) {
                return i;
            }
        }
        return -1;
    }

    @Override
    public int getItemCount() {
        return rows.size();
    }

    @Override
    public int getItemViewType(int position) {
        return rows.get(position).kind;
    }

    @NonNull
    @Override
    public RecyclerView.ViewHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        LayoutInflater inflater = LayoutInflater.from(context);
        switch (viewType) {
            case KIND_TIME:
                return new TimeHolder(inflater.inflate(R.layout.chat_item_time_divider, parent, false));
            case KIND_SYSTEM:
                return new SystemHolder(inflater.inflate(R.layout.chat_item_system, parent, false));
            case KIND_IN:
                return new MsgHolder(inflater.inflate(R.layout.chat_item_bubble_in, parent, false), false);
            case KIND_OUT:
            default:
                return new MsgHolder(inflater.inflate(R.layout.chat_item_bubble_out, parent, false), true);
        }
    }

    @Override
    public void onBindViewHolder(@NonNull RecyclerView.ViewHolder holder, int position) {
        Row row = rows.get(position);
        if (holder instanceof TimeHolder) {
            ((TimeHolder) holder).bind(row);
        } else if (holder instanceof SystemHolder) {
            ((SystemHolder) holder).bind(row);
        } else if (holder instanceof MsgHolder) {
            ((MsgHolder) holder).bind(row.msg);
        }
    }

    // ================= ViewHolder =================

    static class TimeHolder extends RecyclerView.ViewHolder {
        private final TextView tv;

        TimeHolder(@NonNull View itemView) {
            super(itemView);
            tv = itemView.findViewById(R.id.tv_time);
        }

        void bind(Row row) {
            tv.setText(row.text == null ? "" : row.text);
        }
    }

    static class SystemHolder extends RecyclerView.ViewHolder {
        private final TextView tv;

        SystemHolder(@NonNull View itemView) {
            super(itemView);
            tv = itemView.findViewById(R.id.tv_system);
        }

        void bind(Row row) {
            tv.setText(row.text == null ? "" : row.text);
        }
    }

    /** 左右气泡共用一套控件，outgoing=true 时多一个状态小字与重发按钮 */
    class MsgHolder extends RecyclerView.ViewHolder {
        private final AvatarView avatar;
        private final TextView tvSender;
        private final View bubble;
        private final TextView tvText;
        private final FrameLayout flImage;
        private final ImageView ivImage;
        private final TextView tvMediaHint;
        private final LinearLayout llFile;
        private final TextView tvFileName;
        private final TextView tvFileSize;
        private final TextView tvStatus;
        private final TextView tvFail;
        private final boolean outgoing;

        MsgHolder(@NonNull View itemView, boolean outgoing) {
            super(itemView);
            this.outgoing = outgoing;
            avatar = itemView.findViewById(R.id.avatar);
            tvSender = itemView.findViewById(R.id.tv_sender);
            bubble = itemView.findViewById(R.id.bubble);
            tvText = itemView.findViewById(R.id.tv_text);
            flImage = itemView.findViewById(R.id.fl_image);
            ivImage = itemView.findViewById(R.id.iv_image);
            tvMediaHint = itemView.findViewById(R.id.tv_media_hint);
            llFile = itemView.findViewById(R.id.ll_file);
            tvFileName = itemView.findViewById(R.id.tv_file_name);
            tvFileSize = itemView.findViewById(R.id.tv_file_size);
            tvStatus = itemView.findViewById(R.id.tv_status);
            tvFail = itemView.findViewById(R.id.tv_fail);
            bubble.setClipToOutline(true);
        }

        void bind(@Nullable ChatMessage msg) {
            if (msg == null) {
                return;
            }
            MediaMeta meta = msg.meta();
            boolean isImage = msg.type == MsgType.IMAGE;
            boolean isFile = msg.type == MsgType.FILE;

            // ---- 文本 ----
            if (msg.type == MsgType.TEXT || (!isImage && !isFile)) {
                tvText.setVisibility(View.VISIBLE);
                tvText.setText(msg.type == MsgType.TEXT ? msg.content : msg.preview());
            } else {
                tvText.setVisibility(View.GONE);
            }

            // ---- 文件卡片 ----
            if (isFile) {
                llFile.setVisibility(View.VISIBLE);
                String name = meta == null || meta.name.isEmpty()
                        ? context.getString(R.string.chat_pick_file) : meta.name;
                tvFileName.setText(name);
                tvFileSize.setText(Ui.humanSize(meta == null ? 0 : meta.size));
                bubble.setOnClickListener(v -> openMedia(msg, meta, false));
                llFile.setOnClickListener(v -> openMedia(msg, meta, false));
            } else {
                llFile.setVisibility(View.GONE);
                llFile.setOnClickListener(null);
                bubble.setOnClickListener(null);
            }

            // ---- 图片 ----
            if (isImage) {
                flImage.setVisibility(View.VISIBLE);
                bindImage(msg, meta);
                flImage.setOnClickListener(v -> openMedia(msg, meta, true));
            } else {
                flImage.setVisibility(View.GONE);
                ivImage.setTag(null);
            }

            // ---- 群聊：对方气泡上方显示发送者名 ----
            if (tvSender != null) {
                if (isGroup && !outgoing) {
                    String name = im.contacts().senderName(peerId, msg.fromUid);
                    msg.senderName = name;
                    tvSender.setVisibility(View.VISIBLE);
                    tvSender.setText(name);
                } else {
                    tvSender.setVisibility(View.GONE);
                }
            }

            // ---- 头像 ----
            if (avatar != null) {
                if (isGroup) {
                    avatar.setPerson(msg.fromUid, msg.senderName);
                } else {
                    avatar.setPerson(peerId, im.contacts().friendName(peerId));
                }
            }

            // ---- 自己消息的状态小字 ----
            if (tvStatus != null) {
                tvStatus.setVisibility(outgoing ? View.VISIBLE : View.GONE);
                if (outgoing) {
                    tvStatus.setText(statusLabel(msg));
                    tvStatus.setTextColor(context.getColor(msg.status == ChatMessage.Status.FAILED
                            ? R.color.error_red : R.color.text_secondary));
                }
            }
            if (tvFail != null) {
                boolean failed = outgoing && msg.status == ChatMessage.Status.FAILED;
                tvFail.setVisibility(failed ? View.VISIBLE : View.GONE);
                tvFail.setContentDescription(msg.errorText == null || msg.errorText.isEmpty()
                        ? context.getString(R.string.chat_resend) : msg.errorText);
                tvFail.setOnClickListener(v -> callback.onResend(msg));
            }

            // ---- 长按菜单 ----
            bubble.setOnLongClickListener(v -> {
                callback.onBubbleLongClick(v, msg);
                return true;
            });
            itemView.setOnLongClickListener(v -> {
                callback.onBubbleLongClick(bubble == null ? v : bubble, msg);
                return true;
            });
        }

        private String statusLabel(ChatMessage msg) {
            switch (msg.status) {
                case SENDING:
                    return context.getString(R.string.chat_status_sending);
                case FAILED:
                    return context.getString(R.string.chat_status_failed);
                case DUPLICATE:
                case SENT:
                default:
                    return context.getString(R.string.chat_status_sent);
            }
        }

        private void openMedia(@NonNull ChatMessage msg, @Nullable MediaMeta meta, boolean asImage) {
            if (meta == null || meta.url.isEmpty()) {
                Ui.toast(context, context.getString(
                        asImage ? R.string.chat_image_no_url : R.string.chat_file_no_url));
                return;
            }
            callback.onOpenMedia(msg, meta, asImage || FileUploader.isImage(meta.mime));
        }

        // ---------- 图片加载 ----------

        private void bindImage(@NonNull ChatMessage msg, @Nullable MediaMeta meta) {
            if (meta == null || meta.fileId.isEmpty() || meta.url.isEmpty()) {
                // 服务端还没把权威元数据（含 url）重建回来，先占位
                showImagePlaceholder(context.getString(R.string.chat_image_loading), false);
                ivImage.setTag(null);
                return;
            }
            final String fileId = meta.fileId;
            final String url = meta.url;
            // tag 先立起来：复用到别的消息时旧的异步回调会被 tag 校验挡掉
            ivImage.setTag(fileId);

            Bitmap cached = IMAGE_CACHE.get(fileId);
            if (cached != null && !cached.isRecycled()) {
                applyImage(cached);
                return;
            }
            showImagePlaceholder(context.getString(R.string.chat_image_loading), false);

            if (!LOADING.add(fileId)) {
                // 已有同名请求在途，这里只保持占位，完成后的回调会刷新可见行
                return;
            }
            im.http().enqueue(() -> {
                Bitmap bmp = null;
                try {
                    byte[] bytes = im.uploader().download(url);
                    bmp = decodeScaled(bytes, imageMaxPx);
                    if (bmp != null) {
                        IMAGE_CACHE.put(fileId, bmp);
                    }
                } catch (IOException | RuntimeException | OutOfMemoryError e) {
                    bmp = null;
                } finally {
                    LOADING.remove(fileId);
                }
                final Bitmap result = bmp;
                im.http().mainHandler().post(() -> {
                    if (result == null || result.isRecycled()) {
                        if (fileId.equals(ivImage.getTag())) {
                            showImagePlaceholder(
                                    context.getString(R.string.chat_image_broken,
                                            placeholderName(meta)), true);
                        }
                        return;
                    }
                    // 复用错乱保护：只有 tag 仍是本次 fileId 时才赋值
                    if (fileId.equals(ivImage.getTag())) {
                        applyImage(result);
                    }
                    // 整表重绑一次：缓存里已经有这张图了，
                    // 否则「下载期间被历史加载/旋转触发了重绑」或「同一张图出现在多处」的行
                    // 会永远停在「加载中」——因为只有发起请求的这个 holder 拿到了结果
                    notifyDataSetChanged();
                });
            });
        }

        private String placeholderName(@Nullable MediaMeta meta) {
            return meta == null || meta.name.isEmpty()
                    ? context.getString(R.string.chat_pick_image) : meta.name;
        }

        private void applyImage(@NonNull Bitmap bmp) {
            ViewGroup.LayoutParams lp = ivImage.getLayoutParams();
            lp.width = bmp.getWidth();
            lp.height = bmp.getHeight();
            ivImage.setLayoutParams(lp);
            ivImage.setImageBitmap(bmp);
            ivImage.setVisibility(View.VISIBLE);
            tvMediaHint.setVisibility(View.GONE);
        }

        private void showImagePlaceholder(@Nullable String text, boolean broken) {
            ivImage.setVisibility(View.INVISIBLE);
            ivImage.setImageDrawable(null);
            tvMediaHint.setVisibility(View.VISIBLE);
            tvMediaHint.setText(text == null ? "" : text);
            ViewGroup.LayoutParams lp = tvMediaHint.getLayoutParams();
            lp.width = ViewGroup.LayoutParams.WRAP_CONTENT;
            lp.height = ViewGroup.LayoutParams.WRAP_CONTENT;
            tvMediaHint.setLayoutParams(lp);
            tvMediaHint.setTextColor(context.getColor(broken ? R.color.text_primary
                    : R.color.text_secondary));
        }
    }

    /** 先按 inJustDecodeBounds 估 inSampleSize，必要时再精确缩到上限，避免大图 OOM */
    @Nullable
    private static Bitmap decodeScaled(@NonNull byte[] bytes, int maxPx) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            return null;
        }
        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inPreferredConfig = Bitmap.Config.ARGB_8888;
        int sample = 1;
        while (bounds.outWidth / sample > maxPx * 2 || bounds.outHeight / sample > maxPx * 2) {
            sample *= 2;
        }
        opts.inSampleSize = sample;
        Bitmap bmp;
        try {
            bmp = BitmapFactory.decodeByteArray(bytes, 0, bytes.length, opts);
        } catch (OutOfMemoryError e) {
            return null;
        }
        if (bmp == null) {
            return null;
        }
        int w = bmp.getWidth();
        int h = bmp.getHeight();
        int longest = Math.max(w, h);
        if (longest <= maxPx || longest == 0) {
            return bmp;
        }
        float ratio = (float) maxPx / longest;
        int nw = Math.max(1, Math.round(w * ratio));
        int nh = Math.max(1, Math.round(h * ratio));
        Bitmap scaled = Bitmap.createScaledBitmap(bmp, nw, nh, true);
        if (scaled != bmp) {
            bmp.recycle();
        }
        return scaled;
    }
}
