package com.example.helloworld.ui;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.example.helloworld.R;
import com.example.helloworld.im.model.ChatMessage;
import com.example.helloworld.im.model.Conversation;
import com.example.helloworld.ui.widget.AvatarView;

import java.util.ArrayList;
import java.util.List;

/**
 * 会话列表适配器。行构成对齐桌面端 ConversationListView：
 * 头像 + 标题 + 最后一条消息摘要（图片/文件带类型前缀）+ 时间 + 未读角标（>99 显示 99+）。
 */
public class ConversationAdapter extends RecyclerView.Adapter<ConversationAdapter.VH> {

    public interface OnItemAction {
        void onClick(Conversation c);

        void onLongClick(Conversation c, View anchor);
    }

    private final List<Conversation> data = new ArrayList<>();
    private final OnItemAction action;

    public ConversationAdapter(OnItemAction action) {
        this.action = action;
    }

    public void submit(List<Conversation> list) {
        data.clear();
        if (list != null) {
            data.addAll(list);
        }
        notifyDataSetChanged();
    }

    @NonNull
    @Override
    public VH onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View v = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_conversation, parent, false);
        return new VH(v);
    }

    @Override
    public void onBindViewHolder(@NonNull VH h, int position) {
        Conversation c = data.get(position);
        String title = c.title == null || c.title.isEmpty() ? "会话" : c.title;
        h.tvTitle.setText(title);
        if (c.isGroup) {
            h.avatar.setGroup(c.peerId, title);
            h.groupMark.setVisibility(View.VISIBLE);
        } else {
            h.avatar.setPerson(c.peerId, title);
            h.groupMark.setVisibility(View.GONE);
        }

        ChatMessage last = c.lastMessage;
        if (last != null) {
            String preview = last.preview();
            // 自己发的消息加上「我: 」前缀，和微信/QQ 的摘要口径一致
            if (last.outgoing) {
                preview = "我: " + preview;
            } else if (c.isGroup && last.senderName != null && !last.senderName.isEmpty()) {
                preview = last.senderName + ": " + preview;
            }
            h.tvPreview.setText(preview);
            if (last.status == ChatMessage.Status.FAILED && last.outgoing) {
                h.tvPreview.setTextColor(h.itemView.getContext()
                        .getResources().getColor(R.color.error_red, null));
            } else {
                h.tvPreview.setTextColor(h.itemView.getContext()
                        .getResources().getColor(R.color.text_secondary, null));
            }
            h.tvTime.setText(Ui.listTime(last.ts));
        } else {
            h.tvPreview.setText("");
            h.tvTime.setText("");
        }

        if (c.unread > 0) {
            h.tvBadge.setVisibility(View.VISIBLE);
            h.tvBadge.setText(c.unread > 99 ? "99+" : String.valueOf(c.unread));
        } else {
            h.tvBadge.setVisibility(View.GONE);
        }

        h.itemView.setOnClickListener(v -> action.onClick(c));
        h.itemView.setOnLongClickListener(v -> {
            action.onLongClick(c, v);
            return true;
        });
    }

    @Override
    public int getItemCount() {
        return data.size();
    }

    static class VH extends RecyclerView.ViewHolder {
        final AvatarView avatar;
        final ImageView groupMark;
        final TextView tvTitle;
        final TextView tvPreview;
        final TextView tvTime;
        final TextView tvBadge;

        VH(@NonNull View itemView) {
            super(itemView);
            avatar = itemView.findViewById(R.id.iv_avatar);
            groupMark = itemView.findViewById(R.id.iv_group_mark);
            tvTitle = itemView.findViewById(R.id.tv_title);
            tvPreview = itemView.findViewById(R.id.tv_preview);
            tvTime = itemView.findViewById(R.id.tv_time);
            tvBadge = itemView.findViewById(R.id.tv_badge);
        }
    }
}
