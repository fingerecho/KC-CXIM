package com.example.helloworld.ui;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import com.example.helloworld.R;
import com.example.helloworld.im.GroupService;
import com.example.helloworld.im.model.GroupRequestInfo;
import com.example.helloworld.ui.widget.AvatarView;

import java.util.ArrayList;
import java.util.List;

/**
 * 入群申请列表（协议 §4.14.7）。
 * direction=incoming 是「待我审批」（我是群主/管理员的群），outgoing 是「我发出的」。
 * 审批成功后就地把该行变成灰字状态，并顺带对齐群列表与角标。
 */
public class GroupRequestsActivity extends BaseActivity {

    public static final String EXTRA_DIRECTION = "direction";

    private static final String DIRECTION_INCOMING = "incoming";
    private static final String DIRECTION_OUTGOING = "outgoing";

    /** 申请状态，与 friend_requests 的口径一致：0=待处理 1=通过 2=拒绝 3=取消 */
    private static final int STATUS_PENDING = 0;
    private static final int STATUS_ACCEPTED = 1;
    private static final int STATUS_REJECTED = 2;
    private static final int STATUS_CANCELED = 3;

    private String direction = DIRECTION_INCOMING;
    private final List<GroupRequestInfo> items = new ArrayList<>();

    private SwipeRefreshLayout swipe;
    private RecyclerView rv;
    private View layoutEmpty;
    private TextView tvEmpty;
    private RequestAdapter adapter;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.group_activity_requests);
        setHeaderTitle(getString(R.string.group_title_join_requests));

        String d = getIntent().getStringExtra(EXTRA_DIRECTION);
        direction = DIRECTION_OUTGOING.equals(d) ? DIRECTION_OUTGOING : DIRECTION_INCOMING;

        swipe = findViewById(R.id.group_swipe);
        rv = findViewById(R.id.group_rv_requests);
        layoutEmpty = findViewById(R.id.group_empty);
        tvEmpty = findViewById(R.id.group_tv_empty);

        adapter = new RequestAdapter();
        rv.setLayoutManager(new LinearLayoutManager(this));
        rv.setAdapter(adapter);

        swipe.setOnRefreshListener(this::load);
        refreshHeaderAction();
        load();
    }

    /** 右上角文字就是当前方向，点击弹出切换 */
    private void refreshHeaderAction() {
        boolean incoming = DIRECTION_INCOMING.equals(direction);
        setHeaderAction(getString(incoming
                        ? R.string.group_header_switch : R.string.group_header_switch_outgoing),
                v -> switchDirection());
    }

    private void switchDirection() {
        String[] labels = {getString(R.string.group_direction_incoming),
                getString(R.string.group_direction_outgoing)};
        Ui.choices(this, getString(R.string.group_switch_title), labels, (index, text) -> {
            direction = index == 0 ? DIRECTION_INCOMING : DIRECTION_OUTGOING;
            refreshHeaderAction();
            load();
        });
    }

    private void load() {
        // 快照本次请求的方向：切换 direction 后旧请求的慢响应不能覆盖新列表
        final String requested = direction;
        im.group().listJoinRequests(requested, r -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            if (!requested.equals(direction)) {
                return;
            }
            swipe.setRefreshing(false);
            if (!r.ok) {
                toast(r.errorText(getString(R.string.group_err_load_failed)));
                return;
            }
            items.clear();
            items.addAll(GroupService.parseRequests(r));
            tvEmpty.setText(emptyText());
            layoutEmpty.setVisibility(items.isEmpty() ? View.VISIBLE : View.GONE);
            adapter.notifyDataSetChanged();
        });
    }

    private String emptyText() {
        return getString(DIRECTION_INCOMING.equals(direction)
                ? R.string.group_empty_incoming : R.string.group_empty_outgoing);
    }

    // ===== 审批 =====

    private void accept(GroupRequestInfo req, int position) {
        im.group().acceptJoin(req.requestId, (ok, error, data) -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            if (!ok) {
                toast(error == null || error.isEmpty()
                        ? getString(R.string.group_err_op_failed) : error);
                return;
            }
            req.status = STATUS_ACCEPTED;
            notifyRow(position);
            toast(getString(R.string.group_toast_accepted));
            // 通过后群里多了一个人，群列表与角标都要对齐
            im.refreshGroups();
            im.refreshPendingCounts();
        });
    }

    private void reject(GroupRequestInfo req, int position) {
        im.group().rejectJoin(req.requestId, (ok, error, data) -> {
            if (isFinishing() || isDestroyed()) {
                return;
            }
            if (!ok) {
                toast(error == null || error.isEmpty()
                        ? getString(R.string.group_err_op_failed) : error);
                return;
            }
            req.status = STATUS_REJECTED;
            notifyRow(position);
            toast(getString(R.string.group_toast_rejected));
            im.refreshPendingCounts();
        });
    }

    /** 位置可能因为刷新而失效，越界就忽略，交给下一次全量刷新 */
    private void notifyRow(int position) {
        if (position >= 0 && position < items.size()) {
            adapter.notifyItemChanged(position);
        }
    }

    private String statusText(int status) {
        if (status == STATUS_ACCEPTED) {
            return getString(R.string.group_status_accepted);
        }
        if (status == STATUS_REJECTED) {
            return getString(R.string.group_status_rejected);
        }
        if (status == STATUS_CANCELED) {
            return getString(R.string.group_status_canceled);
        }
        return getString(R.string.group_status_pending);
    }

    private class RequestAdapter extends RecyclerView.Adapter<RequestVh> {

        @NonNull
        @Override
        public RequestVh onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new RequestVh(LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.group_item_request, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull RequestVh holder, int position) {
            GroupRequestInfo req = items.get(position);
            boolean incoming = DIRECTION_INCOMING.equals(direction);

            if (incoming) {
                holder.avatar.setPerson(req.requesterId, req.requesterName());
                holder.tvName.setText(getString(R.string.group_request_line,
                        req.requesterName(), req.requesterId));
            } else {
                holder.avatar.setGroup(req.groupId, req.groupName);
                holder.tvName.setText(req.groupNameDisplay());
            }
            String message = req.message.trim().isEmpty()
                    ? getString(R.string.group_request_no_message) : req.message.trim();
            holder.tvDesc.setText(getString(R.string.group_request_desc,
                    req.groupNameDisplay(), message));
            holder.tvTime.setText(Ui.listTime(req.createdTs));

            // 待审批才允许点按钮；已处理的整行压暗、按钮就地禁用，右侧状态灰字说明结果
            boolean pending = req.status == STATUS_PENDING;
            holder.actions.setVisibility(incoming ? View.VISIBLE : View.GONE);
            holder.btnAccept.setEnabled(pending);
            holder.btnReject.setEnabled(pending);
            holder.btnAccept.setAlpha(pending ? 1f : 0.4f);
            holder.btnReject.setAlpha(pending ? 1f : 0.4f);
            holder.btnAccept.setOnClickListener(pending
                    ? v -> accept(req, holder.getBindingAdapterPosition()) : null);
            holder.btnReject.setOnClickListener(pending
                    ? v -> reject(req, holder.getBindingAdapterPosition()) : null);
            holder.tvStatus.setVisibility(View.VISIBLE);
            holder.tvStatus.setText(statusText(req.status));
            holder.row.setAlpha(pending && incoming ? 1f : 0.7f);
        }

        @Override
        public int getItemCount() {
            return items.size();
        }
    }

    private static class RequestVh extends RecyclerView.ViewHolder {
        final View row;
        final AvatarView avatar;
        final TextView tvName;
        final TextView tvDesc;
        final TextView tvTime;
        final View actions;
        final TextView tvStatus;
        final Button btnAccept;
        final Button btnReject;

        RequestVh(@NonNull View itemView) {
            super(itemView);
            row = itemView.findViewById(R.id.group_row_root);
            avatar = itemView.findViewById(R.id.group_avatar);
            tvName = itemView.findViewById(R.id.group_tv_name);
            tvDesc = itemView.findViewById(R.id.group_tv_desc);
            tvTime = itemView.findViewById(R.id.group_tv_time);
            actions = itemView.findViewById(R.id.group_row_actions);
            tvStatus = itemView.findViewById(R.id.group_tv_status);
            btnAccept = itemView.findViewById(R.id.group_btn_accept);
            btnReject = itemView.findViewById(R.id.group_btn_reject);
        }
    }
}
