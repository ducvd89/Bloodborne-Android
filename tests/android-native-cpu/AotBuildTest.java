// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

/** Host-JDK test of the pure parts of the build on the phone (AotBuild: the command line, the tool's progress lines, the bar, the time left, the
 *  jobs and the needs of the phone, the readers of /proc and /sys). Run: tests/android-native-cpu/run.sh */
public final class AotBuildTest {
    static int checks, failed;
    static void check(boolean ok, String what) { ++checks; if (!ok) { ++failed; System.out.println("FAIL: " + what); } }
    static boolean near(double a, double b, double eps) { return Math.abs(a - b) <= eps; }

    public static void main(String[] args) throws Exception {
        // ---- the command line
        List<String> c = AotBuild.command("/lib/libbbrecomp.so", "/f/bbport/data/boot-linked.bin", "/w/game", "/f/recomp/1.2.0/bundle", "/f/recomp/1.2.0/variants", "/w", 4, false, null);
        check(String.join(" ", c).equals("/lib/libbbrecomp.so --boot /f/bbport/data/boot-linked.bin --game-dir /w/game --bundle-dir /f/recomp/1.2.0/bundle "
            + "--variants /f/recomp/1.2.0/variants --so /w/libbbaot.so --stats /w/compile.txt --jobs 4 --shards 64 -O1 --no-nice --progress --resume --clean-work"), "the command line is the tool's contract");
        check(AotBuild.workName("0123456789abcdef").equals("0123456789abcdef"), "work folder name");
        check(String.join(" ", AotBuild.command("/r", "/b", "/g", "/bu", "/v", "/w", 2, true, "/m.json")).endsWith("--resume --manifest /m.json"), "kept work files: no --clean-work, manifest passed");
        check(AotBuild.keepWork(3L << 30, false) && !AotBuild.keepWork(2L << 30, false) && AotBuild.keepWork(2L << 30, true), "keep the work files only with room to spare");
        check(AotBuild.outdated(null, "ab12") && AotBuild.outdated("cd", "ab12") && !AotBuild.outdated("AB12", "ab12") && !AotBuild.outdated(null, null), "outdated library detection");
        int[] ru = AotBuild.parseReused("bb-recomp: Reused 58 of 64 shards (6 to build)");
        check(ru != null && ru[0] == 58 && ru[1] == 64 && AotBuild.parseReused("compile done") == null, "reused shards line");
        AotBuild.Tracker rt = new AotBuild.Tracker(); rt.feed("reused 60 of 64 shards"); rt.feed("BBR-PROGRESS compile 2 4");
        check(rt.fraction > 0.9, "progress that counts only the rebuilt shards includes the reused ones");

        // ---- the tool's lines
        AotBuild.Progress p = AotBuild.parseProgress("BBR-PROGRESS compile 123 4567");
        check(p != null && p.stage.equals("compile") && p.done == 123 && p.total == 4567, "progress line");
        check(AotBuild.parseProgress("  BBR-PROGRESS scan 0 1  \r") != null, "padding and CR are fine");
        check(AotBuild.parseProgress("BBR-PROGRESS compile x 4") == null && AotBuild.parseProgress("compile 1 2") == null && AotBuild.parseProgress(null) == null
            && AotBuild.parseProgress("BBR-PROGRESS compile 1") == null && AotBuild.parseProgress("lifted BBR-PROGRESS a 1 2") == null, "not progress lines");
        check("cannot map the image".equals(AotBuild.parseError("BBR-ERROR cannot map the image")) && AotBuild.parseError("BBR-ERROR").equals("the build failed")
            && AotBuild.parseError("BBR-DONE 1") == null, "error line");
        check(AotBuild.parseDone("BBR-DONE 0") == 0 && AotBuild.parseDone("BBR-DONE 143") == 143 && AotBuild.parseDone("BBR-DONE") == AotBuild.NO_EXIT
            && AotBuild.parseDone("BBR-DONE x") == AotBuild.NO_EXIT && AotBuild.parseDone("something") == AotBuild.NO_EXIT, "done line");

        // ---- the bar: weights scan 1, discover 2, lift 2, compile 93, variants 1, link 1
        double sum = 0; for (double w : AotBuild.WEIGHTS) sum += w;
        check(sum == 100 && AotBuild.STAGES.length == AotBuild.WEIGHTS.length, "weights add up to 100");
        check(near(AotBuild.fractionOf(new AotBuild.Progress("scan", 1, 1)), 0.01, 1e-9), "scan complete = 1 %");
        check(near(AotBuild.fractionOf(new AotBuild.Progress("lift", 0, 10)), 0.03, 1e-9) && near(AotBuild.fractionOf(new AotBuild.Progress("lift", 10, 10)), 0.05, 1e-9), "lift: 3 to 5 %");
        check(near(AotBuild.fractionOf(new AotBuild.Progress("compile", 50, 100)), 0.05 + 0.465, 1e-9), "compile half = 51.5 %");
        check(near(AotBuild.fractionOf(new AotBuild.Progress("compile", 100, 100)), 0.98, 1e-9) && near(AotBuild.fractionOf(new AotBuild.Progress("variants", 1, 1)), 0.99, 1e-9)
            && near(AotBuild.fractionOf(new AotBuild.Progress("link", 3, 3)), 1.0, 1e-9), "end of compile 98 %, variants 99 %, link 100 %");
        check(AotBuild.fractionOf(new AotBuild.Progress("compile", 200, 100)) <= 0.98 + 1e-9 && AotBuild.fractionOf(new AotBuild.Progress("compile", 5, 0)) >= 0.05, "a done beyond total is capped; total 0 does not divide");
        check(AotBuild.fractionOf(new AotBuild.Progress("weird", 1, 2)) == 0 && AotBuild.fractionOf(new AotBuild.Progress("done", 1, 1)) == 1, "unknown stage, done stage");
        AotBuild.Tracker t = new AotBuild.Tracker();
        check(!t.feed("some human line") && t.fraction == 0, "a human line changes nothing");
        t.feed("BBR-PROGRESS scan 0 1"); t.feed("BBR-PROGRESS scan 1 1"); t.feed("BBR-PROGRESS discover 10 20");
        check(t.stage.equals("discover") && near(t.fraction, 0.02, 1e-9) && !t.compileSeen, "tracker: discover half");
        t.feed("BBR-PROGRESS compile 30 100");
        double f1 = t.fraction;
        t.feed("BBR-PROGRESS compile 10 100"); // never goes back
        check(t.compileSeen && t.fraction == f1 && near(f1, 0.05 + 0.279, 1e-9), "tracker: compile, the bar never goes back");
        t.feed("BBR-ERROR boom"); t.feed("BBR-DONE 1");
        check("boom".equals(t.error) && t.exit == 1 && !t.succeeded() && t.fraction < 1, "tracker: failure");
        AotBuild.Tracker ok = new AotBuild.Tracker();
        ok.feed("BBR-PROGRESS json 1 1"); ok.feed("BBR-PROGRESS done 1 1"); ok.feed("BBR-DONE 0");
        check(ok.succeeded() && ok.fraction == 1, "tracker: only BBR-DONE 0 counts");
        AotBuild.Tracker killed = new AotBuild.Tracker(); killed.feed("BBR-PROGRESS compile 1 2");
        check(!killed.succeeded() && killed.exit == AotBuild.NO_EXIT, "no BBR-DONE (killed): not a success");
        check(!AotBuild.stageLabel("compile").isEmpty() && AotBuild.stageLabel("nonsense").equals("Starting"), "stage names");

        // ---- the time left
        AotBuild.Eta eta = new AotBuild.Eta(360);
        check(near(eta.remaining(0), 360, 1e-9) && near(eta.remaining(0.5), 180, 1e-9), "before the first sample: the estimate scaled by what is left");
        eta.update(0, 0.05, false);
        check(near(eta.remaining(0.05), 342, 1e-9), "the analysis stages do not set the pace");
        eta.update(10_000, 0.05, true); // compile starts
        eta.update(20_000, 0.07, true);
        check(near(eta.remaining(0.07), 360 * 0.93, 1e-9), "under 5 % of progress since the compile began: still the estimate");
        eta.update(30_000, 0.12, true); // 7 % in 20 s: 0.07/20 per s -> (1-.12)/0.0035 = 251 s
        check(near(eta.remaining(0.12), 251.4, 1.0), "from 5 % on: the pace so far (" + eta.remaining(0.12) + ")");
        // a steady pace of 0.1 % per second: the figure follows without jumping
        AotBuild.Eta e2 = new AotBuild.Eta(1000); long now = 0; double f = 0.05;
        e2.update(now, f, true);
        double prev = -1; boolean smooth = true;
        for (int i = 1; i <= 400; ++i) {
            now += 1000; f += 0.001;
            e2.update(now, f, true);
            if (f - 0.05 >= 0.05) {
                double r = e2.remaining(f);
                if (prev >= 0 && Math.abs(r - prev) > 5) smooth = false;
                prev = r;
            }
        }
        check(smooth, "steady pace: the time left does not jump");
        check(near(e2.remaining(f), (1 - f) / 0.001, 5), "steady pace: " + e2.remaining(f) + " s left at " + f);
        // a resume that jumps ahead at the first compile line
        AotBuild.Eta e3 = new AotBuild.Eta(360);
        e3.update(0, 0.05, false); e3.update(5000, 0.60, true); e3.update(25_000, 0.70, true); e3.update(35_000, 0.80, true);
        check(e3.remaining(0.80) > 30 && e3.remaining(0.80) < 45, // the pace is 0.2 in 30 s: 30 s left, smoothed from the 60 s of the first estimate
             "resume: the pace counts from the first compile line");
        check(new AotBuild.Eta(0).remaining(0.3) == -1, "no estimate, nothing known");
        check(near(eta.remaining(1.0), 0, 1e-9) || eta.remaining(1.0) >= 0, "never negative");

        // the estimate before the start: the Thor's ~1300 CPU seconds over the jobs, scaled by the cores' speed
        long[] thor = {2_000_000, 2_000_000, 2_000_000, 2_800_000, 2_800_000, 2_800_000, 2_800_000, 3_360_000};
        double est = AotBuild.estimateSeconds(4, thor);
        check(est > 330 && est < 380, "Thor, 4 jobs: " + est + " s (measured 355 s)");
        check(AotBuild.estimateSeconds(4, new long[0]) == AotBuild.estimateSeconds(4, null), "unknown frequencies: the Thor's speed");
        check(AotBuild.estimateSeconds(1, thor) > AotBuild.estimateSeconds(4, thor) * 2, "one job takes much longer");
        long[] slow = new long[8]; java.util.Arrays.fill(slow, 1_400_000);
        check(AotBuild.estimateSeconds(4, slow) > est * 1.8, "half the clock: about twice the time");
        check(AotBuild.about(355).equals("about 6 min") && AotBuild.about(10).equals("about 1 min") && AotBuild.left(20).equals("less than a minute left")
            && AotBuild.left(250).equals("about 4 min left") && AotBuild.left(-1).isEmpty(), "time texts");
        check(AotBuild.clock(355_000).equals("5 min 55 s") && AotBuild.clock(42_000).equals("42 s") && AotBuild.clock(-5).equals("0 s"), "clock text");

        // ---- jobs and what the phone must have
        check(AotBuild.nonLittleCores(thor) == 5, "Thor: five non-little cores (1 prime + 4 mid)");
        long[] gen3 = {2_270_000, 2_270_000, 3_150_000, 3_150_000, 3_150_000, 3_150_000, 3_150_000, 3_300_000};
        check(AotBuild.nonLittleCores(gen3) == 6, "8 Gen 3: the two slow cores are little");
        check(AotBuild.nonLittleCores(new long[0]) == 1 && AotBuild.nonLittleCores(new long[]{0, 0, 0, 0}) == 4, "unknown speeds: all cores, at least 1");
        long gb = 1048576;
        check(AotBuild.chooseJobs(5, 8 * gb) == 4, "plenty of memory: 4 jobs");
        check(AotBuild.chooseJobs(5, (long) (4.6 * gb)) == 4 && AotBuild.chooseJobs(5, (long) (4.1 * gb)) == 3, "memory limits the jobs: (avail - 1 GB) / 0.8 GB");
        check(AotBuild.chooseJobs(2, 12 * gb) == 2 && AotBuild.chooseJobs(1, 12 * gb) == 1, "few big cores: fewer jobs");
        check(AotBuild.chooseJobs(5, gb) == 1 && AotBuild.chooseJobs(5, 0) == 1, "at least one job");
        check(near(AotBuild.ramNeededGb(4), 3.7, 1e-9), "4 jobs need 3.7 GB (measured peak 3.8 GB)");
        AotBuild.Plan plan = AotBuild.plan(true, 9 * gb, 20L << 30, thor, false);
        check(plan.blocker == null && plan.jobs == 4 && plan.etaSeconds > 300 && plan.storageNeed == AotBuild.STORAGE_NEED, "a good phone can build");
        check(AotBuild.plan(false, 9 * gb, 20L << 30, thor, false).blocker.contains("arm64"), "not arm64: blocked");
        check(AotBuild.plan(true, gb, 20L << 30, thor, false).blocker.contains("free memory"), "1 GB free: blocked by memory");
        check(AotBuild.plan(true, (long) (1.6 * gb), 20L << 30, thor, false).blocker == null && AotBuild.plan(true, (long) (1.6 * gb), 20L << 30, thor, false).jobs == 1, "1.6 GB free: one job");
        check(AotBuild.plan(true, 9 * gb, 800L << 20, thor, false).blocker.contains("free storage"), "800 MB of storage: blocked");
        check(AotBuild.plan(true, 9 * gb, 800L << 20, thor, true).blocker == null, "800 MB is enough to resume");

        // ---- readers of /proc and /sys
        check(AotBuild.memAvailableKb("MemTotal:  16000000 kB\nMemFree: 100 kB\nMemAvailable:    8123456 kB\n") == 8123456 && AotBuild.memAvailableKb("nothing") == -1 && AotBuild.memAvailableKb((String) null) == -1, "MemAvailable");
        check(AotBuild.vmRssKb("Name:\tx\nVmPeak:\t 9 kB\nVmRSS:\t  2048000 kB\nThreads: 5") == 2048000 && AotBuild.vmRssKb("") == -1, "VmRSS");
        check(AotBuild.isCpuZone("cpu-1-2-usr") && AotBuild.isCpuZone("cpuss-0-usr") && AotBuild.isCpuZone("CPU-0-0") && !AotBuild.isCpuZone("gpuss-0-usr") && !AotBuild.isCpuZone("battery") && !AotBuild.isCpuZone(null), "cpu zones");
        check(AotBuild.parseTemp("62300\n") == 62.3 && AotBuild.parseTemp("48") == 48 && Double.isNaN(AotBuild.parseTemp("abc")) && Double.isNaN(AotBuild.parseTemp("-274000")) && Double.isNaN(AotBuild.parseTemp("900")), "temperatures in milli-degrees or degrees, junk is NaN");
        File root = Files.createTempDirectory("bbsys").toFile();
        String[][] zones = {{"cpu-0-0-usr", "71000"}, {"cpuss-1-usr", "96400"}, {"gpuss-0-usr", "99000"}, {"cpu-1-0-usr", "junk"}, {"battery", "30000"}};
        for (int i = 0; i < zones.length; ++i) {
            File z = new File(root, "thermal_zone" + i); z.mkdirs();
            Files.write(new File(z, "type").toPath(), (zones[i][0] + "\n").getBytes(StandardCharsets.UTF_8));
            Files.write(new File(z, "temp").toPath(), (zones[i][1] + "\n").getBytes(StandardCharsets.UTF_8));
        }
        check(near(AotBuild.hottestCpuC(root), 96.4, 1e-9), "hottest CPU zone (a GPU zone does not count)");
        check(Double.isNaN(AotBuild.hottestCpuC(new File(root, "none"))), "no thermal folder: NaN, the row is hidden");
        File cpus = Files.createTempDirectory("bbcpu").toFile();
        long[] khz = {1_800_000, 1_800_000, 2_800_000, 3_360_000};
        for (int i = 0; i < khz.length; ++i) {
            File d = new File(cpus, "cpu" + i + "/cpufreq"); d.mkdirs();
            Files.write(new File(d, "cpuinfo_max_freq").toPath(), (khz[i] + "\n").getBytes(StandardCharsets.UTF_8));
        }
        new File(cpus, "cpu4").mkdirs(); // no cpufreq: unreadable counts as 0
        new File(cpus, "cpufreq").mkdirs(); // not a core
        long[] read = AotBuild.maxFreqs(cpus);
        check(read.length == 5 && read[0] == 1_800_000 && read[3] == 3_360_000 && read[4] == 0, "core speeds");

        // ---- the work folder
        File work = Files.createTempDirectory("bbwork").toFile();
        for (int i = 0; i < 20; ++i) {
            String n = String.format("libbbaot.so.o.%03d.o", i);
            Files.write(new File(work, n).toPath(), new byte[]{1});
            if (i != 7) Files.write(new File(work, n + ".stat").toPath(), new byte[]{1}); // shard 7 has no .stat: not finished
        }
        Files.write(new File(work, "libbbaot.so.o.030.o.stat").toPath(), new byte[]{1}); // a .stat without its object: not finished
        Files.write(new File(work, "compile.txt").toPath(), new byte[]{1});
        check(AotBuild.shardsDone(work) == 19, "finished shards: object and .stat both there (" + AotBuild.shardsDone(work) + ")");
        check(AotBuild.percentDone(32) == 46 && AotBuild.percentDone(64) == 93 && AotBuild.percentDone(0) == 0 && AotBuild.shardsDone(new File(work, "none")) == 0, "percent of an unfinished build");

        // ---- the game files copied for the tool
        File game = Files.createTempDirectory("bbgame").toFile(), dest = Files.createTempDirectory("bbdest").toFile();
        for (String name : AotBuild.GAME_FILES) {
            File gf = new File(game, name); gf.getParentFile().mkdirs();
            Files.write(gf.toPath(), name.getBytes(StandardCharsets.UTF_8));
        }
        new File(game, "sce_module/libc.prx").delete();
        check("sce_module/libc.prx".equals(AotBuild.copyGameFiles(game, dest)), "a missing file is named");
        Files.write(new File(game, "sce_module/libc.prx").toPath(), "libc".getBytes(StandardCharsets.UTF_8));
        check(AotBuild.copyGameFiles(game, dest) == null && new File(dest, "eboot.bin").length() == 9 && new File(dest, "sce_sys/param.sfo").isFile(), "the four files are copied");
        long stamp = new File(dest, "eboot.bin").lastModified(); Thread.sleep(20);
        check(AotBuild.copyGameFiles(game, dest) == null && new File(dest, "eboot.bin").lastModified() == stamp, "files that are there with the same size are kept");

        System.out.println(checks + " checks, " + failed + " failed");
        if (failed != 0) System.exit(1);
    }
}
