package com.example.helloworld.ui;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.annotation.Nullable;
import androidx.core.content.FileProvider;

import com.example.helloworld.R;
import com.example.helloworld.im.Config;
import com.example.helloworld.im.FileUploader;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.TimeUnit;

import okhttp3.Call;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.Response;
import okhttp3.ResponseBody;

/**
 * 媒体消息的文件查看页：下载到应用外部私有目录（Downloads），图片页内预览，
 * 其他类型走 FileProvider + ACTION_VIEW 交给系统应用打开。
 *
 * <p>下载地址是服务器换取入口（GET /api/files/{uuid} → 302 签名直链，协议 §4.15.3），
 * 与 {@code HttpClient.getBytes} 走同一套鉴权与重连策略；这里自己发请求是为了拿到
 * 字节级进度，好把百分比显示出来。
 */
public class FileViewerActivity extends BaseActivity {

    public static final String EXTRA_URL = "url";
    public static final String EXTRA_NAME = "name";
    public static final String EXTRA_SIZE = "size";
    public static final String EXTRA_IS_IMAGE = "is_image";

    /** 页内预览图最长边上限，超过就不解码到全屏尺寸 */
    private static final int PREVIEW_MAX_PX = 1600;

    private String url = "";
    private String fileName = "";
    private long fileSize;
    private boolean isImage;

    private LinearLayout llProgress;
    private ProgressBar pbDownload;
    private TextView tvProgress;
    private ScrollView svPreview;
    private ImageView ivPreview;
    private LinearLayout llInfo;
    private TextView tvName;
    private TextView tvSize;
    private TextView tvPath;
    private Button btnOpen;
    private Button btnRetry;

