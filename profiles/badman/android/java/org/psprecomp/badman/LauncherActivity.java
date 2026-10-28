package org.psprecomp.badman;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.provider.OpenableColumns;
import android.webkit.JavascriptInterface;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Collections;
import java.util.Locale;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * Entry screen: the Windows launcher's own page (launcher-ui, packaged as
 * assets/launcher) in a WebView, laid out for portrait. This class is its host,
 * as launcher.cpp is on Windows: it keeps the settings, opens the ISO picker,
 * runs the preparation service and starts Play. The page only renders state and
 * sends the player's choices over the same messages.
 */
public final class LauncherActivity extends Activity {
    static final String NAME = "Badman Recompiled";
    private static final int PICK_ISO = 1;
    private static final long NEEDED_SPACE = 1200L << 20; // ISO copy, extraction, translation, objects, compiler
    // The page and the player's own launcher art, under fixed origins (WebView2 maps art.local the same way).
    private static final String PAGE = "launcher.local", ART = "art.local";
    private final Handler handler = new Handler(Looper.getMainLooper());
    private Settings settings;
    private WebView web;
    private boolean pageReady, busyShown;
    private String lastProgress = "";
    private float lastFraction = -1;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        settings = new Settings(this);
        getWindow().setStatusBarColor(Color.rgb(4, 46, 71));        // The page's --deep.
        getWindow().setNavigationBarColor(Color.rgb(61, 150, 202)); // --sky, under the tab row.
        web = new WebView(this);
        web.setBackgroundColor(Color.rgb(15, 65, 98)); // --navy until the page draws.
        WebSettings options = web.getSettings();
        options.setJavaScriptEnabled(true);
        options.setAllowFileAccess(false);
        options.setAllowContentAccess(false);
        options.setTextZoom(100); // The page sizes its own type from the screen.
        web.addJavascriptInterface(new Bridge(), "BadmanAndroid");
        web.setWebViewClient(new WebViewClient() {
            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                return serve(request.getUrl());
            }
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                return true; // The launcher never leaves its page.
            }
        });
        setContentView(web);
        web.loadUrl("https://" + PAGE + "/index.html");
        if (getIntent().getBooleanExtra("play", false) && new GameFiles(this).missing() == null) play();
    }

    @Override protected void onResume() { super.onResume(); refresh(); }
    @Override protected void onPause() { handler.removeCallbacks(poll); super.onPause(); }
    @Override protected void onDestroy() { web.destroy(); super.onDestroy(); }

    @Override public void onBackPressed() {
        // The page closes an expanded log or returns to Play first.
        web.evaluateJavascript("window.badmanBack ? window.badmanBack() : false", handled -> {
            if (!"true".equals(handled)) finish();
        });
    }

    // ---------------------------------------------------------------- page files
    /** Only the packaged page and the art folder; everything else (any other host) is not found. */
    private WebResourceResponse serve(Uri url) {
        String path = url.getPath() == null ? "" : url.getPath().replaceFirst("^/", "");
        try {
            if (PAGE.equals(url.getHost()) && !path.contains("..")) return file(path, getAssets().open("launcher/" + path));
            if (ART.equals(url.getHost()) && path.matches("[a-z]+\\.png")) {
                File art = new File(new GameFiles(this).art(), path);
                if (art.isFile()) return file(path, new FileInputStream(art));
            }
        } catch (IOException missing) {
            // Answered as not found below.
        }
        return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found", Collections.emptyMap(),
            new ByteArrayInputStream(new byte[0]));
    }
    private static WebResourceResponse file(String path, InputStream body) {
        String type = path.endsWith(".html") ? "text/html" : path.endsWith(".css") ? "text/css"
            : path.endsWith(".js") ? "text/javascript" : path.endsWith(".svg") ? "image/svg+xml"
            : path.endsWith(".ttf") ? "font/ttf" : path.endsWith(".png") ? "image/png" : "application/octet-stream";
        return new WebResourceResponse(type, type.startsWith("text/") ? "utf-8" : null, body);
    }

    // ---------------------------------------------------------------- messages
    private final class Bridge {
        @JavascriptInterface public void postMessage(String text) {
            handler.post(() -> {
                try {
                    receive(new JSONObject(text));
                } catch (JSONException ignored) {
                    // Not a launcher message.
                }
            });
        }
    }

    private void receive(JSONObject message) {
        switch (message.optString("type")) {
            case "ready":
                pageReady = true;
                busyShown = false;
                sendInit();
                refresh();
                break;
            case "set":
                if (BuildService.status.running) {
                    toast("Settings can be changed after the current job finishes.");
                    sendSettings();
                } else if (!settings.setFromLauncher(message.optString("key"), message.optString("value"))) {
                    toast("That option value is not supported.");
                    sendSettings();
                }
                break;
            case "browse":
                if (BuildService.status.running) toast("Please wait for the current job to finish.");
                else if ("iso".equals(message.optString("target"))) chooseIso();
                break;
            case "prepare": prepare(false); break;
            case "reprepare": prepare(true); break;
            case "cancel":
                if (BuildService.status.running) startService(new Intent(this, BuildService.class).setAction("cancel"));
                else toast("Nothing is being prepared.");
                break;
            case "verify": verify(); break;
            case "play": play(); break;
            default: break;
        }
    }

    private void send(String type, Object... fields) {
        if (!pageReady) return;
        JSONObject message = new JSONObject();
        try {
            message.put("type", type);
            for (int i = 0; i + 1 < fields.length; i += 2) message.put((String) fields[i], fields[i + 1]);
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
        web.evaluateJavascript("window.badmanReceive(" + message + ")", null);
    }
    private void toast(String text) { send("toast", "text", text); }
    private void status(String text, String kind) { send("status", "text", text, "kind", kind); }

    private JSONObject launcherSettings() {
        JSONObject values = settings.launcherSettings();
        try {
            values.put("ISO", settings.isoName());
            long free = new StatFs(getFilesDir().getPath()).getAvailableBytes() >> 20;
            values.put("DataDirectory", "This phone, " + (free >= 1024 ? String.format(Locale.ROOT, "%.1f GB", free / 1024.0) : free + " MB") + " free");
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
        return values;
    }
    private void sendSettings() { send("settings", "settings", launcherSettings()); }
    private void sendInit() {
        GameFiles files = new GameFiles(this);
        String missing = files.missing();
        String text = missing == null ? "Your game is prepared. Press Play to launch."
            : !files.prepared() ? "Choose your ISO, then Prepare and build."
            : "Not ready: " + missing + ". Press Prepare and build.";
        send("init", "platform", "android", "name", NAME, "art", "https://" + ART + "/", "settings", launcherSettings(),
            "status", text, "statusKind", missing == null ? "ok" : "", "progress", missing == null ? 1.0 : 0.0);
    }

    // ---------------------------------------------------------------- preparation
    private final Runnable poll = this::refresh;

    /** Mirrors the preparation service into the page: busy, the step and progress bar, then the result. */
    private void refresh() {
        handler.removeCallbacks(poll);
        if (!pageReady) return;
        BuildService.Status build = BuildService.status;
        if (build.running) {
            if (!busyShown) {
                busyShown = true;
                lastProgress = "";
                lastFraction = -1;
                send("busy", "busy", true, "play", false, "clear", true);
            }
            String line = build.detail.isEmpty() ? build.stage : build.stage + ": " + build.detail;
            if (!line.isEmpty() && !line.equals(lastProgress)) {
                lastProgress = line;
                status(line, "");
            }
            if (build.progress != lastFraction) {
                lastFraction = build.progress;
                send("progress", "value", (double) build.progress, "active", true);
            }
            handler.postDelayed(poll, 250);
            return;
        }
        if (build.finished) {
            build.finished = false;
            if (!build.failed && build.fromIso) settings.setIsoPrepared();
            send("progress", "value", build.failed ? (double) build.progress : 1.0, "active", false);
            send("finished", "code", build.failed ? 1 : 0, "play", false);
            if (build.failed) status(build.error, "bad");
            sendSettings();
        }
        busyShown = false;
    }

    private void chooseIso() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        startActivityForResult(pick, PICK_ISO);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ISO || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri iso = data.getData();
        try {
            // Kept readable for a later Prepare and build, as the Windows launcher keeps the path.
            getContentResolver().takePersistableUriPermission(iso, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException error) {
            // The provider offers no lasting grant; the ISO stays readable until the app closes.
        }
        settings.setIso(iso.toString(), displayName(iso));
        sendSettings();
        status("ISO chosen. Press Prepare and build.", "");
    }
    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && cursor.getString(0) != null) return cursor.getString(0);
        } catch (RuntimeException error) {
            // Fall back to the URI's last part.
        }
        String last = uri.getLastPathSegment();
        return last == null ? "Your ISO" : last;
    }

    /** again: prepare from the ISO even when the game is already prepared. */
    private void prepare(boolean again) {
        if (BuildService.status.running) { toast("Please wait for the current job to finish."); return; }
        GameFiles files = new GameFiles(this);
        Uri iso = settings.isoUri().isEmpty() ? null : Uri.parse(settings.isoUri());
        boolean newIso = iso != null && !settings.isoPrepared();
        if (again || !files.prepared() || newIso) {
            if (iso == null) {
                toast("Choose your game ISO first.");
                status("Choose your ISO, then Prepare and build.", "bad");
                return;
            }
            startBuild(iso);
        } else if (files.missing() != null) {
            startBuild(null); // Prepared files are in place: compile them again for this app version.
        } else {
            status("Your game is already prepared and compiled. Press Play to launch.", "ok");
        }
    }

    private void startBuild(Uri iso) {
        long free = new StatFs(getFilesDir().getPath()).getAvailableBytes();
        if (free < NEEDED_SPACE) {
            status("Preparing the game needs about " + (NEEDED_SPACE >> 20) + " MB free; " + (free >> 20) + " MB are free.", "bad");
            return;
        }
        if (Build.VERSION.SDK_INT >= 33) requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, 0);
        Intent intent = new Intent(this, BuildService.class);
        if (iso != null) intent.putExtra(BuildService.EXTRA_ISO, iso.toString()).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startForegroundService(intent);
        handler.postDelayed(poll, 250);
    }

    private void verify() {
        String missing = new GameFiles(this).missing();
        send("progress", "value", missing == null ? 1.0 : 0.0, "active", false);
        if (missing == null) status("Local build verified. Press Play to launch.", "ok");
        else status("Not ready: " + missing + ".", "bad");
    }

    private void play() {
        if (BuildService.status.running) { toast("Please wait for the current job to finish."); return; }
        GameFiles files = new GameFiles(this);
        String missing = files.missing();
        if (missing != null) {
            status("Not ready: " + missing + ". Press Prepare and build.", "bad");
            return;
        }
        try {
            files.installFonts();
            files.createSaves();
        } catch (IOException error) {
            status("Cannot start: " + error.getMessage(), "bad");
            return;
        }
        startActivity(new Intent(this, GameActivity.class));
    }
}
