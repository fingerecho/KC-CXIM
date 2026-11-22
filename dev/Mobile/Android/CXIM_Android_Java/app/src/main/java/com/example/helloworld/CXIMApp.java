package com.example.helloworld;

import android.app.Application;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.os.Build;

import com.example.helloworld.im.ImService;
import com.example.helloworld.ui.Notifier;

/**
 * 应用入口：持有进程级唯一的 {@link ImService}（长连接 + 通讯录缓存 + 会话数据）。
 * 放在 Application 而不是 Activity，是为了在页面之间切换时不断连接、不丢离线补推的消息。
 */
public class CXIMApp extends Application {

    public static final String CHANNEL_ID = "cxim_message";

    private static CXIMApp instance;
    private ImService im;

    public static CXIMApp get() {
        return instance;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        im = new ImService(this);
        createNotificationChannel();
        // 全局通知器常驻：界面退到后台时由它把消息转成系统通知
        Notifier notifier = new Notifier(this, im);
        registerActivityLifecycleCallbacks(new ForwardingCallbacks(notifier));
    }

    /** 用 Activity 计数判断前后台，避免引入 lifecycle-process 依赖 */
    private static class ForwardingCallbacks implements ActivityLifecycleCallbacks {
        private final Notifier notifier;

        ForwardingCallbacks(Notifier notifier) {
            this.notifier = notifier;
        }

        @Override
        public void onActivityStarted(android.app.Activity activity) {
            notifier.onActivityStarted();
        }

        @Override
        public void onActivityStopped(android.app.Activity activity) {
            notifier.onActivityStopped();
        }

        @Override
        public void onActivityCreated(android.app.Activity a, android.os.Bundle b) {
        }

        @Override
        public void onActivityResumed(android.app.Activity a) {
        }

        @Override
        public void onActivityPaused(android.app.Activity a) {
        }

        @Override
        public void onActivitySaveInstanceState(android.app.Activity a, android.os.Bundle b) {
        }

        @Override
        public void onActivityDestroyed(android.app.Activity a) {
        }
    }

    public ImService im() {
        return im;
    }

    /** 便捷静态入口，Activity 里少写样板 */
    public static ImService service() {
        return instance == null ? null : instance.im;
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm == null) {
            return;
        }
        NotificationChannel ch = new NotificationChannel(CHANNEL_ID,
                getString(R.string.channel_name), NotificationManager.IMPORTANCE_DEFAULT);
        ch.setDescription(getString(R.string.channel_desc));
        nm.createNotificationChannel(ch);
    }
}