    @Nullable
    private volatile Call inFlight;
    private volatile boolean destroyed;
    private int lastShownPct = -1;
    @Nullable
    private File savedFile;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.chat_activity_file_viewer);

        Intent intent = getIntent();
        url = intent == null ? "" : nvl(intent.getStringExtra(EXTRA_URL));
        fileName = sanitize(intent == null ? null : intent.getStringExtra(EXTRA_NAME));
        fileSize = intent != null ? intent.getLongExtra(EXTRA_SIZE, 0L) : 0L;
        isImage = intent != null && intent.getBooleanExtra(EXTRA_IS_IMAGE, false);
        if (isImage || FileUploader.isImage(fileName)) {
            isImage = true;
        }

        llProgress = findViewById(R.id.ll_progress);
        pbDownload = findViewById(R.id.pb_download);
        tvProgress = findViewById(R.id.tv_progress);
        svPreview = findViewById(R.id.sv_preview);
        ivPreview = findViewById(R.id.iv_preview);
        llInfo = findViewById(R.id.ll_info);
        tvName = findViewById(R.id.tv_name);
        tvSize = findViewById(R.id.tv_size);
        tvPath = findViewById(R.id.tv_path);
        btnOpen = findViewById(R.id.btn_open);
        btnRetry = findViewById(R.id.btn_retry);

        setHeaderTitle(fileName.isEmpty() ? getString(R.string.chat_pick_file) : fileName);
        tvName.setText(fileName);
        tvSize.setText(getString(R.string.chat_viewer_size, Ui.humanSize(fileSize)));
        btnRetry.setOnClickListener(v -> startDownload());
        btnOpen.setOnClickListener(v -> openWithOtherApp());

        if (url.isEmpty()) {
            showFailed(getString(R.string.chat_file_no_url));
        } else {
            startDownload();
        }
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        Call call = inFlight;
        if (call != null) {
            call.cancel();
        }
        super.onDestroy();
    }

    // ================================================================
    //  下载
    // ================================================================

    private void startDownload() {
        final File dest = downloadTarget();
        tvPath.setText(dest.getAbsolutePath());
        lastShownPct = -1;
        llProgress.setVisibility(View.VISIBLE);
        pbDownload.setProgress(0);
        tvProgress.setText(R.string.chat_viewer_downloading);
        svPreview.setVisibility(View.GONE);
        llInfo.setVisibility(isImage ? View.GONE : View.VISIBLE);
        btnOpen.setVisibility(View.GONE);
        btnRetry.setVisibility(View.GONE);

        im.http().enqueue(() -> {
            String error = null;
            try {
                streamDownloadTo(dest);
            } catch (IOException | RuntimeException e) {
                error = e.getMessage() == null ? e.getClass().getSimpleName() : e.getMessage();
            }
            if (destroyed) {
                return;
            }
            if (error != null || !dest.exists() || dest.length() == 0) {
                // 兜底走 FileUploader 的实现：鉴权 + 302 换取直链的口径与消息图片一致
                boolean ok = im.uploader().downloadTo(url, dest);
                if (!ok || !dest.exists() || dest.length() == 0) {
                    final String msg = error == null ? getString(R.string.chat_read_failed) : error;
                    runOnUiThread(() -> {
                        if (!destroyed) {
                            showFailed(msg);
                        }
                    });
                    return;
                }
            }
            savedFile = dest;
            runOnUiThread(() -> {
                if (destroyed) {
                    return;
                }
                if (fileSize <= 0) {
                    fileSize = dest.length();
                    tvSize.setText(getString(R.string.chat_viewer_size, Ui.humanSize(fileSize)));
                }
                showDone(dest);
            });
        });
    }

    /** 流式下载，只为把百分比做出来；失败由调用方回落到 FileUploader.downloadTo */
    private void streamDownloadTo(File dest) throws IOException {
        Request.Builder rb = new Request.Builder().url(absoluteUrl(url))
                .header("User-Agent", "CXIM-Android/1.0");
        String token = im.session().accessToken();
        if (token != null && !token.isEmpty()) {
            rb.header("Authorization", "Bearer " + token);
        }
        OkHttpClient client = new OkHttpClient.Builder()
                .connectTimeout(20, TimeUnit.SECONDS)
                .readTimeout(120, TimeUnit.SECONDS)
                .followRedirects(true)
                .followSslRedirects(true)
                .build();
        Call call = client.newCall(rb.build());
        inFlight = call;
        try (Response resp = call.execute()) {
            if (!resp.isSuccessful()) {
                throw new IOException("HTTP " + resp.code());
            }
            ResponseBody body = resp.body();
            if (body == null) {
                throw new IOException("empty body");
            }
            long total = body.contentLength();
            File parent = dest.getParentFile();
            if (parent != null && !parent.exists() && !parent.mkdirs()) {
                throw new IOException(getString(R.string.chat_read_failed));
            }
            long written = 0L;
            try (InputStream in = body.byteStream();
                 FileOutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) {
                    if (destroyed) {
                        throw new IOException("cancelled");
                    }
                    out.write(buf, 0, n);
                    written += n;
                    reportProgress(written, total);
                }
                out.flush();
            }
        } finally {
            inFlight = null;
        }
    }

    private void reportProgress(long written, long total) {
        if (total <= 0) {
            runOnUiThread(() -> {
                if (!destroyed) {
                    tvProgress.setText(getString(R.string.chat_viewer_progress_size,
                            Ui.humanSize(written), Ui.humanSize(Math.max(written, fileSize))));
                }
            });
            return;
        }
        int pct = (int) Math.min(100L, written * 100L / total);
        if (pct == lastShownPct) {
            return;
        }
        lastShownPct = pct;
        runOnUiThread(() -> {
            if (destroyed) {
                return;
            }
            pbDownload.setProgress(pct);
            tvProgress.setText(getString(R.string.chat_viewer_progress, pct) + "  "
                    + getString(R.string.chat_viewer_progress_size,
                    Ui.humanSize(written), Ui.humanSize(total)));
        });
    }

    /** 相对地址补成绝对地址，口径与 FileUploader.absolute() 一致 */
    private String absoluteUrl(String u) {
        if (u.startsWith("http://") || u.startsWith("https://")) {
            return u;
        }
        String base = Config.httpBaseUrl(this);
        return u.startsWith("/") ? base + u : base + "/" + u;
    }

    private File downloadTarget() {
        File dir = getExternalFilesDir("Downloads");
        if (dir == null) {
            dir = new File(getFilesDir(), "Downloads");
        }
        String name = fileName.isEmpty() ? ("file_" + System.currentTimeMillis()) : fileName;
        return new File(dir, name);
    }

    // ================================================================
    //  结果展示
    // ================================================================

    private void showDone(File file) {
        llProgress.setVisibility(View.GONE);
        tvPath.setText(file.getAbsolutePath());
        if (isImage) {
            // 解码放到后台线程：一张 4000x3000 的照片在主线程解码会卡住画面数百毫秒
            im.http().enqueue(() -> {
                final Bitmap bmp = decodePreview(file);
                runOnUiThread(() -> {
                    if (destroyed || isFinishing()) {
                        if (bmp != null) {
                            bmp.recycle();
                        }
                        return;
                    }
                    if (bmp != null) {
                        ivPreview.setImageBitmap(bmp);
                        svPreview.setVisibility(View.VISIBLE);
                        llInfo.setVisibility(View.GONE);
                        return;
                    }
                    // 解码失败退回文件卡，仍然可以给其他应用打开
                    showFileCard(getString(R.string.chat_viewer_preview_fail));
                });
            });
            return;
        }
        showFileCard(getString(R.string.chat_viewer_done));
    }

    /** 文件信息卡（非图片或图片解码失败时的兜底展示） */
    private void showFileCard(String progressText) {
        svPreview.setVisibility(View.GONE);
        llInfo.setVisibility(View.VISIBLE);
        btnOpen.setVisibility(View.VISIBLE);
        btnRetry.setVisibility(View.GONE);
        tvProgress.setText(progressText);
    }

    private void showFailed(String reason) {
        // 失败时保留进度区，让「下载失败：xxx」和重试按钮都能看见
        llProgress.setVisibility(View.VISIBLE);
        pbDownload.setProgress(0);
        svPreview.setVisibility(View.GONE);
        llInfo.setVisibility(View.VISIBLE);
        btnOpen.setVisibility(savedFile != null ? View.VISIBLE : View.GONE);
        btnRetry.setVisibility(View.VISIBLE);
        tvProgress.setText(getString(R.string.chat_viewer_failed, reason));
    }

    @Nullable
    private static Bitmap decodePreview(File file) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(file.getAbsolutePath(), bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            return null;
        }
        BitmapFactory.Options opts = new BitmapFactory.Options();
        int sample = 1;
        while (bounds.outWidth / sample > PREVIEW_MAX_PX * 2
                || bounds.outHeight / sample > PREVIEW_MAX_PX * 2) {
            sample *= 2;
        }
        opts.inSampleSize = sample;
        try {
            return BitmapFactory.decodeFile(file.getAbsolutePath(), opts);
        } catch (OutOfMemoryError e) {
            return null;
        }
    }

    // ================================================================
    //  用其他应用打开
    // ================================================================

    private void openWithOtherApp() {
        File file = savedFile == null ? downloadTarget() : savedFile;
        if (!file.exists()) {
            toast(getString(R.string.chat_viewer_failed, file.getAbsolutePath()));
            return;
        }
        try {
            Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".fileprovider", file);
            Intent view = new Intent(Intent.ACTION_VIEW);
            view.setDataAndType(uri, FileUploader.guessMime(file.getName()));
            view.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(view);
        } catch (IllegalArgumentException | ActivityNotFoundException e) {
            // 没有能打开该 MIME 的应用：提示并保留已保存路径
            toast(getString(R.string.chat_viewer_no_app));
            tvPath.setText(file.getAbsolutePath());
        }
    }

    private static String nvl(String s) {
        return s == null ? "" : s;
    }

    /** 下载目录里的文件名要防路径穿越与非法字符 */
    private static String sanitize(String name) {
        if (name == null || name.trim().isEmpty()) {
            return "";
        }
        return name.trim().replaceAll("[\\\\/:*?\"<>|\\r\\n]", "_");
    }
}
