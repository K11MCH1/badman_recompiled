package org.psprecomp.badman;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.File;
import java.util.ArrayList;
import java.util.List;

/** Runs the locally built game in its own process and presents it on a SurfaceView. */
public final class GameActivity extends Activity implements SurfaceHolder.Callback, InputManager.InputDeviceListener {
    static { System.loadLibrary("badman_android"); }

    static native boolean nativeStart(String[] arguments, String[] environment, String log);
    static native void nativeSetSurface(Object surface, int width, int height);
    static native void nativeSetPaused(boolean paused);
    static native void nativeSetInput(int buttons, int analogX, int analogY);
    static native void nativeSetSharp(boolean sharp);
    static native void nativeRequestClose();
    static native int nativeExitCode();
    static native String nativeUtilityMessage();
    static native String[] nativeOskDialog();
    static native void nativeOskRespond(long generation, boolean confirm, String text);

    // PSP controller bits (badman::Key).
    static final int SELECT = 0x0001, START = 0x0008, UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040, LEFT = 0x0080,
        L = 0x0100, R = 0x0200, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000;

    private static boolean started;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView message;
    private int keys, hat, triggers, analogX = 128, analogY = 128;
    private int touchButtons, touchX = 128, touchY = 128;
    private TouchOverlay overlay; // Null when touch controls are off.
    private InputManager inputManager;
    private float deadzone = 0.2f;
    private boolean touchControls = true, quitting, reported;
    private float refreshRate; // The rate in-between frames are drawn at; 0 when they are off.
    private long oskShown = -1, quitAt;
    private File log;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        final Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        window.getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        Settings settings = new Settings(this);
        deadzone = settings.deadzone() / 100f;
        touchControls = settings.touchControls();
        if (settings.smoothMotion()) preferFastestRefresh(window);

