// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** The pure parts of the native code build on the phone (AotBuildService runs libbbrecomp.so, the Rebuild Tool's command line build):
 *  the command line, the progress lines the tool prints (BBR-PROGRESS, BBR-ERROR, BBR-DONE) and the bar they make, the time left, the
 *  number of jobs for the phone's memory and cores, what the phone must have, and the readers of memory, frequencies and
 *  temperature. No Android classes, so it is tested on a desktop JDK (tests/android-native-cpu). The contract is "The phone build" in the
 *  tool's docs/NATIVE_CPU.md. */
final class AotBuild {
    private AotBuild() {}

    /** The stages the tool reports, in order, and their share of the bar in percent (from the CPU time of a build: the compile is 93 %). */
    static final String[] STAGES = {"scan", "discover", "lift", "compile", "variants", "link"};
    static final double[] WEIGHTS = {1, 2, 2, 93, 1, 1};
    /** The output does not depend on --jobs or on the memory with a fixed shard count. */
    static final int SHARDS = 64;
    static final int MAX_JOBS = 4;
    /** Memory: about 0.5 GB + 0.8 GB per job (the tool's measurement: 4 jobs peaked at 3.8 GB). */
    static final double RAM_BASE_GB = 0.5, RAM_PER_JOB_GB = 0.8;
    /** What the work folder takes while it works (objects and the result) and what is left to install the result and keep going. */
    static final long STORAGE_NEED = 1300L << 20, STORAGE_NEED_RESUME = 500L << 20;
    /** The AYN Thor (Snapdragon 8 Gen 2): the whole build is about 1300 CPU seconds on cores of 2.8 GHz, plus about 25 s that run on one core. */
    static final double THOR_CPU_SECONDS = 1300, THOR_KHZ = 2_800_000, SERIAL_SECONDS = 25;
    /** The four files of the game folder the tool reads (eboot.bin, the two system modules, the param.sfo). */
    static final String[] GAME_FILES = {"eboot.bin", "sce_module/libc.prx", "sce_module/libSceFios2.prx", "sce_sys/param.sfo"};

    // ---------------------------------------------------------------- the command line

    /** The tool's command line for the phone (paths absolute, without spaces: the tool splits some internal commands on spaces). */
    static List<String> command(String recomp, String boot, String gameDir, String bundleDir, String variantsDir, String work, int jobs) {
        List<String> c = new ArrayList<>();
        c.add(recomp);
        c.addAll(Arrays.asList("--boot", boot, "--game-dir", gameDir, "--bundle-dir", bundleDir, "--variants", variantsDir,
            "--so", work + "/libbbaot.so", "--stats", work + "/compile.txt", "--jobs", String.valueOf(jobs),
            "--shards", String.valueOf(SHARDS), "-O1", "--no-nice", "--progress", "--resume", "--clean-work"));
        return c;
    }

    /** The work folder of a tool version and a game image: a different one starts fresh, the same one resumes. */
    static String workName(String toolVersion, String imageHash) { return toolVersion + "-" + imageHash; }

    // ---------------------------------------------------------------- the tool's lines

    /** `BBR-PROGRESS <stage> <done> <total>`. */
    static final class Progress {
        final String stage; final long done, total;
        Progress(String stage, long done, long total) { this.stage = stage; this.done = done; this.total = total; }
    }
    private static final Pattern PROGRESS = Pattern.compile("^BBR-PROGRESS\\s+(\\S+)\\s+(\\d+)\\s+(\\d+)\\s*$");
    private static final Pattern DONE = Pattern.compile("^BBR-DONE\\s+(-?\\d+)\\s*$");

    static Progress parseProgress(String line) {
        if (line == null) return null;
        Matcher m = PROGRESS.matcher(line.trim());
        if (!m.matches()) return null;
        try { return new Progress(m.group(1), Long.parseLong(m.group(2)), Long.parseLong(m.group(3))); }
        catch (NumberFormatException e) { return null; }
    }
    /** The message of a `BBR-ERROR <message>` line, or null. */
    static String parseError(String line) {
        if (line == null) return null;
        String t = line.trim();
        if (!t.startsWith("BBR-ERROR")) return null;
        String m = t.substring("BBR-ERROR".length()).trim();
        return m.isEmpty() ? "the build failed" : m;
    }
    static final int NO_EXIT = Integer.MIN_VALUE;
    /** The status of a `BBR-DONE <rc>` line, or NO_EXIT. */
    static int parseDone(String line) {
        if (line == null) return NO_EXIT;
        Matcher m = DONE.matcher(line.trim());
        if (!m.matches()) return NO_EXIT;
        try { return Integer.parseInt(m.group(1)); } catch (NumberFormatException e) { return NO_EXIT; }
    }

