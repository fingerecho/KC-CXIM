package com.example.helloworld.ui.widget;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.View;

import androidx.annotation.Nullable;

/**
 * 圆形头像控件，复刻桌面端 AvatarWidget 的规则：
 * <ul>
 *   <li>底色 = 调色板[seed(userId) % 8]</li>
 *   <li>无图时画名字首字符（大写；中文取首字），字号 = 直径 × 0.45，白色粗体</li>
 *   <li>群聊模式画「群」字，底色固定用品牌蓝渐变的中段，便于和好友区分</li>
 * </ul>
 */
public class AvatarView extends View {

    private static final int[] PALETTE = {
            0xFF5AA3DF, 0xFF6CC4A1, 0xFFE8A25C, 0xFFB58AD6,
            0xFFE07A8F, 0xFF5CB8C4, 0xFF8FA85C, 0xFFD68AB0
    };

    private static final int GROUP_COLOR = 0xFF5A7FA6;

    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();

    private String label = "";
    private int color = PALETTE[0];
    @Nullable
    private Bitmap image;

    public AvatarView(Context context) {
        super(context);
        init();
    }

    public AvatarView(Context context, @Nullable AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public AvatarView(Context context, @Nullable AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init();
    }

    private void init() {
        textPaint.setColor(Color.WHITE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setTypeface(Typeface.DEFAULT_BOLD);
        setLayerType(LAYER_TYPE_SOFTWARE, null);
    }

    /** 个人头像：按 userId 取色，取名字首字符 */
    public void setPerson(long userId, @Nullable String name) {
        this.color = PALETTE[(int) Math.floorMod(seedOf(userId), PALETTE.length)];
        this.label = firstChar(name);
        this.image = null;
        invalidate();
    }

    /** 群头像 */
    public void setGroup(long groupId, @Nullable String name) {
        this.color = GROUP_COLOR;
        this.label = "群";
        this.image = null;
        invalidate();
    }

    public void setImage(@Nullable Bitmap bitmap) {
        this.image = bitmap;
        invalidate();
    }

    public void setColor(int color) {
        this.color = color;
        invalidate();
    }

    /** 与桌面端 seed() 同口径：让同一 userId 在任何端都是同一个颜色 */
    private static long seedOf(long id) {
        long v = id == 0 ? 1 : Math.abs(id);
        return (v * 2654435761L) >>> 1;
    }

    private static String firstChar(@Nullable String name) {
        if (name == null || name.trim().isEmpty()) {
            return "?";
        }
        String s = name.trim();
        int cp = s.codePointAt(0);
        String ch = new String(Character.toChars(cp));
        return ch.toUpperCase();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth();
        int h = getHeight();
        if (w == 0 || h == 0) {
            return;
        }
        float size = Math.min(w, h);
        float left = (w - size) / 2f;
        float top = (h - size) / 2f;
        rect.set(left, top, left + size, top + size);

        fillPaint.setColor(color);
        canvas.drawOval(rect, fillPaint);

        if (image != null && !image.isRecycled()) {
            // 有真实头像时圆形裁切贴图
            int save = canvas.save();
            android.graphics.Path clip = new android.graphics.Path();
            clip.addOval(rect, android.graphics.Path.Direction.CW);
            canvas.clipPath(clip);
            canvas.drawBitmap(image, null, rect, fillPaint);
            canvas.restoreToCount(save);
            return;
        }

        textPaint.setTextSize(size * 0.45f);
        Paint.FontMetrics fm = textPaint.getFontMetrics();
        float baseline = rect.centerY() - (fm.ascent + fm.descent) / 2f;
        canvas.drawText(label, rect.centerX(), baseline, textPaint);
    }
}
