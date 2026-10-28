package org.psprecomp.badman;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.SystemClock;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen controls drawn over the game: a left analog stick for the cursor
 * and menus (it re-centres where the thumb lands in the left third), the four face
 * buttons on the right, L and R in the top corners and Select/Start at the top
 * centre. It reports a PSP button mask and analog position (0-255, 128 at
 * rest) to its listener. The activity hides it while a controller is connected.
 */
final class TouchOverlay extends View {
    interface Listener { void touchChanged(int buttons, int analogX, int analogY); }

    private static final int STICK = -1;
    // The game reads input once per frame (30 a second): a quicker tap stays
    // held this long so it never falls between two reads.
    private static final long MINIMUM_PRESS_MS = 70;
    private static final class Control {
        final int bit;
        final String label;
        final RectF bounds = new RectF();
        final boolean round;
        Control(int bit, String label, boolean round) { this.bit = bit; this.label = label; this.round = round; }
    }

    private final Listener listener;
    private final Control[] controls = {
        new Control(GameActivity.TRIANGLE, "triangle", true), new Control(GameActivity.CIRCLE, "circle", true),
        new Control(GameActivity.CROSS, "cross", true), new Control(GameActivity.SQUARE, "square", true),
        new Control(GameActivity.L, "L", false), new Control(GameActivity.R, "R", false),
        new Control(GameActivity.SELECT, "SELECT", false), new Control(GameActivity.START, "START", false)};
    // Pointer id -> what it holds: a Control, or STICK.
    private final SparseArray<Object> held = new SparseArray<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
        text = new Paint(Paint.ANTI_ALIAS_FLAG), mark = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path shape = new Path();
    private float stickRadius, stickX, stickY, restX, restY, knobX, knobY;
    private int buttons, analogX = 128, analogY = 128;
    private int fingers, latched; // Bits held by fingers, and quick taps still being held.
    private final long[] pressedAt = new long[controls.length];
    // Menus read only the d-pad, so the stick also presses the d-pad when pushed
    // most of the way along one axis (released a little lower, so it never
    // flickers). In a level this changes nothing: the game's input helper
    // (0x0882F978) turns d-pad bits into a full nub push, clamped to the nub's
    // range, and already reports a nub beyond its dead zone as d-pad bits.
    private static final float DPAD_PRESS = 0.8f, DPAD_RELEASE = 0.65f;
    private int stickDpad;

    TouchOverlay(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        stroke.setStyle(Paint.Style.STROKE);
        mark.setStyle(Paint.Style.STROKE);
        mark.setStrokeCap(Paint.Cap.ROUND);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
    }