    /** The name of a stage for the player. */
    static String stageLabel(String stage) {
        switch (stage == null ? "" : stage) {
            case "scan": return "Reading the game image";
            case "discover": return "Looking for the game's functions";
            case "lift": return "Reading the game's code";
            case "compile": return "Compiling the game's code";
            case "variants": return "Compiling the patched code";
            case "link": case "json": case "done": return "Linking the library";
            default: return "Starting";
        }
    }

    /** Reads the tool's lines into a stage, a bar and the final status. The bar never goes back. */
    static final class Tracker {
        String stage = "";
        double fraction;           // 0..1
        String error;              // the last BBR-ERROR
        int exit = NO_EXIT;        // BBR-DONE
        boolean compileSeen;

        /** Feeds one line; returns whether anything changed. */
        boolean feed(String line) {
            Progress p = parseProgress(line);
            if (p != null) {
                double f = fractionOf(p);
                stage = p.stage;
                if (p.stage.equals("compile")) compileSeen = true;
                if (f > fraction) fraction = f;
                return true;
            }
            String e = parseError(line);
            if (e != null) { error = e; return true; }
            int rc = parseDone(line);
            if (rc != NO_EXIT) { exit = rc; if (rc == 0) fraction = 1; return true; }
            return false;
        }
        boolean succeeded() { return exit == 0; }
    }

    /** The share of the whole bar (0..1) a progress line stands for, by the stages' weights. */
    static double fractionOf(Progress p) {
        int i = Arrays.asList(STAGES).indexOf(p.stage);
        if (p.stage.equals("done")) return 1;
        if (p.stage.equals("json")) return 0.995;
        if (i < 0) return 0;
        double base = 0;
        for (int k = 0; k < i; ++k) base += WEIGHTS[k];
        double part = p.total > 0 ? Math.min(1.0, (double) p.done / p.total) : (p.done > 0 ? 1.0 : 0.0);
        return (base + WEIGHTS[i] * part) / 100.0;
    }

    // ---------------------------------------------------------------- the time left

    /** The time left. Before the build has made 5 % of the bar since its compile began (or from its start when a resume skipped
     *  ahead), it is the estimate from the phone's cores, scaled by what is left; after that, the pace so far, smoothed so that the
     *  figure does not jump (a time constant of 15 s, and what has passed is taken off). */
    static final class Eta {
        private final double initialSeconds;
        private long refMs = -1, lastMs; private double refFraction, smoothed = -1;

        Eta(double initialSeconds) { this.initialSeconds = initialSeconds; }

        /** `compile`: whether the compile stage has started (the analysis before it has a different pace). */
        void update(long nowMs, double fraction, boolean compile) {
            if (refMs < 0 && compile) { refMs = nowMs; refFraction = fraction; lastMs = nowMs; return; }
            if (refMs < 0) return;
            double gained = fraction - refFraction;
            long elapsed = nowMs - refMs;
            double dt = Math.max(0, (nowMs - lastMs) / 1000.0);
            lastMs = nowMs;
            if (gained < 0.05 || elapsed <= 0) return;
            double estimate = (1 - fraction) / (gained / (elapsed / 1000.0));
            if (smoothed < 0) smoothed = estimate;
            else {
                double a = 1 - Math.exp(-dt / 15.0);
                smoothed = Math.max(0, smoothed - dt) * (1 - a) + estimate * a;
            }
        }
        /** Seconds left, or -1 when nothing is known. */
        double remaining(double fraction) {
            if (smoothed >= 0) return Math.max(0, smoothed);
            return initialSeconds > 0 ? initialSeconds * Math.max(0, 1 - fraction) : -1;
        }
    }

    /** The whole build's time on this phone before it starts: the Thor's CPU seconds shared by the jobs and scaled by the speed of the cores
     *  the jobs run on (the `jobs` fastest ones; `maxKhz` of every core, empty or zeros when unknown: the Thor's speed is assumed). */
    static double estimateSeconds(int jobs, long[] maxKhz) {
        jobs = Math.max(1, jobs);
        long[] f = maxKhz == null ? new long[0] : maxKhz.clone();
        Arrays.sort(f);
        double sum = 0; int n = 0;
        for (int i = f.length - 1; i >= 0 && n < jobs; --i, ++n) sum += f[i];
        double khz = n > 0 && sum > 0 ? sum / n : THOR_KHZ;
        return THOR_CPU_SECONDS / jobs * (THOR_KHZ / khz) + SERIAL_SECONDS;
    }

