package org.psprecomp.badman;

import android.content.Context;
import android.content.SharedPreferences;
import java.util.Arrays;
import org.json.JSONException;
import org.json.JSONObject;

/** Player options kept between sessions (the Android side of launcher.ini). */
final class Settings {
    private final SharedPreferences preferences;

    Settings(Context context) { preferences = context.getSharedPreferences("settings", Context.MODE_PRIVATE); }

    boolean skipIntro() { return preferences.getBoolean("skip_intro", false); }
    boolean sharpScaling() { return preferences.getBoolean("sharp_scaling", true); }
    /**
     * In-between frames on every display refresh (up to the panel's highest rate).
     * Off by default: on the test phone, presenting at the full display rate made
     * the system cap every CPU core, which halved the game's own frame rate.
     */
    boolean smoothMotion() { return preferences.getBoolean("smooth_motion", false); }
    boolean touchControls() { return preferences.getBoolean("touch_controls", true); }
    int deadzone() { return preferences.getInt("stick_deadzone", 20); }
    /**
     * "cpu" batches sprites and draws them on the phone's spare cores (the
     * default); "software" draws each primitive on the game thread; "vulkan"
     * runs the same batches on the GPU.
     */
    String renderer() { return preferences.getString("renderer", "cpu"); }
    /** Development only: sample the game thread into logs/profile.txt (set through adb). */
    boolean profile() { return preferences.getBoolean("profile", false); }
    /** Development only: redraw every vblank (about 60 presents a second). */
    boolean presentEveryVblank() { return preferences.getBoolean("present_every_vblank", false); }
    /** Development only: false leaves CPU clocks to the system (no performance hint session). */
    boolean performanceHint() { return preferences.getBoolean("performance_hint", true); }

    void set(String key, boolean value) { preferences.edit().putBoolean(key, value).apply(); }

    /** The ISO the player chose (a document URI with a persisted read grant), its file name, and whether it is prepared. */
    String isoUri() { return preferences.getString("iso_uri", ""); }
    String isoName() { return preferences.getString("iso_name", ""); }
    boolean isoPrepared() { return preferences.getBoolean("iso_prepared", false); }
    void setIso(String uri, String name) {
        preferences.edit().putString("iso_uri", uri).putString("iso_name", name).putBoolean("iso_prepared", false).apply();
    }
    void setIsoPrepared() { set("iso_prepared", true); }

    // The launcher page's settings (keys and values as in launcher.js), mapped
    // onto the preferences above. Only options the game honours on Android.
    private static final String[] RENDERERS = {"cpu", "vulkan", "software"};

    JSONObject launcherSettings() {
        JSONObject result = new JSONObject();
        try {
            result.put("SkipIntro", skipIntro() ? "1" : "0");
            result.put("Filter", sharpScaling() ? "nearest" : "smooth");
            result.put("Interpolation", smoothMotion() ? "1" : "0");
            result.put("AndroidRenderer", renderer());
            result.put("AndroidTouch", touchControls() ? "1" : "0");
            result.put("StickDeadzone", String.valueOf(deadzone()));
        } catch (JSONException error) {
            throw new IllegalStateException(error);
        }
        return result;
    }

    /** Applies one launcher setting; false when the key or value is not supported here. */
    boolean setFromLauncher(String key, String value) {
        boolean flag = "1".equals(value);
        boolean valid = "1".equals(value) || "0".equals(value);
        switch (key) {
            case "SkipIntro": if (valid) set("skip_intro", flag); return valid;
            case "Interpolation": if (valid) set("smooth_motion", flag); return valid;
            case "AndroidTouch": if (valid) set("touch_controls", flag); return valid;
            case "Filter":
                if (!"nearest".equals(value) && !"smooth".equals(value)) return false;
                set("sharp_scaling", "nearest".equals(value));
                return true;
            case "AndroidRenderer":
                if (!Arrays.asList(RENDERERS).contains(value)) return false;
                preferences.edit().putString("renderer", value).apply();
                return true;
            case "StickDeadzone":
                try {
                    int percent = Integer.parseInt(value);
                    if (percent < 0 || percent > 50 || percent % 5 != 0) return false;
                    preferences.edit().putInt("stick_deadzone", percent).apply();
                    return true;
                } catch (NumberFormatException error) {
                    return false;
                }
            default: return false;
        }
    }
}
