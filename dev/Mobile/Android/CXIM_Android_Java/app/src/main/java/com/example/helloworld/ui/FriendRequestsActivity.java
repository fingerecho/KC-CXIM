package com.example.helloworld.ui;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import com.example.helloworld.R;
import com.example.helloworld.im.FriendService;
import com.example.helloworld.im.model.FriendRequestInfo;
import com.example.helloworld.ui.widget.AvatarView;
import com.google.android.material.button.MaterialButton;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * 新朋友：好友申请列表（协议 §4.6）。
 *
 * <p>顶栏右上角在「收到的 / 发出的」之间切换，direction 直接透传给服务端；
 * 列表只展示 status==0 的待处理项，通过后就地改成「已添加」，同时刷新通讯录与角标。
 */
public class FriendRequestsActivity extends BaseActivity {

    private static final String INCOMING = "incoming";
    private static final String OUTGOING = "outgoing";

    private String direction = INCOMING;
    private TextView tvStatus;
    private View emptyView;
    private SwipeRefreshLayout swipe;
    private RecyclerView rv;

    private RequestAdapter adapter;
    private final List<FriendRequestInfo> items = new ArrayList<>();
    /** 本页已点过「通过」的申请，就地变状态，不等列表重拉 */
    private final Set<Long> accepted = new HashSet<>();
    private final Set<Long> busy = new HashSet<>();
    private boolean loading;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.friend_activity_requests);
        setHeaderTitle(getString(R.string.new_friends));

        tvStatus = findViewById(R.id.tv_status);
        emptyView = findViewById(R.id.empty_view);
        swipe = findViewById(R.id.swipe_refresh);
        rv = findViewById(R.id.rv_requests);

        adapter = new RequestAdapter();
        rv.setLayoutManager(new LinearLayoutManager(this));
        rv.setAdapter(adapter);

        swipe.setColorSchemeResources(R.color.brand_bottom, R.color.brand_top, R.color.brand_accent);
        swipe.setOnRefreshListener(this::load);
        updateHeaderAction();
        load();
    }

    /** 右上角显示当前视角，点开在「收到的 / 发出的」之间切换 */
    private void updateHeaderAction() {
        boolean incoming = INCOMING.equals(direction);
        String current = getString(incoming ? R.string.friend_req_incoming : R.string.friend_req_outgoing);
        setHeaderAction(current, v -> {
            String[] labels = new String[]{
                    getString(R.string.friend_req_incoming),
                    getString(R.string.friend_req_outgoing)
            };
            Ui.choices(this, getString(R.string.new_friends), labels, (index, text) -> {
                String target = index == 0 ? INCOMING : OUTGOING;
                if (target.equals(direction)) {
                    return;
                }
                direction = target;
                accepted.clear();
                busy.clear();
                updateHeaderAction();
                load();
            });
        });
    }

    // ===================================================================
    //  加载
    // ===================================================================

    private void load() {
        if (loading) {
            swipe.setRefreshing(false);
            return;
        }
        loading = true;
        tvStatus.setVisibility(View.VISIBLE);
        tvStatus.setText(R.string.loading);
        final String dir = direction;
        im.friend().listRequests(dir, r -> {
            loading = false;
            swipe.setRefreshing(false);
            // 中途切换过方向就丢掉这次结果
            if (!dir.equals(direction)) {
                return;
            }
            if (!r.ok) {
                items.clear();
                adapter.notifyDataSetChanged();
                tvStatus.setText(r.errorText(getString(R.string.friend_req_fail)));
                emptyView.setVisibility(View.VISIBLE);
                return;
            }
            items.clear();
            for (FriendRequestInfo req : FriendService.parseRequests(r)) {
                if (req.status == FriendRequestInfo.PENDING) {
                    items.add(req);
                }
            }
            adapter.notifyDataSetChanged();
            tvStatus.setVisibility(View.GONE);
            emptyView.setVisibility(items.isEmpty() ? View.VISIBLE : View.GONE);
        });
    }

    // ===================================================================
    //  通过 / 拒绝
    // ===================================================================

    private void accept(FriendRequestInfo req) {
        if (busy.contains(req.requestId)) {
            return;
        }
        busy.add(req.requestId);
        adapter.notifyDataSetChanged();
        im.friend().acceptRequest(req.requestId, r -> {
            busy.remove(req.requestId);
            if (r.ok) {
                accepted.add(req.requestId);
                toast(getString(R.string.friend_req_accept_done));
                // 事实变了：好友列表 + 角标都要重拉
                im.refreshFriends();
                im.refreshPendingCounts();
            } else {
                toast(r.errorText(getString(R.string.friend_req_accept_fail)));
            }
            adapter.notifyDataSetChanged();
        });
    }

    private void reject(FriendRequestInfo req) {
        if (busy.contains(req.requestId)) {
            return;
        }
        busy.add(req.requestId);
        adapter.notifyDataSetChanged();
        im.friend().rejectRequest(req.requestId, r -> {
            busy.remove(req.requestId);
            if (r.ok) {
                toast(getString(R.string.friend_req_reject_done));
                int idx = indexOf(req.requestId);
                if (idx >= 0) {
                    items.remove(idx);
                    adapter.notifyItemRemoved(idx);
                }
                emptyView.setVisibility(items.isEmpty() ? View.VISIBLE : View.GONE);
                im.refreshPendingCounts();
            } else {
                toast(r.errorText(getString(R.string.friend_req_reject_fail)));
                adapter.notifyDataSetChanged();
            }
        });
    }

    private int indexOf(long requestId) {
        for (int i = 0; i < items.size(); i++) {
            if (items.get(i).requestId == requestId) {
                return i;
            }
        }
        return -1;
    }

    // ===================================================================
    //  Adapter
    // ===================================================================

    private class RequestAdapter extends RecyclerView.Adapter<RequestHolder> {

        @NonNull
        @Override
        public RequestHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new RequestHolder(LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.friend_item_request, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull RequestHolder holder, int position) {
            holder.bind(items.get(position));
        }

        @Override
        public int getItemCount() {
            return items.size();
        }
    }

    private class RequestHolder extends RecyclerView.ViewHolder {
        final AvatarView avatar;
        final TextView tvName;
        final TextView tvMessage;
        final TextView tvTime;
        final View actions;
        final MaterialButton btnAccept;
        final MaterialButton btnReject;
        final TextView tvState;

        RequestHolder(@NonNull View itemView) {
            super(itemView);
            avatar = itemView.findViewById(R.id.avatar);
            tvName = itemView.findViewById(R.id.tv_name);
            tvMessage = itemView.findViewById(R.id.tv_message);
            tvTime = itemView.findViewById(R.id.tv_time);
            actions = itemView.findViewById(R.id.layout_actions);
            btnAccept = itemView.findViewById(R.id.btn_accept);
            btnReject = itemView.findViewById(R.id.btn_reject);
            tvState = itemView.findViewById(R.id.tv_state);
        }

        void bind(FriendRequestInfo req) {
            boolean incoming = INCOMING.equals(direction);
            // 收到的看申请人，发出的看被申请人
            long peerId = incoming ? req.requesterId : req.targetId;
            String peerName = incoming ? req.requesterName() : req.targetName();

            avatar.setPerson(peerId, peerName);
            tvName.setText(getString(R.string.friend_name_id_fmt, peerName, peerId));
            tvMessage.setText(req.message == null || req.message.trim().isEmpty()
                    ? getString(R.string.friend_req_message_none)
                    : getString(R.string.friend_req_message_fmt, req.message.trim()));
            tvTime.setText(Ui.listTime(req.createdTs));

            if (!incoming) {
                // 发出的只能等对方处理
                actions.setVisibility(View.GONE);
                tvState.setVisibility(View.VISIBLE);
                tvState.setText(R.string.friend_req_waiting);
                return;
            }

            tvState.setVisibility(View.VISIBLE);
            if (accepted.contains(req.requestId)) {
                actions.setVisibility(View.GONE);
                tvState.setText(R.string.friend_req_accepted);
            } else {
                actions.setVisibility(View.VISIBLE);
                tvState.setVisibility(View.GONE);
                boolean busyNow = busy.contains(req.requestId);
                btnAccept.setEnabled(!busyNow);
                btnReject.setEnabled(!busyNow);
                btnAccept.setText(busyNow ? R.string.friend_req_accepting : R.string.friend_req_accept);
                btnAccept.setOnClickListener(v -> accept(req));
                btnReject.setOnClickListener(v -> reject(req));
            }
        }
    }
}
