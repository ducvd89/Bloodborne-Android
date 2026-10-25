// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.drawable.Icon;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CopyOnWriteArrayList;

/** Builds the game's native code on the phone: runs the Rebuild Tool's command line build (libbbrecomp.so from the APK's native library
 *  folder, AotBuild.command) on the game image the app made and installs the result through NativeCpu.install. A foreground service,
 *  so the build goes on when the app is in the background (a partial wake lock keeps the CPU on). One build at a time; its state is a
 *  static {@link Status} that the rebuild dialog (RebuildDialog) and the notification both show. Stopping (cancel, a kill, a crash of the
 *  app) leaves the finished shards in the work folder (files/bbport/aot-build/<tool version>-<image hash>/): the next start resumes. */
public final class AotBuildService extends Service {
    static final String ACTION_CANCEL = "ro.cobrabm.fexdroid.AOT_CANCEL", EXTRA_GAME = "game_dir", EXTRA_JOBS = "jobs";
    static final String CHANNEL = "aot_build";
    private static final int NOTIFICATION = 20;

    enum Phase { IDLE, PREPARING, RUNNING, INSTALLING, DONE, FAILED, CANCELLED }

    /** What the dialog and the notification show; copied on every change, so a reader never sees half of an update. */
    static final class Status {
        Phase phase = Phase.IDLE;
        String label = "";            // the stage's name
        double fraction;              // 0..1
        long startMs, elapsedMs;      // since the start of this run
        double etaSeconds = -1;       // -1: unknown
        double tempC = Double.NaN;    // hottest readable CPU zone
        boolean veryHot;              // 95 C or more for 30 s
        boolean charging; int batteryPercent = -1;
        long rssKb = -1;              // the tool's resident memory
        int jobs;
        String error;                 // FAILED: what to tell the player
        long builtMs;                 // DONE: the run's length
        boolean notificationsOff;
        boolean acknowledged;         // the player has seen the end (DONE, FAILED, CANCELLED)
        Status copy() {
            Status s = new Status();
            s.phase = phase; s.label = label; s.fraction = fraction; s.startMs = startMs; s.elapsedMs = elapsedMs; s.etaSeconds = etaSeconds;
            s.tempC = tempC; s.veryHot = veryHot; s.charging = charging; s.batteryPercent = batteryPercent; s.rssKb = rssKb; s.jobs = jobs;
            s.error = error; s.builtMs = builtMs; s.notificationsOff = notificationsOff; s.acknowledged = acknowledged;
            return s;
        }
        boolean active() { return phase == Phase.PREPARING || phase == Phase.RUNNING || phase == Phase.INSTALLING; }
        boolean ended() { return phase == Phase.DONE || phase == Phase.FAILED || phase == Phase.CANCELLED; }
        int percent() { return (int) Math.min(100, Math.floor(fraction * 100)); }
    }

    private static volatile Status status = new Status();
    private static final CopyOnWriteArrayList<Runnable> listeners = new CopyOnWriteArrayList<>();
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static volatile Process child;
    private static volatile boolean cancelRequested;
    private static final Object lock = new Object();

    static Status status() { return status.copy(); }
    static boolean active() { return status.active(); }
    /** Listeners run on the main thread after each change. */
    static void listen(Runnable r) { listeners.addIfAbsent(r); }
    static void unlisten(Runnable r) { listeners.remove(r); }
    /** The player has seen how the build ended: the state goes back to idle, so the dialog does not open for it again. */
    static void acknowledge() {
        synchronized (lock) { if (status.ended()) { Status s = status.copy(); s.acknowledged = true; status = s; } }
    }
    private static void publish(Status s) {
        status = s;
        main.post(() -> { for (Runnable r : listeners) r.run(); });
    }
    private static void update(java.util.function.Consumer<Status> change) {
        synchronized (lock) { Status s = status.copy(); change.accept(s); publish(s); }
    }