    // ---------------------------------------------------------------- what the phone has

    /** Cores that are not "little": a core whose top speed is at least three quarters of the fastest core's (the phone's small cores are
     *  slower than the tool's work needs and the Thor measured no gain from them). At least 1. */
    static int nonLittleCores(long[] maxKhz) {
        long max = 0;
        for (long v : maxKhz) max = Math.max(max, v);
        if (max <= 0) return Math.max(1, maxKhz.length);
        int n = 0;
        for (long v : maxKhz) if (v * 4 >= max * 3) ++n;
        return Math.max(1, n);
    }

    /** min(4, non-little cores, floor((MemAvailable - 1 GB) / 0.8 GB)), at least 1. */
    static int chooseJobs(int nonLittle, long memAvailableKb) {
        double gb = memAvailableKb / 1048576.0;
        int byRam = (int) Math.floor((gb - 1.0) / RAM_PER_JOB_GB);
        return Math.max(1, Math.min(MAX_JOBS, Math.min(nonLittle, byRam)));
    }

    static double ramNeededGb(int jobs) { return RAM_BASE_GB + RAM_PER_JOB_GB * jobs; }

    /** What the phone can do now. */
    static final class Plan {
        int jobs; double etaSeconds; String blocker; // blocker: why it cannot build here (null: it can)
        long ramNeededKb, storageNeed;
    }

    /** `arm64`: the phone runs arm64 code; `memAvailableKb`: /proc/meminfo; `freeBytes`: usable space of the app's data; `maxKhz`: the cores'
     *  top speeds; `resuming`: an unfinished work folder of this game exists (it needs less new space). */
    static Plan plan(boolean arm64, long memAvailableKb, long freeBytes, long[] maxKhz, boolean resuming) {
        Plan p = new Plan();
        p.jobs = chooseJobs(nonLittleCores(maxKhz), memAvailableKb);
        p.etaSeconds = estimateSeconds(p.jobs, maxKhz);
        p.ramNeededKb = (long) (ramNeededGb(1) * 1048576) ; // the least it can do with
        p.storageNeed = resuming ? STORAGE_NEED_RESUME : STORAGE_NEED;
        // One job needs 0.5 + 0.8 GB; a margin of 0.2 GB for the app itself.
        if (!arm64) p.blocker = "This phone does not run arm64 code: the build program is arm64 only.";
        else if (memAvailableKb < (long) ((ramNeededGb(1) + 0.2) * 1048576))
            p.blocker = String.format(Locale.ROOT, "Not enough free memory: %.1f GB free, at least %.1f GB are needed. Close other apps.",
                memAvailableKb / 1048576.0, ramNeededGb(1) + 0.2);
        else if (freeBytes < p.storageNeed)
            p.blocker = String.format(Locale.ROOT, "Not enough free storage: %d MB free in the app's data, %d MB are needed.", freeBytes >> 20, p.storageNeed >> 20);
        else p.ramNeededKb = (long) (ramNeededGb(p.jobs) * 1048576);
        return p;
    }

    // ---------------------------------------------------------------- readers (files of /proc and /sys)

    /** MemAvailable of /proc/meminfo in kB, or -1. */
    static long memAvailableKb(String meminfo) {
        Matcher m = Pattern.compile("(?m)^MemAvailable:\\s+(\\d+)\\s+kB").matcher(meminfo == null ? "" : meminfo);
        return m.find() ? Long.parseLong(m.group(1)) : -1;
    }
    static long memAvailableKb(File meminfo) { return memAvailableKb(readText(meminfo)); }

    /** VmRSS of a /proc/<pid>/status in kB, or -1. */
    static long vmRssKb(String status) {
        Matcher m = Pattern.compile("(?m)^VmRSS:\\s+(\\d+)\\s+kB").matcher(status == null ? "" : status);
        return m.find() ? Long.parseLong(m.group(1)) : -1;
    }

