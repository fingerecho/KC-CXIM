package com.example.helloworld.ui;

import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageButton;
import android.widget.TextView;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import com.example.helloworld.CXIMApp;
import com.example.helloworld.R;
import com.example.helloworld.im.ImService;
import com.example.helloworld.im.TcpClient;

/**
 * 所有页面的基类：统一顶栏绑定、会话过期处理与长连接保活。
 *
 * <p>会话过期（refresh token 也失效）时弹一次提示并清栈回登录页，
 * 对应桌面端 ChatView::sessionExpired → logoutRequested 的处理。
 */
public abstract class BaseActivity extends AppCompatActivity implements ImService.SessionListener {

    protected ImService im;

    @Nullable
    protected View header;
    @Nullable
    protected TextView tvTitle;
    @Nullable
    protected TextView tvAction;
    @Nullable
    protected ImageButton btnBack;
    @Nullable
    protected ImageButton btnAction;

    private boolean expiredShown;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        im = CXIMApp.service();
        // Application 未就绪时不做任何事也比后面每个页面各自判空可靠
        if (im == null) {
            finish();
        }
    }

    @Override
    public void setContentView(int layoutResID) {
        super.setContentView(layoutResID);
        bindHeaderViews();
    }

    /** 页面是否需要登录态；登录/注册/服务器设置页返回 false */
    protected boolean requiresLogin() {
        return true;
    }

    private void bindHeaderViews() {
        header = findViewById(R.id.header);
        tvTitle = findViewById(R.id.tv_title);
        tvAction = findViewById(R.id.tv_action);
        btnBack = findViewById(R.id.btn_back);
        btnAction = findViewById(R.id.btn_action);
        if (btnBack != null) {
            btnBack.setOnClickListener(v -> onBackPressed());
        }
        // 每个带顶栏的页面都自动躲开状态栏，不必各页自己写
        protectBarBelowStatusBar(header);
    }

    /**
     * 底部栏同理：edge-to-edge 下系统手势条/导航栏会压在底部导航上，
     * 把导航栏高度的内缩补成 paddingBottom，图标就不会被手势条盖住。
     */
    protected void protectBarAboveNavigationBar(@Nullable View bar) {
        if (bar == null) {
            return;
        }
        androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(bar, (v, insets) -> {
            androidx.core.graphics.Insets nb = insets.getInsets(
                    androidx.core.view.WindowInsetsCompat.Type.navigationBars()
                            | androidx.core.view.WindowInsetsCompat.Type.displayCutout());
            if (v.getPaddingBottom() != nb.bottom) {
                v.setPadding(v.getPaddingLeft(), v.getPaddingTop(), v.getPaddingRight(), nb.bottom);
            }
            return insets;
        });
        androidx.core.view.ViewCompat.requestApplyInsets(bar);
    }

    /**
     * 把一条自绘的顶部栏从状态栏底下挪下来。
     *
     * <p>targetSdk 35 起系统强制 edge-to-edge，布局没有 fitsSystemWindows 时
     * 顶栏会直接画在状态栏底下，表现为「左上角返回按钮和时钟/电量叠在一起、按不动」。
     * 这里给顶栏补 paddingTop = 状态栏高度（含刘海）+ 约一个汉字的留白；
     * 同时把固定高度改成 wrap_content + minHeight，让留白只把顶栏撑高、不挤压里面的内容。
     *
     * <p>低版本（API &lt; 35）系统本来就把内容顶在状态栏下面，此时 statusBars() 内缩为 0，
     * 这段代码只加上留白，不会出现双重留白，所以同一套逻辑在所有版本都成立。
     */
    protected void protectBarBelowStatusBar(@Nullable View bar) {
        // 20dp ≈ 一个汉字高度：状态栏之下再留一条空白带，返回键不再贴着时钟
        protectBarBelowStatusBar(bar, 20,
                getResources().getDimensionPixelSize(R.dimen.header_height));
    }

    /**
     * @param extraDp         状态栏之下额外留出的空白（dp）
     * @param minContentHeight 内容区最小高度（px），0 表示不改变自身高度约束
     */
    protected void protectBarBelowStatusBar(@Nullable View bar, int extraDp, int minContentHeight) {
        if (bar == null) {
            return;
        }
        final int extra = Ui.dp(this, extraDp);
        androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(bar, (v, insets) -> {
            androidx.core.graphics.Insets bars = insets.getInsets(
                    androidx.core.view.WindowInsetsCompat.Type.statusBars()
                            | androidx.core.view.WindowInsetsCompat.Type.displayCutout());
            int top = bars.top + extra;
            if (v.getPaddingTop() != top) {
                v.setPadding(v.getPaddingLeft(), top, v.getPaddingRight(), v.getPaddingBottom());
            }
            if (minContentHeight > 0) {
                ViewGroup.LayoutParams lp = v.getLayoutParams();
                if (lp != null && lp.height != ViewGroup.LayoutParams.WRAP_CONTENT) {
                    lp.height = ViewGroup.LayoutParams.WRAP_CONTENT;
                    v.setLayoutParams(lp);
                }
                if (v.getMinimumHeight() < minContentHeight + top) {
                    v.setMinimumHeight(minContentHeight + top);
                }
            }
            return insets;
        });
        androidx.core.view.ViewCompat.requestApplyInsets(bar);
    }

    protected void setHeaderTitle(String title) {
        if (tvTitle != null) {
            tvTitle.setText(title);
        }
    }

    protected void hideBackButton() {
        if (btnBack != null) {
            btnBack.setVisibility(View.GONE);
        }
    }

    /** 顶栏右侧文字动作 */
    protected void setHeaderAction(String text, View.OnClickListener onClick) {
        if (tvAction == null) {
            return;
        }
        tvAction.setVisibility(View.VISIBLE);
        tvAction.setText(text);
        tvAction.setOnClickListener(onClick);
    }

    /** 顶栏右侧图标动作 */
    protected void setHeaderIcon(int iconRes, String desc, View.OnClickListener onClick) {
        if (btnAction == null) {
            return;
        }
        btnAction.setVisibility(View.VISIBLE);
        btnAction.setImageResource(iconRes);
        btnAction.setContentDescription(desc);
        btnAction.setOnClickListener(onClick);
    }

    @Override
    protected void onResume() {
        super.onResume();
        expiredShown = false;
        if (im == null) {
            return;
        }
        if (requiresLogin() && !im.isLoggedIn()) {
            finish();
            Ui.gotoLogin(this);
            return;
        }
        im.addSessionListener(this);
        ensureConnected();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (im != null) {
            im.removeSessionListener(this);
        }
    }

    /**
     * 回到前台时如果长连接没起来就重连一次。
     * Android 会在应用退到后台后冻结进程，长连接可能已经断了；
     * TcpClient 内部有抖动退避，这里只是补一次“立刻尝试”。
     */
    protected void ensureConnected() {
        if (im == null || !im.isLoggedIn()) {
            return;
        }
        int state = im.tcp().state();
        if (state == TcpClient.STATE_DISCONNECTED || state == TcpClient.STATE_FATAL) {
            im.startDataPlane();
        }
    }

    @Override
    public void onSessionExpired(String reason) {
        // 回调可能来自后台线程池，此时 Activity 也许已经销毁，对它 show 对话框会抛 BadTokenException
        if (expiredShown || isFinishing() || isDestroyed()) {
            return;
        }
        expiredShown = true;
        runOnUiThread(() -> new AlertDialog.Builder(this)
                .setTitle("登录已过期")
                .setMessage(reason == null || reason.isEmpty() ? "登录已过期，请重新登录" : reason)
                .setCancelable(false)
                .setPositiveButton("重新登录", (d, w) -> {
                    if (im != null) {
                        im.logout();
                    }
                    Ui.gotoLogin(BaseActivity.this);
                })
                .show());
    }

    @Override
    public void onConnectionStateChanged(int state) {
    }

    @Override
    public void onDataPlaneReady() {
    }

    /** 轻提示：对应桌面端面板右上角 3s toast */
    protected void toast(String text) {
        View root = findViewById(android.R.id.content);
        if (root != null) {
            Ui.snack(root, text);
        } else {
            Ui.toast(this, text);
        }
    }
}