    /** Starts the build (from the foreground UI: a foreground service cannot be started from the background). `jobs` from AotBuild.plan. */
    static void start(Context c, String gameDir, int jobs) {
        if (status.active()) return;
        cancelRequested = false;
        synchronized (lock) { Status s = new Status(); s.phase = Phase.PREPARING; s.label = "Starting"; s.jobs = jobs; s.startMs = System.currentTimeMillis(); publish(s); }
        // The channel is created here, with the app in the foreground: on Android 13 and newer the system then asks for the notification
        // permission (the app targets API 28, so there is no permission call of its own). A refusal changes nothing but where the progress shows.
        createChannel(c);
        Intent i = new Intent(c, AotBuildService.class).putExtra(EXTRA_GAME, gameDir).putExtra(EXTRA_JOBS, jobs);
        try { c.startForegroundService(i); }
        catch (RuntimeException e) {
            android.util.Log.w("Bloodborne", "aot build: service: " + e);
            update(s -> { s.phase = Phase.FAILED; s.error = "The build service could not be started: " + e.getMessage(); });
        }
    }

    /** Stops the build: the tool ends at the next function (SIGTERM); finished shards stay for the next start. */
    static void cancel() {
        cancelRequested = true;
        Process p = child;
        if (p != null) terminate(p);
    }
    private static void terminate(Process p) {
        int pid = AotBuild.pidOf(p);
        if (pid > 0) android.os.Process.sendSignal(pid, 15); else p.destroy();
        new Thread(() -> { // the tool ends itself within 8 s; this is for a program that does not listen
            try { Thread.sleep(15000); } catch (InterruptedException ignored) {}
            if (p.isAlive()) p.destroyForcibly();
        }, "aot-kill").start();
    }

    private static void createChannel(Context c) {
        NotificationManager nm = (NotificationManager) c.getSystemService(NOTIFICATION_SERVICE);
        if (nm.getNotificationChannel(CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "Native code build", NotificationManager.IMPORTANCE_LOW);
            ch.setDescription("Progress of building the game's native code on this phone");
            nm.createNotificationChannel(ch);
        }
    }

    // ---------------------------------------------------------------- the service