    @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        float unit = Math.min(height, width * 0.5f);
        stickRadius = unit * 0.15f;
        restX = Math.max(stickRadius * 1.25f, width * 0.11f);
        restY = height - Math.max(stickRadius * 1.35f, height * 0.28f);
        stickX = knobX = restX; stickY = knobY = restY;
        // Face buttons: a diamond whose centre mirrors the stick's.
        float button = unit * 0.07f, spread = unit * 0.125f, cx = width - restX, cy = restY - unit * 0.05f;
        place(controls[0], cx, cy - spread, button);
        place(controls[1], cx + spread, cy, button);
        place(controls[2], cx, cy + spread, button);
        place(controls[3], cx - spread, cy, button);
        // Shoulders in the top corners, Select/Start as small pills at the top centre.
        float shoulderW = unit * 0.22f, shoulderH = unit * 0.09f, margin = unit * 0.035f;
        controls[4].bounds.set(margin, margin, margin + shoulderW, margin + shoulderH);
        controls[5].bounds.set(width - margin - shoulderW, margin, width - margin, margin + shoulderH);
        float pillW = unit * 0.15f, pillH = unit * 0.06f, gap = unit * 0.03f;
        controls[6].bounds.set(width / 2f - gap / 2 - pillW, margin, width / 2f - gap / 2, margin + pillH);
        controls[7].bounds.set(width / 2f + gap / 2, margin, width / 2f + gap / 2 + pillW, margin + pillH);
        stroke.setStrokeWidth(Math.max(2f, unit * 0.006f));
        mark.setStrokeWidth(stroke.getStrokeWidth() * 1.6f);
    }
    private static void place(Control control, float x, float y, float radius) {
        control.bounds.set(x - radius, y - radius, x + radius, y + radius);
    }

    /** Releases everything (for example when a controller takes over). */
    void reset() {
        held.clear();
        fingers = latched = stickDpad = 0;
        stickX = knobX = restX; stickY = knobY = restY;
        update(0, 128, 128);
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked(), index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            float x = event.getX(index), y = event.getY(index);
            Control control = hit(x, y);
            if (control != null) held.put(event.getPointerId(index), control);
            else if (x < getWidth() / 3f && indexOfStick() < 0) {
                held.put(event.getPointerId(index), STICK);
                stickX = knobX = clamp(x, stickRadius, getWidth() / 3f);
                stickY = knobY = clamp(y, stickRadius, getHeight() - stickRadius);
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            Object released = held.get(event.getPointerId(index));
            held.remove(event.getPointerId(index));
            if (released != null && released.equals(STICK)) { stickX = knobX = restX; stickY = knobY = restY; }
        } else if (action == MotionEvent.ACTION_CANCEL) {
            reset();
            return true;
        }
        int bits = 0, x = 128, y = 128;
        boolean stickHeld = false;
        for (int i = 0; i < event.getPointerCount(); ++i) {
            int id = event.getPointerId(i);
            if (action == MotionEvent.ACTION_POINTER_UP && i == index) continue;
            Object what = held.get(id);
            if (what == null) continue;
            if (what.equals(STICK)) {
                float dx = event.getX(i) - stickX, dy = event.getY(i) - stickY, length = (float) Math.hypot(dx, dy);
                if (length > stickRadius) { dx *= stickRadius / length; dy *= stickRadius / length; }
                knobX = stickX + dx; knobY = stickY + dy;
                // Both axes grow right/down, like the PSP nub.
                x = Math.round(128 + 127 * dx / stickRadius);
                y = Math.round(128 + 127 * dy / stickRadius);
                stickDpad = dpad(dx / stickRadius, dy / stickRadius, stickDpad);
                stickHeld = true;
            } else {
                // A finger may slide from one button to the next.
                Control now = hit(event.getX(i), event.getY(i));
                if (now != null && now != what) held.put(id, what = now);
                bits |= ((Control) what).bit;
            }
        }
        long now = SystemClock.uptimeMillis();
        for (int i = 0; i < controls.length; ++i) {
            final int bit = controls[i].bit;
            if ((bits & bit) != 0 && (fingers & bit) == 0) pressedAt[i] = now;
            long remaining = MINIMUM_PRESS_MS - (now - pressedAt[i]);
            if ((bits & bit) == 0 && (fingers & bit) != 0 && remaining > 0) {
                latched |= bit;
                postDelayed(() -> { latched &= ~bit; update(fingers | latched | stickDpad, analogX, analogY); }, remaining);
            }
        }
        fingers = bits;
        if (!stickHeld) stickDpad = 0;
        update(fingers | latched | stickDpad, x, y);
        return true;
    }
    /** The d-pad directions a stick position (-1..1, growing right/down) presses, given those pressed before. */
    static int dpad(float x, float y, int before) {
        return axis(x, before, GameActivity.LEFT, GameActivity.RIGHT) | axis(y, before, GameActivity.UP, GameActivity.DOWN);
    }
    private static int axis(float value, int before, int negative, int positive) {
        if (value <= -((before & negative) != 0 ? DPAD_RELEASE : DPAD_PRESS)) return negative;
        if (value >= ((before & positive) != 0 ? DPAD_RELEASE : DPAD_PRESS)) return positive;
        return 0;
    }
    private int indexOfStick() {
        for (int i = 0; i < held.size(); ++i) if (held.valueAt(i).equals(STICK)) return i;
        return -1;
    }
    private Control hit(float x, float y) {
        // Generous targets: a little outside each drawn control still counts.
        float slack = stickRadius * 0.25f;
        for (Control control : controls) {
            RectF b = control.bounds;
            if (x >= b.left - slack && x <= b.right + slack && y >= b.top - slack && y <= b.bottom + slack) return control;
        }
        return null;
    }
    private static float clamp(float value, float low, float high) { return Math.max(low, Math.min(high, value)); }
    private void update(int bits, int x, int y) {
        x = Math.max(0, Math.min(255, x)); y = Math.max(0, Math.min(255, y));
        boolean changed = bits != buttons || x != analogX || y != analogY;
        buttons = bits; analogX = x; analogY = y;
        if (changed) listener.touchChanged(buttons, analogX, analogY);
        invalidate();
    }

    @Override protected void onDraw(Canvas canvas) {
        // Stick: base ring and knob.
        fill.setColor(0x30FFFFFF); stroke.setColor(0x80FFFFFF);
        canvas.drawCircle(stickX, stickY, stickRadius, fill);
        canvas.drawCircle(stickX, stickY, stickRadius, stroke);
        fill.setColor(indexOfStick() >= 0 ? 0x90FFFFFF : 0x60FFFFFF);
        canvas.drawCircle(knobX, knobY, stickRadius * 0.45f, fill);
        for (Control control : controls) {
            boolean pressed = (buttons & control.bit) != 0;
            RectF b = control.bounds;
            fill.setColor(pressed ? 0x90FFFFFF : 0x30000000);
            stroke.setColor(pressed ? 0xFFFFFFFF : 0xA0FFFFFF);
            if (control.round) {
                canvas.drawCircle(b.centerX(), b.centerY(), b.width() / 2, fill);
                canvas.drawCircle(b.centerX(), b.centerY(), b.width() / 2, stroke);
                symbol(canvas, control, pressed);
            } else {
                float corner = b.height() / 2;
                canvas.drawRoundRect(b, corner, corner, fill);
                canvas.drawRoundRect(b, corner, corner, stroke);
                text.setColor(pressed ? 0xFF202020 : 0xE0FFFFFF);
                text.setTextSize(b.height() * (control.label.length() > 1 ? 0.42f : 0.6f));
                canvas.drawText(control.label, b.centerX(), b.centerY() - (text.descent() + text.ascent()) / 2, text);
            }
        }
    }
    // The PSP face symbols in their usual colours.
    private void symbol(Canvas canvas, Control control, boolean pressed) {
        RectF b = control.bounds;
        float cx = b.centerX(), cy = b.centerY(), r = b.width() * 0.24f;
        switch (control.label) {
            case "triangle":
                mark.setColor(pressed ? 0xFF1E8C6E : 0xE040D0A0);
                shape.reset();
                shape.moveTo(cx, cy - r); shape.lineTo(cx + r * 0.95f, cy + r * 0.7f); shape.lineTo(cx - r * 0.95f, cy + r * 0.7f); shape.close();
                canvas.drawPath(shape, mark);
                break;
            case "circle":
                mark.setColor(pressed ? 0xFFC03040 : 0xE0FF6070);
                canvas.drawCircle(cx, cy, r, mark);
                break;
            case "cross":
                mark.setColor(pressed ? 0xFF3050C0 : 0xE080A0FF);
                canvas.drawLine(cx - r, cy - r, cx + r, cy + r, mark);
                canvas.drawLine(cx - r, cy + r, cx + r, cy - r, mark);
                break;
            default:
                mark.setColor(pressed ? 0xFFB040A0 : 0xE0FF90E0);
                canvas.drawRect(cx - r * 0.85f, cy - r * 0.85f, cx + r * 0.85f, cy + r * 0.85f, mark);
                break;
        }
    }
}
