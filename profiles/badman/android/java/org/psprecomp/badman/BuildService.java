package org.psprecomp.badman;

import android.app.ActivityManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.Uri;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;

/**
 * Prepares the game from the player's own ISO on the phone: verify, extract,
 * decrypt, prepare the font module, translate to C++ and compile the game
 * library. Runs as a foreground service with a wake lock so a locked screen
 * does not stall the compile.
 */
public final class BuildService extends Service {
    static final String EXTRA_ISO = "iso"; // Absent: rebuild from the files already prepared.
    private static final String CHANNEL = "build";
    private static final int NOTIFICATION = 1;

    /** Shown by the launcher; written only by the build thread. */
    static final class Status {
        volatile boolean running, failed, finished;
        volatile boolean fromIso; // Started from the player's ISO, not a rebuild of prepared files.
        volatile String stage = "", detail = "", error = "";
        volatile float progress;
    }
    static volatile Status status = new Status();

    private final List<Process> processes = Collections.synchronizedList(new ArrayList<>());
    private volatile boolean cancelled;
    private PowerManager.WakeLock wakeLock;
    private Notification.Builder notification;

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if ("cancel".equals(intent == null ? null : intent.getAction())) {
            cancel();
            return START_NOT_STICKY;
        }
        if (status.running) return START_NOT_STICKY;
        final Status current = new Status();
        current.running = true;
        status = current;
        cancelled = false;
        startInForeground();
        PowerManager power = (PowerManager) getSystemService(POWER_SERVICE);
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "badman:build");
        wakeLock.acquire(TimeUnit.HOURS.toMillis(2));
        final String iso = intent == null ? null : intent.getStringExtra(EXTRA_ISO);
        current.fromIso = iso != null;
        new Thread(() -> {
            try {
                build(iso == null ? null : Uri.parse(iso), current);
                stage(current, "Ready to play", "", 1f);
            } catch (Throwable error) {
                current.failed = true;
                current.error = cancelled ? "Preparation was cancelled." : String.valueOf(error.getMessage());
            } finally {
                stopProcesses();
                GameFiles.deleteTree(new GameFiles(this).setup());
                current.running = false;
                current.finished = true;
                if (wakeLock.isHeld()) wakeLock.release();
                stopForeground(STOP_FOREGROUND_REMOVE);
                stopSelf();
            }
        }, "badman-build").start();
        return START_NOT_STICKY;
    }

    @Override public void onDestroy() {
        cancel();
        super.onDestroy();
    }

    private void cancel() {
        cancelled = true;
        stopProcesses();
    }

    private void stopProcesses() {
        synchronized (processes) { for (Process process : processes) process.destroyForcibly(); }
    }

    private void startInForeground() {
        NotificationManager manager = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL, "Game preparation", NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, LauncherActivity.class),
            PendingIntent.FLAG_IMMUTABLE);
        notification = new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle("Preparing Badman").setContentText("Starting").setOngoing(true)
            .setContentIntent(open).setProgress(100, 0, false);
        if (Build.VERSION.SDK_INT >= 34)
            startForeground(NOTIFICATION, notification.build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else
            startForeground(NOTIFICATION, notification.build());
    }

    private synchronized void stage(Status current, String stage, String detail, float progress) {
        current.stage = stage;
        current.detail = detail;
        current.progress = progress;
        if (notification != null) {
            notification.setContentText(detail.isEmpty() ? stage : stage + ": " + detail)
                .setProgress(100, Math.round(progress * 100), false);
            ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).notify(NOTIFICATION, notification.build());
        }
    }

    private void build(Uri iso, Status current) throws Exception {
        GameFiles files = new GameFiles(this);
        JSONObject facts = files.facts();
        File setup = files.setup();
        GameFiles.deleteTree(setup);
        if (!setup.mkdirs()) throw new IOException("Cannot create the preparation folder");
        Toolchain tools = Toolchain.prepare(this);
        String libraries = tools.libraries;

        if (iso != null) {
            stage(current, "Checking your ISO", "", 0f);
            File copy = new File(setup, "game.iso");
            String hash = copyAndHash(iso, copy, facts.getLong("iso_size"), current);
            if (copy.length() != facts.getLong("iso_size") || !hash.equals(facts.getString("iso_sha256")))
                throw new IOException("This is not the supported ISO. " + LauncherActivity.NAME + " needs Holy Invasion of Privacy, "
                    + "Badman! What Did I Do to Deserve This? (ULUS10456, version 1.01).");

            stage(current, "Extracting game files", "", 0.12f);
            File extracted = new File(setup, "extracted");
            run(libraries, "extract", tools.library("libpsp_iso_extract.so").getPath(), copy.getPath(), extracted.getPath(),
                "--subtree", "PSP_GAME", "--exclude", "PSP_GAME/SYSDIR/UPDATE");
            copy.delete();
            File data = files.dataRoot();
            data.mkdirs();
            File game = new File(data, "PSP_GAME");
            GameFiles.deleteTree(game);
            if (!new File(extracted, "PSP_GAME").renameTo(game)) throw new IOException("Cannot move the extracted game files");

            stage(current, "Decrypting the game executable", "", 0.15f);
            run(libraries, "decrypt", tools.library("libpspdecrypt.so").getPath(), "-o", files.executable().getPath(),
                new File(game, "SYSDIR/EBOOT.BIN").getPath());
            stage(current, "Preparing the font module", "", 0.17f);
            File font = files.fontModule();
            font.getParentFile().mkdirs();
            font.delete();
            run(libraries, "font", tools.library("libbadman_prepare.so").getPath(), "font",
                new File(game, "USRDIR/data_usa/module/libfont.prx").getPath(), tools.library("libpspdecrypt.so").getPath(),
                new File(setup, "font-work").getPath(), font.getPath());
        }
        stage(current, "Checking prepared files", "", 0.19f);
        if (!files.executable().isFile() || !GameFiles.sha256(files.executable()).equals(facts.getString("elf_sha256")))
            throw new IOException("The decrypted game executable does not match ULUS10456 1.01.");
        if (!files.fontModule().isFile() || !GameFiles.sha256(files.fontModule()).equals(facts.getString("font_sha256")))
            throw new IOException("The prepared font module does not match the expected result.");

        stage(current, "Translating the game to C++", "", 0.2f);
        File build = files.buildDirectory();
        File generated = new File(build, "generated"), fontGenerated = new File(build, "font_generated"), objects = new File(build, "obj");
        for (File directory : new File[] {generated, fontGenerated, objects}) GameFiles.deleteTree(directory);
        String recompiler = tools.library("libpsp_recomp.so").getPath();
        run(libraries, "generate-main", tools.library("libbadman_prepare.so").getPath(), "generate", "main",
            files.executable().getPath(), recompiler, generated.getPath());
        run(libraries, "generate-font", tools.library("libbadman_prepare.so").getPath(), "generate", "font",
            files.fontModule().getPath(), recompiler, fontGenerated.getPath());

        // Same flags as the NDK build of the game library (Release, then the per-unit levels).
        List<String[]> units = new ArrayList<>();
        for (File unit : sorted(generated)) units.add(new String[] {unit.getPath(), "-O2", "-ffp-contract=off", "-fno-strict-aliasing"});
        for (File unit : sorted(fontGenerated)) units.add(new String[] {unit.getPath(), "-O0"});
        units.add(new String[] {tools.entry.getPath(), "-O3"});
        if (!objects.mkdirs()) throw new IOException("Cannot create the object folder");
        int jobs = jobs();
        final AtomicInteger done = new AtomicInteger();
        final int total = units.size();
        stage(current, "Compiling the game", "0 of " + total + " parts, " + jobs + " at a time", 0.25f);
        ExecutorService pool = Executors.newFixedThreadPool(jobs);
        List<Future<?>> results = new ArrayList<>();
        final List<String> outputs = Collections.synchronizedList(new ArrayList<>());
        for (final String[] unit : units) {
            results.add(pool.submit(() -> {
                String name = new File(unit[0]).getName().replace(".cpp", "");
                String object = new File(objects, name + ".o").getPath();
                List<String> command = new ArrayList<>(Arrays.asList(tools.library("libpsp_clang_tool.so").getPath(),
                    "--driver-mode=g++", "--target=" + tools.target, "--sysroot=" + tools.sysroot, "-resource-dir=" + tools.resource,
                    "-fintegrated-cc1", "-std=c++20", "-fPIC", "-DANDROID", "-DNDEBUG", "-D_FORTIFY_SOURCE=2",
                    "-fdata-sections", "-ffunction-sections", "-funwind-tables", "-fstack-protector-strong", "-g0",
                    "-I", tools.include.getPath()));
                command.addAll(Arrays.asList(unit).subList(1, unit.length));
                command.addAll(Arrays.asList("-c", unit[0], "-o", object));
                run(tools.compilerLibraryPath(), "compile-" + name, command.toArray(new String[0]));
                outputs.add(object);
                int count = done.incrementAndGet();
                stage(current, "Compiling the game", count + " of " + total + " parts", 0.25f + 0.65f * count / total);
                return null;
            }));
        }
        pool.shutdown();
        try {
            for (Future<?> result : results) result.get();
        } catch (java.util.concurrent.ExecutionException error) {
            pool.shutdownNow();
            stopProcesses();
            Throwable cause = error.getCause();
            throw cause instanceof Exception ? (Exception) cause : new IOException(cause);
        }

        stage(current, "Linking the game library", "", 0.92f);
        File api = tools.apiLibraries();
        File partial = new File(build, "libbadman_game.so.partial");
        partial.delete();
        List<String> link = new ArrayList<>(Arrays.asList(tools.library("libpsp_lld_tool.so").getPath(), "-flavor", "gnu",
            "--sysroot=" + tools.sysroot, "-shared", "-o", partial.getPath(), "-soname", "libbadman_game.so",
            "-z", "now", "-z", "relro", "-z", "max-page-size=16384", "--hash-style=gnu", "--eh-frame-hdr",
            "--build-id=sha1", "--no-rosegment", "--no-undefined", "--gc-sections"));
        if (tools.triple.startsWith("aarch64")) link.add("--fix-cortex-a53-843419");
        link.add(new File(api, "crtbegin_so.o").getPath());
        List<String> sortedObjects = new ArrayList<>(outputs);
        Collections.sort(sortedObjects);
        link.addAll(sortedObjects);
        // The libraries and order NDK Clang passes for a shared library.
        String builtins = tools.builtins.getPath(), unwind = tools.unwind.getPath();
        link.addAll(Arrays.asList(tools.library("libbadman_runtime.so").getPath(), tools.library("libc++_shared.so").getPath(),
            "-L" + api, "-lm", builtins, unwind, "-ldl", "-lc", builtins, unwind, "-ldl", new File(api, "crtend_so.o").getPath()));
        run(tools.compilerLibraryPath(), "link", link.toArray(new String[0]));
        if (!partial.renameTo(files.gameLibrary())) throw new IOException("Cannot install the game library");
        files.writeStamp();
        // The translation and objects are only inputs; the library is what Play loads.
        for (File directory : new File[] {generated, fontGenerated, objects}) GameFiles.deleteTree(directory);
    }

    /** Parallel compiles: one per core, as far as free memory allows (about 450 MB each at -O2). */
    private int jobs() {
        ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
        ((ActivityManager) getSystemService(ACTIVITY_SERVICE)).getMemoryInfo(memory);
        long byMemory = memory.availMem / (450L << 20);
        return (int) Math.max(1, Math.min(Math.min(Runtime.getRuntime().availableProcessors(), 8), byMemory));
    }

    private static List<File> sorted(File directory) {
        File[] list = directory.listFiles((dir, name) -> name.endsWith(".cpp"));
        if (list == null) return new ArrayList<>();
        List<File> result = new ArrayList<>(Arrays.asList(list));
        Collections.sort(result);
        return result;
    }

    private String copyAndHash(Uri uri, File target, long expected, Status current) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = getContentResolver().openInputStream(uri); OutputStream output = new FileOutputStream(target)) {
            if (input == null) throw new IOException("Cannot open the chosen file");
            byte[] buffer = new byte[1 << 20];
            long copied = 0;
            for (int count; (count = input.read(buffer)) > 0; ) {
                if (cancelled) throw new IOException("cancelled");
                output.write(buffer, 0, count);
                digest.update(buffer, 0, count);
                copied += count;
                if (copied > expected) throw new IOException("This is not the supported ISO (it is larger than ULUS10456 1.01).");
                current.progress = 0.12f * copied / expected;
            }
        }
        return GameFiles.hex(digest.digest());
    }

    /** Runs one tool to completion, keeping its output in logs/build-<step>.log for the error message. */
    private void run(String libraryPath, String step, String... command) throws Exception {
        if (cancelled) throw new IOException("cancelled");
        File log = new File(new GameFiles(this).logs(), "build-" + step + ".log");
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true).redirectOutput(log);
        builder.environment().put("LD_LIBRARY_PATH", libraryPath);
        builder.environment().put("TMPDIR", getCacheDir().getPath());
        Process process = builder.start();
        processes.add(process);
        try {
            int code = process.waitFor();
            if (cancelled) throw new IOException("cancelled");
            if (code != 0) throw new IOException("The " + step + " step failed (exit " + code + "):\n" + GameFiles.tail(log, 12));
        } finally {
            processes.remove(process);
        }
        if (!step.startsWith("compile-")) return;
        log.delete(); // A clean compile leaves nothing worth keeping.
    }
}
