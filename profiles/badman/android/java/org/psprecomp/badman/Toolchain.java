package org.psprecomp.badman;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.system.ErrnoException;
import android.system.Os;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Iterator;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * The phone-side compiler: Clang and LLD run from the APK's read-only library
 * folder (Android refuses to run programs from writable storage); the headers,
 * link stubs and sources they need are unpacked once per app version.
 */
final class Toolchain {
    final File root, aliases, sysroot, resource, include, builtins, unwind, entry;
    final String target, triple;
    final int api;
    final String libraries;

    private Toolchain(Context context, File root, JSONObject description) throws JSONException {
        this.root = root;
        aliases = new File(root, "aliases");
        sysroot = new File(root, "sysroot");
        resource = new File(root, "resource");
        include = new File(root, "include");
        builtins = new File(root, description.getString("builtins"));
        unwind = new File(root, description.getString("unwind"));
        entry = new File(root, "source/game_entry.cpp");
        target = description.getString("target");
        triple = description.getString("triple");
        api = description.getInt("api");
        libraries = context.getApplicationInfo().nativeLibraryDir;
    }

    File library(String name) { return new File(libraries, name); }
    File apiLibraries() { return new File(sysroot, "usr/lib/" + triple + "/" + api); }
    /** The compiler's libraries first (Termux's libc++ under its own name), then the app's. */
    String compilerLibraryPath() { return aliases.getPath() + ":" + libraries; }

    static Toolchain prepare(Context context) throws IOException {
        File root = new File(context.getFilesDir(), "toolchain");
        File stamp = new File(root, ".unpacked");
        String version;
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            version = info.lastUpdateTime + "/" + info.getLongVersionCode();
        } catch (Exception error) {
            throw new IOException("Cannot read the app version", error);
        }
        if (!version.equals(read(stamp))) {
            GameFiles.deleteTree(root);
            unpack(context, root);
            write(stamp, version);
        }
        try {
            Toolchain toolchain = new Toolchain(context, root, new JSONObject(read(new File(root, "toolchain.json"))));
            toolchain.link(new JSONObject(read(new File(root, "toolchain.json"))).getJSONObject("aliases"));
            return toolchain;
        } catch (JSONException error) {
            throw new IOException("The packaged compiler description is damaged", error);
        }
    }

    /** Library names the compiler asks for, pointing at the files the APK holds them under. */
    private void link(JSONObject names) throws IOException, JSONException {
        GameFiles.deleteTree(aliases);
        if (!aliases.mkdirs()) throw new IOException("Cannot create the compiler library folder");
        for (Iterator<String> it = names.keys(); it.hasNext(); ) {
            String soname = it.next();
            if (soname.contains("/") || soname.startsWith(".")) throw new IOException("Invalid library name " + soname);
            try {
                Os.symlink(library(names.getString(soname)).getPath(), new File(aliases, soname).getPath());
            } catch (ErrnoException error) {
                throw new IOException("Cannot link " + soname, error);
            }
        }
    }

    private static void unpack(Context context, File root) throws IOException {
        if (!root.mkdirs()) throw new IOException("Cannot create the compiler folder");
        String base = root.getCanonicalPath() + File.separator;
        try (ZipInputStream zip = new ZipInputStream(context.getAssets().open("toolchain.zip"))) {
            byte[] buffer = new byte[1 << 16];
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                File file = new File(root, entry.getName());
                if (!file.getCanonicalPath().startsWith(base)) throw new IOException("Invalid compiler file " + entry.getName());
                if (entry.isDirectory()) { file.mkdirs(); continue; }
                file.getParentFile().mkdirs();
                try (OutputStream output = new FileOutputStream(file)) {
                    for (int count; (count = zip.read(buffer)) > 0; ) output.write(buffer, 0, count);
                }
            }
        }
    }

    static String read(File file) {
        if (!file.isFile()) return "";
        try (InputStream input = new java.io.FileInputStream(file)) {
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            GameFiles.copy(input, bytes);
            return bytes.toString("UTF-8");
        } catch (IOException error) {
            return "";
        }
    }

    static void write(File file, String text) throws IOException {
        File temporary = new File(file.getPath() + ".partial");
        try (OutputStream output = new FileOutputStream(temporary)) {
            output.write(text.getBytes(StandardCharsets.UTF_8));
        }
        if (!temporary.renameTo(file)) throw new IOException("Cannot write " + file);
    }
}
