package org.psprecomp.badman;

import android.content.Context;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import org.json.JSONException;
import org.json.JSONObject;

/** Where the player's prepared game lives inside the app's private storage. */
final class GameFiles {
    static final String SAVES_OWNER = "PSPRecomp Badman player storage\nschema=1\n";
    private final Context context;
    private final File root;

    GameFiles(Context context) {
        this.context = context;
        root = context.getFilesDir();
    }
    /** Extracted PSP_GAME, prepared font module and Saves (the native data root). */
    File dataRoot() { return new File(root, "game"); }
    File executable() { return new File(dataRoot(), "PSP_GAME/SYSDIR/EBOOT_DECRYPTED.ELF"); }
    File fontModule() { return new File(dataRoot(), "prepared/libfont-fixed.elf"); }
    File saves() { return new File(dataRoot(), "Saves"); }
    /** The translation compiled on this phone from the player's ISO, and its working files. */
    File buildDirectory() { return new File(root, "build"); }
    File gameLibrary() { return new File(buildDirectory(), "libbadman_game.so"); }
    File stamp() { return new File(buildDirectory(), "build.json"); }
    /** Temporary space for the ISO copy and extraction; removed after every preparation. */
    File setup() { return new File(root, "setup"); }
    File fonts() { return new File(root, "fonts"); }
    File logs() {
        File logs = new File(root, "logs");
        logs.mkdirs();
        return logs;
    }
    File gameLog() { return new File(logs(), "game.log"); }
    /** Launcher background art made from the player's own game (as BadmanRecomp/art on Windows); none yet. */
    File art() { return new File(root, "art"); }

    /** Identities the preparation checks (from the profile's config at APK build time). */
    JSONObject facts() throws IOException {
        try (InputStream input = context.getAssets().open("profile.json")) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            copy(input, bytes);
            return new JSONObject(bytes.toString("UTF-8"));
        } catch (JSONException error) {
            throw new IOException("The packaged profile facts are damaged", error);
        }
    }

    /** True when files from an earlier preparation are present (a rebuild can reuse them). */
    boolean prepared() { return executable().isFile() && fontModule().isFile(); }

    /** What is still missing before Play, or null when everything is in place. */
    String missing() {
        if (!executable().isFile()) return "the game has not been prepared from your ISO yet";
        if (!fontModule().isFile()) return "the prepared font module is missing";
        if (!gameLibrary().isFile()) return "the game has not been compiled on this phone yet";
        if (!runtimeIdentity().equals(Toolchain.read(stamp()).trim()))
            return "this version of the app needs the game compiled again";
        return null;
    }

    /**
     * The interface a game library is compiled against (framework headers, entry
     * source, build recipe; hashed at APK build time). A new app version with the
     * same interface keeps the game library; a changed one needs a rebuild.
     */
    String runtimeIdentity() {
        try {
            return "game_interface=" + facts().getString("game_interface");
        } catch (Exception error) {
            return "interface unavailable";
        }
    }
    void writeStamp() throws IOException { Toolchain.write(stamp(), runtimeIdentity() + "\n"); }

    /** Copies the open-licence replacement PGF fonts out of the APK once. */
    void installFonts() throws IOException {
        File directory = fonts();
        directory.mkdirs();
        for (String name : context.getAssets().list("fonts")) {
            File target = new File(directory, name);
            if (target.isFile()) continue; // Pinned fonts; a complete copy is never replaced.
            try (InputStream input = context.getAssets().open("fonts/" + name)) {
                File temporary = new File(directory, name + ".partial");
                try (OutputStream output = new FileOutputStream(temporary)) { copy(input, output); }
                if (!temporary.renameTo(target)) throw new IOException("Cannot install font " + name);
            }
        }
    }

    /** Creates the Saves folder with its ownership marker the first time; never adopts an existing folder. */
    void createSaves() throws IOException {
        File saves = saves();
        if (saves.exists()) return;
        if (!saves.mkdirs()) throw new IOException("Cannot create the Saves folder");
        try (OutputStream output = new FileOutputStream(new File(saves, ".badman-saves-owner"))) {
            output.write(SAVES_OWNER.getBytes(StandardCharsets.UTF_8));
        }
    }

    static void copy(InputStream input, OutputStream output) throws IOException {
        byte[] buffer = new byte[1 << 16];
        for (int count; (count = input.read(buffer)) > 0; ) output.write(buffer, 0, count);
    }

    static String sha256(File file) throws IOException {
        try (InputStream input = new FileInputStream(file)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[1 << 16];
            for (int count; (count = input.read(buffer)) > 0; ) digest.update(buffer, 0, count);
            return hex(digest.digest());
        } catch (NoSuchAlgorithmException error) {
            throw new IOException(error);
        }
    }

    static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder();
        for (byte b : bytes) text.append(String.format("%02x", b & 0xFF));
        return text.toString();
    }

    /** Removes an app-private folder; symbolic links are removed, never followed. */
    static void deleteTree(File file) {
        File[] children = isLink(file) ? null : file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }

    private static boolean isLink(File file) {
        try {
            return java.nio.file.Files.isSymbolicLink(file.toPath());
        } catch (Exception error) {
            return false;
        }
    }

    /** The last lines of a log, for an error dialog. */
    static String tail(File file, int lines) {
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            long start = Math.max(0, input.length() - 8192);
            byte[] bytes = new byte[(int) (input.length() - start)];
            input.seek(start);
            input.readFully(bytes);
            String[] all = new String(bytes, StandardCharsets.UTF_8).split("\n");
            StringBuilder result = new StringBuilder();
            for (int i = Math.max(0, all.length - lines); i < all.length; ++i) result.append(all[i]).append('\n');
            return result.toString().trim();
        } catch (IOException error) {
            return "No log is available (" + error.getMessage() + ").";
        }
    }
}