    private PowerManager.WakeLock wake;
    private Thread worker;

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_CANCEL.equals(intent.getAction())) { cancel(); if (!status.active()) stopSelf(); return START_NOT_STICKY; }
        if (intent == null || intent.getStringExtra(EXTRA_GAME) == null) { stopSelf(); return START_NOT_STICKY; } // restarted without a request: nothing to do
        // The first notification at once: a foreground service has a few seconds to show one.
        createChannel(this);
        startForeground(NOTIFICATION, notification(status));
        if (worker != null && worker.isAlive()) return START_NOT_STICKY;
        final String game = intent.getStringExtra(EXTRA_GAME);
        final int jobs = Math.max(1, intent.getIntExtra(EXTRA_JOBS, 1));
        PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
        wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Bloodborne:aot-build");
        wake.acquire(90 * 60 * 1000L); // never more than the longest build on a slow phone plus a margin
        worker = new Thread(() -> {
            try { build(new File(game), jobs); }
            catch (Throwable t) { // never leave the dialog waiting
                android.util.Log.e("Bloodborne", "aot build: " + t, t);
                update(s -> { s.phase = Phase.FAILED; s.error = "The build failed: " + t; });
            } finally {
                child = null;
                if (wake != null && wake.isHeld()) wake.release();
                finish();
            }
        }, "aot-build");
        worker.start();
        return START_NOT_STICKY;
    }

    @Override public void onDestroy() {
        // The system ended the service: the tool goes with it (its finished shards stay).
        Process p = child;
        if (p != null && status.active()) { cancelRequested = true; terminate(p); }
        if (wake != null && wake.isHeld()) wake.release();
        super.onDestroy();
    }

    /** The last notification (the ending), then the service ends. */
    private void finish() {
        Status s = status;
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        stopForeground(Service.STOP_FOREGROUND_DETACH);
        if (s.phase == Phase.DONE || s.phase == Phase.FAILED) nm.notify(NOTIFICATION, notification(s)); // CANCELLED: the player did it, nothing to tell
        else nm.cancel(NOTIFICATION);
        stopSelf();
    }

    private Notification notification(Status s) {
        Intent open = new Intent(this, MainActivity.class).putExtra("ui_action", "aot-build").addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_NEW_TASK);
        PendingIntent content = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification.Builder b = new Notification.Builder(this, CHANNEL).setContentIntent(content).setOnlyAlertOnce(true).setShowWhen(false);
        int icon = getResources().getIdentifier("ic_cpu", "drawable", getPackageName());
        if (icon != 0) b.setSmallIcon(Icon.createWithResource(this, icon)); else b.setSmallIcon(android.R.drawable.stat_notify_sync);
        if (s.phase == Phase.DONE) {
            b.setContentTitle("Native code ready").setContentText("Built in " + AotBuild.clock(s.builtMs) + ". Tap to start the game.").setAutoCancel(true);
        } else if (s.phase == Phase.FAILED) {
            b.setContentTitle("Native code build failed").setContentText(s.error == null ? "" : s.error).setAutoCancel(true);
        } else {
            String text = s.label + (s.phase == Phase.RUNNING ? " · " + s.percent() + " %" : "");
            String left = s.phase == Phase.RUNNING ? AotBuild.left(s.etaSeconds) : "";
            if (!left.isEmpty()) text += " · " + left;
            b.setContentTitle("Building native code").setContentText(text).setOngoing(true);
            if (s.phase == Phase.RUNNING) b.setProgress(100, s.percent(), false); else b.setProgress(0, 0, true);
            Intent cancel = new Intent(this, AotBuildService.class).setAction(ACTION_CANCEL);
            b.addAction(new Notification.Action.Builder(null, "Cancel",
                PendingIntent.getService(this, 1, cancel, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE)).build());
        }
        return b.build();
    }

    // ---------------------------------------------------------------- the build

    private File base() { return new File(getFilesDir(), "bbport"); }

    private void say(PrintWriter log, String line) {
        android.util.Log.i("Bloodborne", "aot build: " + line);
        if (log != null) { log.println(line); log.flush(); }
    }

    private void build(File gameDir, int jobs) throws IOException, InterruptedException {
        final File base = base();
        File logs = new File(base, "logs"); logs.mkdirs();
        File logFile = new File(logs, "aot-build.log");
        if (logFile.length() > (2L << 20)) logFile.delete(); // one log of the last builds (a resume appends), started again past 2 MB
        try (PrintWriter log = new PrintWriter(new java.io.OutputStreamWriter(new FileOutputStream(logFile, true), StandardCharsets.UTF_8))) {
            log.println("---- " + new java.util.Date() + " build, " + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE + ", jobs " + jobs);
            runBuild(base, gameDir, jobs, log);
        }
    }

    private void runBuild(File base, File gameDir, int jobs, PrintWriter log) throws IOException, InterruptedException {
        File files = getFilesDir();
        update(s -> { s.phase = Phase.PREPARING; s.label = "Preparing the game image"; });
        notifyNow();
        try { RuntimeInstaller.ensure(this, files, m -> {}); }
        catch (Exception e) { fail("The app's runtime could not be installed: " + e.getMessage()); return; }
        String text = RecompAssets.versionText(this), toolVersion = RecompAssets.version(text);
        File program = RecompAssets.program(this);
        if (toolVersion == null) { fail("This APK has no Rebuild Tool files."); return; }
        if (program == null) { fail("The build program (libbbrecomp.so) is not in this app: this phone is not arm64, or the APK was built without it."); return; }
        say(log, "tool " + toolVersion + ", program " + program + " (" + program.length() + " bytes)");
        File recomp = RecompAssets.ensure(this, base);
        if (cancelRequested) { update(s -> s.phase = Phase.CANCELLED); return; }

        // The game image (data/boot-linked.bin): the app makes it at the game's start; a game that never started has none. bb-prepare is a
        // no-op when it matches the game folder.
        File prepare = new File(base, "bionic/bin/bb-prepare");
        if (!prepare.canExecute()) { fail("The app's game preparation tool is missing: " + prepare); return; }
        String prepared = runPrepare(prepare, base, gameDir, log);
        if (prepared != null) { if (!cancelRequested) fail(prepared); return; }
        String hash = NativeCpu.deviceImageHash(base, true);
        if (hash == null) { fail("The game image could not be read after it was prepared."); return; }

        // The work folder: the same tool and the same game resume; anything else is deleted.
        File root = new File(base, "aot-build"), work = new File(root, AotBuild.workName(toolVersion, hash));
        String[] others = root.list();
        if (others != null) for (String n : others) if (!n.equals(work.getName())) FsUtil.deleteTree(new File(root, n));
        if (!work.isDirectory() && !work.mkdirs()) { fail("Cannot create " + work); return; }
        boolean resuming = AotBuild.shardsDone(work) > 0;
        if (files.getUsableSpace() < (resuming ? AotBuild.STORAGE_NEED_RESUME : AotBuild.STORAGE_NEED)) {
            fail(String.format(java.util.Locale.ROOT, "Not enough free storage: %d MB free, about %d MB are needed.", files.getUsableSpace() >> 20,
                (resuming ? AotBuild.STORAGE_NEED_RESUME : AotBuild.STORAGE_NEED) >> 20));
            return;
        }
        // The four files the tool reads, copied next to the work files: it never depends on the child's access to shared storage.
        File gameCopy = new File(work, "game");
        try {
            String missing = AotBuild.copyGameFiles(gameDir, gameCopy);
            if (missing != null) { fail("The game folder has no " + missing + "."); return; }
        } catch (IOException e) { fail("The game files could not be copied: " + e.getMessage()); return; }
        if (cancelRequested) { update(s -> s.phase = Phase.CANCELLED); return; }

        List<String> cmd = AotBuild.command(program.getPath(), NativeCpu.bootImage(base).getPath(), gameCopy.getPath(),
            new File(recomp, "bundle").getPath(), new File(recomp, "variants").getPath(), work.getPath(), jobs);
        say(log, String.join(" ", cmd) + (resuming ? "  (resuming: " + AotBuild.shardsDone(work) + " shards)" : ""));
        ProcessBuilder pb = new ProcessBuilder(cmd).directory(work).redirectErrorStream(true);
        Map<String, String> env = pb.environment(); env.clear();
        env.put("PATH", "/system/bin"); env.put("HOME", files + "/home/bbport"); env.put("LANG", "C.UTF-8");
        new File(files, "tmp").mkdirs(); env.put("TMPDIR", files + "/tmp");
        // The program is a static glibc one: the app's seccomp policy may not know rseq (glibc 2.35 registers it at start); this turns it off.
        env.put("GLIBC_TUNABLES", "glibc.pthread.rseq=0");

        final long t0 = System.currentTimeMillis();
        AotBuild.Tracker tracker = new AotBuild.Tracker();
        AotBuild.Eta eta = new AotBuild.Eta(AotBuild.estimateSeconds(jobs, AotBuild.maxFreqs(new File("/sys/devices/system/cpu"))));
        update(s -> { s.phase = Phase.RUNNING; s.label = AotBuild.stageLabel(""); s.startMs = t0; s.fraction = 0; s.etaSeconds = -1; });
        Process p;
        try { p = pb.start(); }
        catch (IOException e) { fail("The build program could not be started: " + e.getMessage()); return; }
        child = p;
        if (cancelRequested) terminate(p);
        final int pid = AotBuild.pidOf(p);
        Monitor monitor = new Monitor(pid, tracker, eta, t0);
        monitor.start();
        ArrayDeque<String> tail = new ArrayDeque<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null;) {
                if (AotBuild.parseProgress(line) == null) { log.println(line); log.flush(); tail.addLast(line); if (tail.size() > 40) tail.pollFirst(); }
                boolean changed;
                synchronized (tracker) { changed = tracker.feed(line); }
                if (changed) monitor.changed();
            }
        } catch (IOException e) { say(log, "reading the output: " + e); }
        int rc = p.waitFor();
        monitor.stop();
        child = null;
        long ms = System.currentTimeMillis() - t0;
        say(log, "exit " + rc + ", BBR-DONE " + (tracker.exit == AotBuild.NO_EXIT ? "none" : tracker.exit) + ", " + AotBuild.clock(ms) + ", " + AotBuild.percentDone(AotBuild.shardsDone(work)) + "% of shards kept");
        saveStats(base, work);

        if (tracker.succeeded() && rc == 0) { install(base, gameDir, work, ms, log); return; }
        if (cancelRequested || rc == 143 || rc == 130 || tracker.exit == 143 || tracker.exit == 130) {
            update(s -> { s.phase = Phase.CANCELLED; s.elapsedMs = ms; });
            return;
        }
        String why = tracker.error;
        if (why == null) why = exitText(rc, tracker.exit, tail);
        fail(why);
    }

    private static String exitText(int rc, int done, ArrayDeque<String> tail) {
        if (rc == 137 || rc == 9 + 128) return "The build program was killed from outside, most likely by the phone's memory manager. Close other apps and try again: the finished parts are kept.";
        if (rc == 159) return "The phone's security policy stopped the build program (a blocked system call, signal 31). Please send a report.";
        String last = tail.isEmpty() ? "" : " Last line: " + tail.peekLast();
        return "The build program stopped with status " + (done != AotBuild.NO_EXIT ? done : rc) + "." + last;
    }

    private void fail(String message) {
        android.util.Log.w("Bloodborne", "aot build failed: " + message);
        update(s -> { s.phase = Phase.FAILED; s.error = message; });
    }
    private void notifyNow() {
        ((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).notify(NOTIFICATION, notification(status));
    }

    /** bb-prepare <game folder> <data folder>: the game image, a no-op when it is current. Returns null, or why it failed. */
    private String runPrepare(File prepare, File base, File gameDir, PrintWriter log) throws IOException, InterruptedException {
        ProcessBuilder pb = new ProcessBuilder(prepare.getPath(), gameDir.getPath(), new File(base, "data").getPath()).directory(base).redirectErrorStream(true);
        Map<String, String> env = pb.environment(); env.clear();
        env.put("PATH", "/system/bin"); env.put("HOME", getFilesDir() + "/home/bbport"); env.put("TMPDIR", getFilesDir() + "/tmp"); env.put("LANG", "C.UTF-8");
        new File(base, "data").mkdirs(); new File(getFilesDir(), "tmp").mkdirs();
        Process p;
        try { p = pb.start(); } catch (IOException e) { return "The game preparation tool could not be started: " + e.getMessage(); }
        child = p; // a cancel during it ends it too
        String last = "";
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
            for (String line; (line = r.readLine()) != null;) { say(log, "bb-prepare: " + line); if (!line.trim().isEmpty()) last = line.trim(); }
        }
        int rc = p.waitFor();
        child = null;
        if (cancelRequested) { update(s -> s.phase = Phase.CANCELLED); return "Cancelled."; }
        return rc == 0 ? null : "The game image could not be prepared (" + (last.isEmpty() ? "status " + rc : last) + ").";
    }

    /** compile.txt of the run is kept in logs/ (the problem report includes it). */
    private void saveStats(File base, File work) {
        File stats = new File(work, "compile.txt");
        if (!stats.isFile()) return;
        try { java.nio.file.Files.copy(stats.toPath(), new File(base, "logs/aot-compile.txt").toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING); }
        catch (IOException ignored) {}
    }

    /** BBR-DONE 0: the pair goes through the same checks as a pair from a PC, into bbport/aot, and a copy into the game folder's aot/. */
    private void install(File base, File gameDir, File work, long ms, PrintWriter log) {
        update(s -> { s.phase = Phase.INSTALLING; s.label = "Checking and installing"; s.fraction = 1; s.elapsedMs = ms; s.etaSeconds = 0; });
        notifyNow();
        File so = new File(work, NativeCpu.SO), json = new File(work, NativeCpu.JSON);
        String[] id = NativeCpu.identity(gameDir);
        try {
            String jsonText = SafeWrite.readText(json);
            if (jsonText == null || !so.isFile()) { fail("The build finished but did not leave libbbaot.so and libbbaot.json."); return; }
            try (FileInputStream in = new FileInputStream(so)) { NativeCpu.install(base, jsonText, in, so.length(), id[0], id[1], null); }
            String copy = NativeCpu.copyToGame(gameDir, so, json);
            say(log, copy == null ? "copied libbbaot.so and libbbaot.json into " + new File(gameDir, "aot") : "could not copy them into the game folder's aot/ (" + copy + "); the app's own copy is installed");
        } catch (NativeCpu.Problem p) { fail("The built library was refused: " + p.getMessage()); return; }
        catch (IOException | RuntimeException e) { fail("The built library could not be installed: " + e); return; }
        FsUtil.deleteTree(work);
        update(s -> { s.phase = Phase.DONE; s.builtMs = ms; s.fraction = 1; s.label = "Native code ready"; });
    }

    /** Once a second (and after a progress line): the bar, the time left, the temperature, the charger, the tool's memory; the notification. */
    private final class Monitor {
        private final int pid; private final AotBuild.Tracker tracker; private final AotBuild.Eta eta; private final long t0;
        private volatile boolean stop; private final Thread thread; private long hotSince = -1, lastNotify, lastPublish; private int lastPercent = -1;
        Monitor(int pid, AotBuild.Tracker tracker, AotBuild.Eta eta, long t0) {
            this.pid = pid; this.tracker = tracker; this.eta = eta; this.t0 = t0;
            thread = new Thread(() -> {
                while (!stop) {
                    changed();
                    try { Thread.sleep(1000); } catch (InterruptedException e) { return; }
                }
            }, "aot-monitor");
            thread.setDaemon(true);
        }
        void start() { thread.start(); }
        void stop() { stop = true; thread.interrupt(); }
        synchronized void changed() {
            long now = System.currentTimeMillis();
            final String stage; final double fraction; final boolean compile;
            synchronized (tracker) { stage = tracker.stage; fraction = tracker.fraction; compile = tracker.compileSeen; }
            if (now - lastPublish < 200) return;
            lastPublish = now;
            eta.update(now, fraction, compile);
            double temp = AotBuild.hottestCpuC(new File("/sys/class/thermal"));
            if (!Double.isNaN(temp) && temp >= 95) { if (hotSince < 0) hotSince = now; } else hotSince = -1;
            long rss = pid > 0 ? AotBuild.vmRssKb(AotBuild.readText(new File("/proc/" + pid + "/status"))) : -1;
            Intent battery = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            boolean charging = false; int pct = -1;
            if (battery != null) {
                int plugged = battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0);
                charging = plugged != 0;
                int level = battery.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = battery.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
                if (level >= 0 && scale > 0) pct = level * 100 / scale;
            }
            final boolean ch = charging; final int bp = pct; final double remaining = eta.remaining(fraction);
            final boolean hot = hotSince >= 0 && now - hotSince >= 30000;
            final boolean notificationsOff = !((NotificationManager) getSystemService(NOTIFICATION_SERVICE)).areNotificationsEnabled();
            update(s -> {
                if (s.phase != Phase.RUNNING) return;
                s.label = AotBuild.stageLabel(stage); s.fraction = fraction; s.elapsedMs = now - t0; s.etaSeconds = remaining;
                s.tempC = temp; s.veryHot = hot; s.charging = ch; s.batteryPercent = bp; s.rssKb = rss; s.notificationsOff = notificationsOff;
            });
            Status s = status;
            if (s.phase == Phase.RUNNING && (s.percent() != lastPercent || now - lastNotify > 15000)) {
                lastPercent = s.percent(); lastNotify = now;
                notifyNow();
            }
        }
    }
}
