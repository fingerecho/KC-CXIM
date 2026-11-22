package com.example.helloworld.ui;

import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.StringRes;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.example.helloworld.R;
import com.example.helloworld.im.ApiResult;
import com.example.helloworld.im.FriendService;
import com.example.helloworld.im.model.UserCard;
import com.example.helloworld.ui.widget.AvatarView;
import com.google.android.material.button.MaterialButton;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * 添加好友：按用户 ID / 用户名搜索（协议 §4.4），发申请（§4.8）。
 *
 * <p>申请是「发出即返回」的异步动作，所以按钮状态只在本地维护：
 * 空闲 → 发送中 → 已添加；服务端说 already friends 时同样落「已添加」。
 */
public class AddFriendActivity extends BaseActivity {

    private static final int ST_IDLE = 0;
    private static final int ST_SENDING = 1;
    private static final int ST_ADDED = 2;

    private EditText etKeyword;
    private MaterialButton btnSearch;
    private TextView tvStatus;
    private View emptyView;
    private RecyclerView rv;

    private UserAdapter adapter;
    private final List<UserCard> users = new ArrayList<>();
    /** userId -> ST_* ，搜索结果里的按钮状态 */
    private final Map<Long, Integer> states = new HashMap<>();
    private boolean searching;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.friend_activity_add);
        setHeaderTitle(getString(R.string.friend_add_title));

        etKeyword = findViewById(R.id.et_keyword);
        btnSearch = findViewById(R.id.btn_search);
        tvStatus = findViewById(R.id.tv_status);
        emptyView = findViewById(R.id.empty_view);
        rv = findViewById(R.id.rv_users);

        adapter = new UserAdapter();
        rv.setLayoutManager(new LinearLayoutManager(this));
        rv.setAdapter(adapter);

        btnSearch.setOnClickListener(v -> doSearch());
        etKeyword.setOnEditorActionListener((v, actionId, event) -> {
            if (actionId == EditorInfo.IME_ACTION_SEARCH) {
                doSearch();
                return true;
            }
            return false;
        });

        // 通讯录页回车跳过来时会带 keyword，直接搜一次
        String keyword = getIntent().getStringExtra("keyword");
        if (keyword != null && !keyword.trim().isEmpty()) {
            etKeyword.setText(keyword.trim());
            doSearch();
        }
    }

    // ===================================================================
    //  搜索
    // ===================================================================

    private void doSearch() {
        String kw = etKeyword.getText().toString().trim();
        Ui.hideKeyboard(this);
        if (kw.isEmpty()) {
            setStatus(R.string.friend_keyword_empty);
            return;
        }
        if (searching) {
            return;
        }
        searching = true;
        btnSearch.setEnabled(false);
        setStatus(R.string.friend_status_searching);
        im.friend().searchUsers(kw, r -> {
            searching = false;
            btnSearch.setEnabled(true);
            if (!r.ok) {
                showUsers(new ArrayList<>());
                setStatusText(r.errorText(getString(R.string.friend_status_fail)));
                return;
            }
            showUsers(filter(r));
            if (users.isEmpty()) {
                setStatus(R.string.friend_status_empty);
            } else {
                setStatusText(getString(R.string.friend_status_result_fmt, users.size()));
            }
        });
    }

    /** 去掉自己；已经是好友的直接标成「已添加」，省得再点一次吃一个错误码 */
    private List<UserCard> filter(ApiResult r) {
        List<UserCard> out = new ArrayList<>();
        long self = im.session().userId();
        for (UserCard u : FriendService.parseUsers(r)) {
            if (u.userId == self || u.userId == 0) {
                continue;
            }
            out.add(u);
            states.put(u.userId, im.contacts().isFriend(u.userId) ? ST_ADDED : ST_IDLE);
        }
        return out;
    }

    private void showUsers(List<UserCard> list) {
        users.clear();
        users.addAll(list);
        adapter.notifyDataSetChanged();
        emptyView.setVisibility(users.isEmpty() ? View.VISIBLE : View.GONE);
    }

    // ===================================================================
    //  发送申请
    // ===================================================================

    private void sendRequest(UserCard u) {
        if (stateOf(u.userId) != ST_IDLE) {
            return;
        }
        states.put(u.userId, ST_SENDING);
        notifyItem(u);
        im.friend().sendRequest(u.userId, FriendService.DEFAULT_REQUEST_MESSAGE, r -> {
            if (r.ok) {
                states.put(u.userId, ST_ADDED);
                setStatus(R.string.friend_status_sent);
            } else if (r.message.toLowerCase(Locale.US).contains("already friends")) {
                states.put(u.userId, ST_ADDED);
                setStatus(R.string.friend_status_already);
            } else {
                states.put(u.userId, ST_IDLE);
                setStatusText(r.errorText(getString(R.string.friend_status_add_fail)));
            }
            notifyItem(u);
        });
    }

    private int stateOf(long userId) {
        Integer s = states.get(userId);
        return s == null ? ST_IDLE : s;
    }

    private void notifyItem(UserCard u) {
        int idx = indexOf(u.userId);
        if (idx >= 0) {
            adapter.notifyItemChanged(idx);
        }
    }

    private int indexOf(long userId) {
        for (int i = 0; i < users.size(); i++) {
            if (users.get(i).userId == userId) {
                return i;
            }
        }
        return -1;
    }

    // ===================================================================
    //  状态行
    // ===================================================================

    private void setStatus(@StringRes int res) {
        tvStatus.setText(res);
    }

    private void setStatusText(String text) {
        tvStatus.setText(text);
    }

    // ===================================================================
    //  Adapter
    // ===================================================================

    private class UserAdapter extends RecyclerView.Adapter<UserHolder> {

        @NonNull
        @Override
        public UserHolder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
            return new UserHolder(LayoutInflater.from(parent.getContext())
                    .inflate(R.layout.friend_item_user, parent, false));
        }

        @Override
        public void onBindViewHolder(@NonNull UserHolder holder, int position) {
            holder.bind(users.get(position));
        }

        @Override
        public int getItemCount() {
            return users.size();
        }
    }

    private class UserHolder extends RecyclerView.ViewHolder {
        final AvatarView avatar;
        final TextView tvName;
        final TextView tvId;
        final TextView tvUsername;
        final MaterialButton btnAdd;

        UserHolder(@NonNull View itemView) {
            super(itemView);
            avatar = itemView.findViewById(R.id.avatar);
            tvName = itemView.findViewById(R.id.tv_name);
            tvId = itemView.findViewById(R.id.tv_id);
            tvUsername = itemView.findViewById(R.id.tv_username);
            btnAdd = itemView.findViewById(R.id.btn_add);
        }

        void bind(UserCard u) {
            String shown = u.displayName();
            avatar.setPerson(u.userId, shown);
            tvName.setText(shown);
            tvId.setText(getString(R.string.friend_id_label_fmt, u.userId));
            tvUsername.setText(u.username.isEmpty() ? "—" : u.username);

            int st = stateOf(u.userId);
            btnAdd.setEnabled(st == ST_IDLE);
            if (st == ST_SENDING) {
                btnAdd.setText(R.string.friend_btn_adding);
            } else if (st == ST_ADDED) {
                btnAdd.setText(R.string.friend_btn_added);
            } else {
                btnAdd.setText(R.string.friend_btn_add);
            }
            btnAdd.setOnClickListener(v -> sendRequest(u));
        }
    }
}
