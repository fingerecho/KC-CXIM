package com.example.helloworld.ui;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.view.View;
import android.view.inputmethod.InputMethodManager;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;

import com.google.android.material.snackbar.Snackbar;

import java.text.SimpleDateFormat;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;

/** UI 侧通用小工具：尺寸、时间、提示、确认框 */
public final class Ui {

    private static final SimpleDateFormat HHMM = new SimpleDateFormat("HH:mm", Locale.CHINA);
    private static final SimpleDateFormat MMDD = new SimpleDateFormat("MM-dd", Locale.CHINA);
    private static final SimpleDateFormat FULL = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.CHINA);
    /** 会话列表行很窄，跨年只用日期不含时分 */
    private static final SimpleDateFormat YEAR_MD = new SimpleDateFormat("yyyy-MM-dd", Locale.CHINA);

    private Ui() {
    }

    public static int dp(Context c, float v) {
        return Math.round(v * c.getResources().getDisplayMetrics().density);
    }

    public static int sp(Context c, float v) {
        return Math.round(v * c.getResources().getDisplayMetrics().scaledDensity);
    }

    /** 会话列表右侧时间：今天 HH:mm，昨天「昨天」，本年 MM-dd，跨年完整日期 */
    public static String listTime(long ts) {
        if (ts <= 0) {
            return "";
        }
        Calendar now = Calendar.getInstance();
        Calendar c = Calendar.getInstance();
        c.setTimeInMillis(ts);
        if (isSameDay(now, c)) {
            return HHMM.format(new Date(ts));
        }
        Calendar y = Calendar.getInstance();
        y.add(Calendar.DAY_OF_YEAR, -1);
        if (isSameDay(y, c)) {
            return "昨天";
        }
        if (now.get(Calendar.YEAR) == c.get(Calendar.YEAR)) {
            return MMDD.format(new Date(ts));
        }
        return YEAR_MD.format(new Date(ts));
    }

    /** 聊天页时间分隔：跨天显示日期，同天显示 HH:mm */
    public static String chatTime(long ts) {
        if (ts <= 0) {
            return "";
        }
        Calendar now = Calendar.getInstance();
        Calendar c = Calendar.getInstance();
        c.setTimeInMillis(ts);
        if (isSameDay(now, c)) {
            return HHMM.format(new Date(ts));
        }
        if (now.get(Calendar.YEAR) == c.get(Calendar.YEAR)) {
            return MMDD.format(new Date(ts)) + " " + HHMM.format(new Date(ts));
        }
        return FULL.format(new Date(ts));
    }

    /** 两条消息之间是否需要插入时间分隔（间隔超过 5 分钟） */
    public static boolean needTimeDivider(long prevTs, long ts) {
        if (prevTs <= 0) {
            return true;
        }
        return ts - prevTs > 5 * 60 * 1000L;
    }

    private static boolean isSameDay(Calendar a, Calendar b) {
        return a.get(Calendar.YEAR) == b.get(Calendar.YEAR)
                && a.get(Calendar.DAY_OF_YEAR) == b.get(Calendar.DAY_OF_YEAR);
    }

    /** 字节数人性化（与桌面端 humanSize 一致，保留 1 位小数） */
    public static String humanSize(long bytes) {
        if (bytes < 1024) {
            return bytes + "B";
        }
        String[] units = {"KB", "MB", "GB", "TB"};
        double v = bytes;
        int i = -1;
        while (v >= 1024 && i < units.length - 1) {
            v /= 1024;
            i++;
        }
        return String.format(Locale.US, "%.1f%s", v, units[i]);
    }

    public static void toast(Context c, String text) {
        if (c == null || text == null) {
            return;
        }
        Toast.makeText(c.getApplicationContext(), text, Toast.LENGTH_SHORT).show();
    }

    public static void snack(View anchor, String text) {
        if (anchor == null) {
            return;
        }
        Snackbar.make(anchor, text, Snackbar.LENGTH_SHORT).show();
    }

    /** 确认框：与桌面端 QMessageBox::question 的语义一致 */
    public static void confirm(Context c, String title, String message, Runnable onOk) {
        new AlertDialog.Builder(c)
                .setTitle(title)
                .setMessage(message)
                .setNegativeButton("取消", null)
                .setPositiveButton("确定", (d, w) -> {
                    if (onOk != null) {
                        onOk.run();
                    }
                })
                .show();
    }

    /** 带「取消」按钮之外的第三个动作，用于成员长按菜单等场景 */
    public static void choices(Context c, String title, String[] items, OnChoice onChoice) {
        new AlertDialog.Builder(c)
                .setTitle(title)
                .setItems(items, (d, which) -> onChoice.onChoice(which, items[which]))
                .setNegativeButton("取消", null)
                .show();
    }

    public interface OnChoice {
        void onChoice(int index, String text);
    }

    public static void hideKeyboard(Activity a) {
        View focus = a.getCurrentFocus();
        if (focus == null) {
            return;
        }
        InputMethodManager imm = (InputMethodManager) a.getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm != null) {
            imm.hideSoftInputFromWindow(focus.getWindowToken(), 0);
        }
    }

    /** 跳到登录页并清栈（会话过期时用） */
    public static void gotoLogin(Context c) {
        Intent i = new Intent(c, LoginActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        c.startActivity(i);
    }
}