    /** The top speed of every core that exists (cpuN/cpufreq/cpuinfo_max_freq in kHz); an unreadable one counts as 0. */
    static long[] maxFreqs(File cpuRoot) {
        List<Long> out = new ArrayList<>();
        for (int i = 0; i < 64; ++i) {
            File dir = new File(cpuRoot, "cpu" + i);
            if (!dir.isDirectory()) break;
            long v = 0;
            try { v = Long.parseLong(readText(new File(dir, "cpufreq/cpuinfo_max_freq")).trim()); } catch (RuntimeException ignored) {}
            out.add(v);
        }
        long[] r = new long[out.size()];
        for (int i = 0; i < r.length; ++i) r[i] = out.get(i);
        return r;
    }

    /** A CPU thermal zone: cpu-* or cpuss-* (Qualcomm; the zones of the cores and the CPU subsystem). */
    static boolean isCpuZone(String type) {
        if (type == null) return false;
        String t = type.trim().toLowerCase(Locale.ROOT);
        return t.startsWith("cpu-") || t.startsWith("cpuss-");
    }
    /** A thermal zone's temp file in degrees Celsius (millidegrees or degrees), NaN when unreadable or absurd. */
    static double parseTemp(String raw) {
        try {
            double v = Double.parseDouble(raw.trim());
            if (Math.abs(v) >= 1000) v /= 1000.0;
            return v > -20 && v < 200 ? v : Double.NaN;
        } catch (RuntimeException e) { return Double.NaN; }
    }
    /** The hottest readable CPU zone under /sys/class/thermal in degrees Celsius, or NaN (the row is then hidden). */
    static double hottestCpuC(File thermalRoot) {
        double best = Double.NaN;
        File[] zones = thermalRoot.listFiles((d, n) -> n.startsWith("thermal_zone"));
        if (zones == null) return best;
        for (File z : zones) {
            if (!isCpuZone(readText(new File(z, "type")))) continue;
            double t = parseTemp(readText(new File(z, "temp")));
            if (!Double.isNaN(t) && (Double.isNaN(best) || t > best)) best = t;
        }
        return best;
    }

    static String readText(File f) {
        try { return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8); }
        catch (IOException | RuntimeException e) { return ""; }
    }

    // ---------------------------------------------------------------- the work folder

    /** Shards that are finished in a work folder (an object and its .stat). */
    static int shardsDone(File work) {
        String[] names = work.list();
        if (names == null) return 0;
        int n = 0;
        for (String name : names)
            if (name.matches("libbbaot\\.so\\.o\\.\\d+\\.o\\.stat") && new File(work, name.substring(0, name.length() - 5)).isFile()) ++n;
        return n;
    }
    /** What a resumed build has behind it, in percent of the bar: the finished shards' share of the compile. */
    static int percentDone(int shardsDone) { return (int) Math.min(93, shardsDone * 93L / SHARDS); }

    /** Copies the four files the tool reads from the game folder into `dest` (kept when they are there with the same size). Returns the
     *  name of the file that is missing, or null. IOException: a copy failed. */
    static String copyGameFiles(File gameDir, File dest) throws IOException {
        for (String name : GAME_FILES) {
            File src = new File(gameDir, name);
            if (!src.isFile()) return name;
            File dst = new File(dest, name);
            if (dst.isFile() && dst.length() == src.length()) continue;
            dst.getParentFile().mkdirs();
            File part = new File(dst.getPath() + ".part");
            Files.copy(src.toPath(), part.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            if (!part.renameTo(dst)) { part.delete(); throw new IOException("cannot write " + dst); }
        }
        return null;
    }

    // ---------------------------------------------------------------- text

    /** "5 min 55 s", "42 s". */
    static String clock(long ms) {
        long s = Math.max(0, ms / 1000);
        return s >= 60 ? (s / 60) + " min " + (s % 60) + " s" : s + " s";
    }
    /** "about 4 min left", "less than a minute left"; "" when unknown. */
    static String left(double seconds) {
        if (seconds < 0) return "";
        if (seconds < 45) return "less than a minute left";
        return "about " + Math.max(1, Math.round(seconds / 60)) + " min left";
    }
    /** "about 6 min": the estimate before the start. */
    static String about(double seconds) { return "about " + Math.max(1, Math.round(seconds / 60)) + " min"; }

    /** The process id of a Process (Android 9 has no Process.pid()); 0 when unknown. */
    static int pidOf(Process p) {
        for (Class<?> c = p.getClass(); c != null; c = c.getSuperclass()) {
            try {
                java.lang.reflect.Field f = c.getDeclaredField("pid"); f.setAccessible(true);
                return f.getInt(p);
            } catch (ReflectiveOperationException | RuntimeException ignored) {}
        }
        return 0;
    }
}
