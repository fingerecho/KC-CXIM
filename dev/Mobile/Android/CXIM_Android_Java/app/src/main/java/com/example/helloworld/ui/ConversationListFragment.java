package com.example.helloworld.ui;

import android.content.Intent;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import com.example.helloworld.CXIMApp;
import com.example.helloworld.R;
import com.example.helloworld.im.ChatService;
import com.example.helloworld.im.Contacts;
import com.example.helloworld.im.ImService;
import com.example.helloworld.im.model.Conversation;

import java.util.List;

/**
 * 消息 tab：会话列表。
 *
 * <p>会话不是服务端接口，而是本地聚合（ChatService.conversations()），
 * 按最后一条消息时间降序；标题/头像来自通讯录缓存，所以通讯录刷新后要重绘。
 */
public class ConversationListFragment extends Fragment {

    private ImService im;
    private RecyclerView rv;
    private SwipeRefreshLayout swipe;
    private View emptyView;
    private ConversationAdapter adapter;

    private final ChatService.SimpleListener chatListener = new ChatService.SimpleListener() {
        @Override
        public void onConversationChanged(String convKey, Conversation conversation) {
            refresh();
        }

        @Override
        public void onMessageIncoming(String convKey, com.example.helloworld.im.model.ChatMessage m) {
            refresh();
        }

        @Override
        public void onMessageLocalAdded(String convKey, com.example.helloworld.im.model.ChatMessage m) {
            refresh();
        }
    };

    private final Contacts.SimpleListener contactsListener = new Contacts.SimpleListener() {
        @Override
        public void onFriendsChanged() {
            refresh();
        }

        @Override
        public void onGroupsChanged() {
            refresh();
        }
    };

    @Nullable
    @Override
    public View onCreateView(@NonNull LayoutInflater inflater, @Nullable ViewGroup container,
                             @Nullable Bundle savedInstanceState) {
        return inflater.inflate(R.layout.fragment_conversation_list, container, false);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);
        im = CXIMApp.service();

        rv = view.findViewById(R.id.rv_conversations);
        swipe = view.findViewById(R.id.swipe);
        emptyView = view.findViewById(R.id.empty_view);

        ImageView icon = emptyView.findViewById(R.id.empty_icon);
        icon.setImageResource(R.drawable.ic_chat);
        TextView text = emptyView.findViewById(R.id.empty_text);
        text.setText(R.string.empty_conversation);

        adapter = new ConversationAdapter(new ConversationAdapter.OnItemAction() {
            @Override
            public void onClick(Conversation c) {
                openConversation(c);
            }

            @Override
            public void onLongClick(Conversation c, View anchor) {
                showRowMenu(c);
            }
        });
        rv.setLayoutManager(new LinearLayoutManager(requireContext()));
        rv.setAdapter(adapter);

        swipe.setColorSchemeResources(R.color.brand_accent);
        swipe.setOnRefreshListener(() -> {
            if (im != null) {
                // 会话是本地聚合，下拉刷新真正要做的是对齐通讯录；
                // 只有长连接确实断了才重建，避免每次下拉都把在线连接踢掉
                im.syncAll();
                int state = im.tcp().state();
                if (state == com.example.helloworld.im.TcpClient.STATE_DISCONNECTED
                        || state == com.example.helloworld.im.TcpClient.STATE_FATAL) {
                    im.startDataPlane();
                }
            }
            swipe.postDelayed(() -> {
                if (swipe != null) {
                    swipe.setRefreshing(false);
                }
                refresh();
            }, 700);
        });
    }

    @Override
    public void onResume() {
        super.onResume();
        if (im != null) {
            im.chat().addListener(chatListener);
            im.contacts().addListener(contactsListener);
        }
        refresh();
    }

    @Override
    public void onPause() {
        super.onPause();
        if (im != null) {
            im.chat().removeListener(chatListener);
            im.contacts().removeListener(contactsListener);
        }
    }

    private void refresh() {
        if (adapter == null || im == null) {
            return;
        }
        List<Conversation> list = im.chat().conversations();
        // 标题兜底：离线补推的会话可能还没被通讯录回填过
        for (Conversation c : list) {
            if (c.title == null || c.title.isEmpty()) {
                c.title = im.contacts().titleOf(c.peerId, c.isGroup);
            }
        }
        adapter.submit(list);
        emptyView.setVisibility(list.isEmpty() ? View.VISIBLE : View.GONE);
        rv.setVisibility(list.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void openConversation(Conversation c) {
        Intent i = new Intent(requireContext(), ChatActivity.class);
        i.putExtra("peer_id", c.peerId);
        i.putExtra("is_group", c.isGroup);
        i.putExtra("title", c.title);
        startActivity(i);
    }

    private void showRowMenu(Conversation c) {
        String[] items = c.unread > 0
                ? new String[]{"标记为已读", "删除会话"}
                : new String[]{"删除会话"};
        Ui.choices(requireContext(), c.title, items, (index, textItem) -> {
            if ("标记为已读".equals(textItem)) {
                im.chat().markRead(c.peerId, c.isGroup);
            } else {
                Ui.confirm(requireContext(), "删除会话",
                        "确定删除与「" + c.title + "」的会话吗？（不影响服务端历史消息）",
                        () -> {
                            im.chat().clearConversation(c.peerId, c.isGroup);
                            refresh();
                        });
            }
        });
    }
}