        FrameLayout root = new AspectLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));
        if (touchControls) {
            overlay = new TouchOverlay(this, (buttons, x, y) -> { touchButtons = buttons; touchX = x; touchY = y; publish(); });
            root.addView(overlay, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
            inputManager.registerInputDeviceListener(this, handler);
            showOverlay(!controllerConnected());
        }
        message = new TextView(this);
        message.setTextColor(Color.WHITE);
        message.setBackgroundColor(0xB0000000);
        message.setPadding(24, 12, 24, 12);
        message.setVisibility(View.GONE);
        root.addView(message, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
            ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        setContentView(root);
        hideSystemBars();

        GameFiles files = new GameFiles(this);
        log = files.gameLog();
        if (!started) {
            started = true;
            nativeSetSharp(settings.sharpScaling());
            List<String> environment = new ArrayList<>();
            String[][] pairs = {
                {"BADMAN_GAME_LIBRARY", files.gameLibrary().getPath()},
                {"BADMAN_FFMPEG_DIRECTORY", getApplicationInfo().nativeLibraryDir},
                {"BADMAN_SAVES_DIRECTORY", files.saves().getPath()},
                {"BADMAN_SKIP_INTRO", settings.skipIntro() ? "1" : "0"},
                {"BADMAN_RENDERER", settings.renderer()},
                {"BADMAN_INTERPOLATION", settings.smoothMotion() ? "1" : "0"},
                // Development: used only if this folder exists (never created by the app).
                {"BADMAN_MISMATCH_DIRECTORY", new File(files.logs(), "mismatch").getPath()},
                {"TMPDIR", getCacheDir().getPath()}};
            for (String[] pair : pairs) { environment.add(pair[0]); environment.add(pair[1]); }
            if (settings.profile()) {
                environment.add("BADMAN_PROFILE");
                environment.add(new File(files.logs(), "profile.txt").getPath());
                environment.add("BADMAN_RASTER_STATS");
                environment.add("1");
            }
            if (!settings.performanceHint()) {
                environment.add("BADMAN_PERFORMANCE_HINT");
                environment.add("0");
            }
            if (settings.presentEveryVblank()) {
                environment.add("BADMAN_PRESENT_EVERY_VBLANK");
                environment.add("1");
            }
            String[] arguments = {
                new File(getApplicationInfo().nativeLibraryDir, "libbadman_android.so").getPath(),
                files.executable().getPath(), files.dataRoot().getPath(), "18446744073709551615", "/dev/null",
                files.fonts().getPath(), files.fontModule().getPath()};
            nativeStart(arguments, environment.toArray(new String[0]), log.getPath());
        }
        handler.post(poll);
    }

    private final Runnable poll = new Runnable() {
        @Override public void run() {
            int code = nativeExitCode();
            if (code >= 0) { stopped(code); return; }
            // A game stuck outside its frame loop must not keep the player waiting.
            if (quitting && System.currentTimeMillis() - quitAt > 3000) { finish(); return; }
            String text = nativeUtilityMessage();
            message.setText(text);
            message.setVisibility(text.isEmpty() ? View.GONE : View.VISIBLE);
            String[] dialog = nativeOskDialog();
            if (dialog != null && Long.parseLong(dialog[0]) != oskShown) showKeyboard(dialog);
            handler.postDelayed(this, 100);
        }
    };

    private void stopped(int code) {
        if (reported) return;
        reported = true;
        if (code == 0 || quitting) { finish(); return; }
        new AlertDialog.Builder(this).setTitle("The game stopped")
            .setMessage(GameFiles.tail(log, 30)).setCancelable(false)
            .setPositiveButton("Close", (d, w) -> finish()).show();
    }

    private void showKeyboard(String[] dialog) {
        final long generation = Long.parseLong(dialog[0]);
        oskShown = generation;
        final EditText edit = new EditText(this);
        edit.setText(dialog[2]);
        edit.setSelection(edit.getText().length());
        edit.setFilters(new InputFilter[] {new InputFilter.LengthFilter(Integer.parseInt(dialog[3]))});
        boolean multiline = Integer.parseInt(dialog[4]) > 1;
        edit.setInputType(InputType.TYPE_CLASS_TEXT | (multiline ? InputType.TYPE_TEXT_FLAG_MULTI_LINE : 0));
        new AlertDialog.Builder(this).setTitle(dialog[1]).setView(edit).setCancelable(false)
            .setPositiveButton("OK", (d, w) -> nativeOskRespond(generation, true, edit.getText().toString()))
            .setNegativeButton("Cancel", (d, w) -> nativeOskRespond(generation, false, ""))
            .show();
    }

    /**
     * Asks for the display mode with the panel's highest refresh rate (for
     * example 144 Hz) at the current resolution while the game shows: the
     * presenter draws an in-between frame on every refresh.
     */
    private void preferFastestRefresh(Window window) {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode(), best = current;
        for (Display.Mode mode : display.getSupportedModes())
            if (mode.getPhysicalWidth() == current.getPhysicalWidth() && mode.getPhysicalHeight() == current.getPhysicalHeight()
                && mode.getRefreshRate() > best.getRefreshRate()) best = mode;
        WindowManager.LayoutParams attributes = window.getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        window.setAttributes(attributes);
        refreshRate = best.getRefreshRate();
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {}
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // The surface's own vote for that rate (Android 11+), so the system keeps the panel at it.
        if (refreshRate > 0 && Build.VERSION.SDK_INT >= 30)
            holder.getSurface().setFrameRate(refreshRate, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
        nativeSetSurface(holder.getSurface(), width, height);
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { nativeSetSurface(null, 0, 0); }

    @Override protected void onResume() { super.onResume(); nativeSetPaused(false); hideSystemBars(); }
    @Override protected void onPause() { nativeSetPaused(true); super.onPause(); }
    @Override public void onWindowFocusChanged(boolean focused) { if (focused) hideSystemBars(); }
    @Override protected void onDestroy() {
        handler.removeCallbacks(poll);
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        super.onDestroy();
        // The game runs in this activity's own process; ending it ends the game.
        if (isFinishing()) Process.killProcess(Process.myPid());
    }
    @Override public void onBackPressed() {
        nativeSetPaused(true);
        new AlertDialog.Builder(this).setMessage("Quit Badman?")
            .setPositiveButton("Quit", (d, w) -> {
                quitting = true; quitAt = System.currentTimeMillis();
                nativeSetPaused(false); nativeRequestClose();
            })
            .setNegativeButton("Keep playing", (d, w) -> nativeSetPaused(false))
            .setOnCancelListener(d -> nativeSetPaused(false)).show();
    }

    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    // Controllers and keyboards. Xbox, DualSense and most Android pads report
    // the face buttons by position: A bottom, B right, X left, Y top.
    private static int button(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_Z: return CROSS;
            case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_X: return CIRCLE;
            case KeyEvent.KEYCODE_BUTTON_X: case KeyEvent.KEYCODE_A: return SQUARE;
            case KeyEvent.KEYCODE_BUTTON_Y: case KeyEvent.KEYCODE_S: return TRIANGLE;
            case KeyEvent.KEYCODE_BUTTON_L1: case KeyEvent.KEYCODE_BUTTON_L2: case KeyEvent.KEYCODE_Q: return L;
            case KeyEvent.KEYCODE_BUTTON_R1: case KeyEvent.KEYCODE_BUTTON_R2: case KeyEvent.KEYCODE_E: return R;
            case KeyEvent.KEYCODE_BUTTON_START: case KeyEvent.KEYCODE_ENTER: return START;
            case KeyEvent.KEYCODE_BUTTON_SELECT: case KeyEvent.KEYCODE_DEL: return SELECT;
            case KeyEvent.KEYCODE_DPAD_UP: return UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return RIGHT;
            default: return 0;
        }
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (fromController(event.getSource())) showOverlay(false);
        int bit = button(event.getKeyCode());
        if (bit == 0) return super.dispatchKeyEvent(event);
        if (event.getAction() == KeyEvent.ACTION_DOWN) keys |= bit;
        else if (event.getAction() == KeyEvent.ACTION_UP) keys &= ~bit;
        publish();
        return true;
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
            || event.getAction() != MotionEvent.ACTION_MOVE) return super.dispatchGenericMotionEvent(event);
        showOverlay(false);
        float hx = event.getAxisValue(MotionEvent.AXIS_HAT_X), hy = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        hat = (hx < -0.5f ? LEFT : 0) | (hx > 0.5f ? RIGHT : 0) | (hy < -0.5f ? UP : 0) | (hy > 0.5f ? DOWN : 0);
        float lt = Math.max(event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_BRAKE));
        float rt = Math.max(event.getAxisValue(MotionEvent.AXIS_RTRIGGER), event.getAxisValue(MotionEvent.AXIS_GAS));
        triggers = (lt > 0.5f ? L : 0) | (rt > 0.5f ? R : 0);
        analogX = axis(event.getAxisValue(MotionEvent.AXIS_X));
        analogY = axis(event.getAxisValue(MotionEvent.AXIS_Y)); // Both grow down/right, like the PSP nub.
        publish();
        return true;
    }
    private int axis(float value) {
        float magnitude = Math.abs(value);
        if (magnitude <= deadzone) return 128;
        float scaled = Math.min(1f, (magnitude - deadzone) / (1f - deadzone));
        return Math.max(0, Math.min(255, Math.round(128 + Math.signum(value) * scaled * 127)));
    }

    // The on-screen controls show only while no controller is connected:
    // Xbox, DualSense, Steam and other pads report gamepad or joystick input.
    private static boolean fromController(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }
    private static boolean controllerConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && !device.isVirtual() && fromController(device.getSources())) return true;
        }
        return false;
    }
    private void showOverlay(boolean show) {
        if (overlay == null || (overlay.getVisibility() == View.VISIBLE) == show) return;
        if (!show) overlay.reset(); // Nothing stays held when the pad takes over.
        overlay.setVisibility(show ? View.VISIBLE : View.GONE);
    }
    @Override public void onInputDeviceAdded(int id) { showOverlay(!controllerConnected()); }
    @Override public void onInputDeviceRemoved(int id) { showOverlay(!controllerConnected()); }
    @Override public void onInputDeviceChanged(int id) { showOverlay(!controllerConnected()); }

    private void publish() {
        // The on-screen stick, while held off centre, moves the nub; otherwise the controller's stick does.
        boolean stick = touchX != 128 || touchY != 128;
        nativeSetInput(keys | hat | triggers | touchButtons, stick ? touchX : analogX, stick ? touchY : analogY);
    }

    /** Keeps the game picture at the PSP's 480:272 shape, centred, with black bars. */
    static final class AspectLayout extends FrameLayout {
        AspectLayout(Context context) { super(context); }
        @Override protected void onMeasure(int widthSpec, int heightSpec) {
            super.onMeasure(widthSpec, heightSpec);
            int width = getMeasuredWidth(), height = getMeasuredHeight();
            int w = width, h = width * 272 / 480;
            if (h > height) { h = height; w = height * 480 / 272; }
            getChildAt(0).measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY),
                MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY));
        }
    }
}
