package com.ducvd89.bloodborne;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.RandomAccessFile;
import java.math.BigDecimal;
import java.math.RoundingMode;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** PS4 fake PKG installs for the app: unpacks a PKG with ps4-pkg-tool (a Bionic program run as a process) and either
 *  keeps its dvdroot_ps4 as a mod (1.09 data mods only) or installs it as the game (base game "gd" or update "gp",
 *  merged into DIR/<TITLE_ID>). Java port of tools/android/mods_android.py's install-pkg and install-game; no
 *  Android classes, so it is tested on a desktop JDK.
 *
 *  The installs never throw for a problem the player should read: they return a result whose `problem` is the text.
 *  A cancelled install throws CancellationException after removing what it had made. */
final class PkgInstaller {
    /** `progress <bytesDone> <bytesTotal> <label>` (total 0: unknown). */
    interface Progress { void progress(long done, long total, String label); }
    /** Free space where a folder is (replaced in tests). */
    interface Space { long free(File dir); }
    /** The game check after an install (replaced in tests). */
    interface Checker {
        GameCheck.Found problem(File game) throws Exception;
        String explain(GameCheck.Found found);
    }

    static final class ModResult {
        boolean ok; String name, problem; ModManager.Mod mod;
        /** Set (with ok false, problem the same text) when the PKG did not match its own checksums: the file is
         *  damaged or incomplete. The text lists the parts that are damaged. */
        String damaged;
    }
    static final class GameResult {
        boolean ok, ready; String gameDir, titleId, category, appVer, next, problem;
        /** As ModResult.damaged. */
        String damaged;
    }

    /** A problem the app shows to the player (the message is the text). */
    static class Failure extends Exception { Failure(String message) { super(message); } }
    /** The PKG does not match its checksums (the message lists the damaged parts). */
    static final class Damaged extends Failure { Damaged(String message) { super(message); } }

    static final long MIB = 1L << 20, GIB = 1L << 30;
    static final long MOD_MARGIN = 256 * MIB, GAME_MARGIN = GIB; // free space wanted beyond the extracted size
    static final String MOD_VERSION = "01.09";
    static final String ENCRYPTED = "This PKG is encrypted for a retail PS4 and can't be read; only homebrew (fake) PKGs are supported";
    /** Moved last by an update: an interrupted one still reads as the old version. */
    private static final Set<String> LAST_NAMES = new HashSet<>(java.util.Arrays.asList("eboot.bin", "sce_sys", "param.sfo"));
    private static final Pattern PROGRESS = Pattern.compile("Progress-bytes: (\\d+) (\\d+)");

    private final File tool;
    private final Consumer<String> log;
    Space space = new Space() { @Override public long free(File dir) { return FsUtil.freeSpace(dir); } };
    Checker checker = new Checker() {
        @Override public GameCheck.Found problem(File game) { return GameCheck.problem(game, GameCheck.skipped()); }
        @Override public String explain(GameCheck.Found found) { return GameCheck.explain(found); }
    };

    /** tool: ps4-pkg-tool (files/bbport/bionic/bin/ps4-pkg-tool); log: its output and other notes. */
    PkgInstaller(File tool, Consumer<String> log) { this.tool = tool; this.log = log != null ? log : new Consumer<String>() { @Override public void accept(String s) {} }; }

    static String gigabytes(long size) {
        return new BigDecimal((double)size / GIB).setScale(1, RoundingMode.HALF_EVEN).toPlainString() + " GB";
    }

    // ---- The PKG header ----

    /** The PKG's header facts. */
    static final class Header {
        String contentId; long contentType, size; Map<String, Object> sfo;
    }

    /** Reads a PKG's header (big-endian): content id, size, and its unencrypted param.sfo entry (sfo null when it
     *  cannot be read). Failure for a file that is not a complete PS4 PKG. */
    static Header readPkg(File path) throws Failure {
        try {
            long fileSize = path.length();
            if (!path.isFile()) throw new IOException(path + " (No such file or directory)");
            try (RandomAccessFile file = new RandomAccessFile(path, "r")) {
                byte[] head = new byte[0x440];
                int got = 0;
                for (int n; got < head.length && (n = file.read(head, got, head.length - got)) > 0; ) got += n;
                if (got < 4 || head[0] != 0x7f || head[1] != 'C' || head[2] != 'N' || head[3] != 'T') throw new Failure("This is not a PS4 PKG file");
                if (got < 0x440) throw new Failure("This PKG file is incomplete");
                Header info = new Header();
                int end = 0x40;
                while (end < 0x64 && head[end] != 0) ++end;
                info.contentId = new String(head, 0x40, end - 0x40, java.nio.charset.Charset.forName("ISO-8859-1"));
                info.contentType = be32(head, 0x74);
                info.size = be64(head, 0x430);
                if (Long.compareUnsigned(info.size, fileSize) > 0) throw new Failure("This PKG file is incomplete (the download or copy was cut short)");
                long count = be32(head, 0x10), table = be32(head, 0x18);
                try {
                    file.seek(table);
                    for (long i = 0; i < Math.min(count, 4096); ++i) {
                        byte[] entry = new byte[32];
                        int read = 0;
                        for (int n; read < 32 && (n = file.read(entry, read, 32 - read)) > 0; ) read += n;
                        if (read < 32) break;
                        long identifier = be32(entry, 0), offset = be32(entry, 16), size = be32(entry, 20);
                        if (identifier == 0x1000 && size > 0 && size <= MIB) { // param.sfo
                            long here = file.getFilePointer();
                            file.seek(offset);
                            byte[] data = new byte[(int)size];
                            int r = 0;
                            for (int n; r < data.length && (n = file.read(data, r, data.length - r)) > 0; ) r += n;
                            info.sfo = Sfo.parse(r == data.length ? data : java.util.Arrays.copyOf(data, r));
                            file.seek(here);
                            break;
                        }
                    }
                } catch (Sfo.Invalid | IOException e) {
                    info.sfo = null;
                }
                return info;
            }
        } catch (IOException e) {
            throw new Failure("Can't read the PKG: " + FsUtil.strerror(e));
        }
    }
    private static long be32(byte[] d, int at) { return ((d[at] & 0xffL) << 24) | ((d[at + 1] & 0xffL) << 16) | ((d[at + 2] & 0xffL) << 8) | (d[at + 3] & 0xffL); }
    private static long be64(byte[] d, int at) { return (be32(d, at) << 32) | be32(d, at + 4); }

    // ---- Checking for damage ----

    /** Checks the PKG against the SHA-256 digests it carries (PkgVerifier) before anything is unpacked, as the
     *  progress label PkgVerifier.LABEL. Damaged gives the parts that do not match. The check never blocks an
     *  install by failing itself: an error other than a read error is logged and the PKG goes on. */
    private void verifyPkg(File pkg, Progress progress, BooleanSupplier cancelled) throws Failure {
        PkgVerifier.Report report;
        try {
            progress.progress(0, 0, PkgVerifier.LABEL);
            report = PkgVerifier.verify(pkg, progress, cancelled);
        } catch (CancellationException e) {
            throw e;
        } catch (IOException e) {
            throw new Failure("Can't read the PKG: " + FsUtil.strerror(e));
        } catch (RuntimeException e) {
            log.accept("PKG check skipped: " + describe(e));
            return;
        }
        log.accept("PKG check: " + report.bytesRead + " bytes read, matched " + report.matched + ", damaged " + report.damaged.size());
        if (!report.ok()) throw new Damaged(report.describe());
    }

    // ---- Staging ----

    /** Deletes the .staging-* folders (left by an install that was killed) of `parent`. */
    static void removeStaging(File parent) {
        String[] names = parent.list();
        if (names != null) for (String n : names) if (n.startsWith(".staging-")) FsUtil.deleteTree(new File(parent, n));
    }
    private static String randomHex() {
        byte[] b = new byte[4];
        new java.security.SecureRandom().nextBytes(b);
        StringBuilder s = new StringBuilder();
        for (byte x : b) s.append(String.format(Locale.ROOT, "%02x", x & 0xff));
        return s.toString();
    }
    private static File newStaging(File parent) throws IOException {
        while (true) {
            File staging = new File(parent, ".staging-" + randomHex());
            try { Files.createDirectory(staging.toPath()); return staging; }
            catch (java.nio.file.FileAlreadyExistsException e) { /* another name */ }
        }
    }
    /** The folder the tool made in `staging` (<TITLE_ID>), or null. */
    static File extractedRoot(File staging) {
        List<File> folders = new ArrayList<>();
        for (String n : FsUtil.names(staging)) {
            File f = new File(staging, n);
            if (f.isDirectory() && !FsUtil.isSymlink(f)) folders.add(f);
        }
        return folders.size() == 1 ? folders.get(0) : null;
    }

    // ---- The tool ----

    /** Runs ps4-pkg-tool PKG STAGING, relaying its `Progress-bytes: done total` lines as progress. Failure when it
     *  fails or the disk is too small (judged when it reports the total). Cancelling kills it. */
    private void runTool(File pkg, File staging, long margin, File spaceDir, Progress progress, BooleanSupplier cancelled) throws Failure {
        if (tool == null || !tool.isFile() || !tool.canExecute())
            throw new Failure("The PKG tool is missing from this installation (ps4-pkg-tool)");
        Deque<String> tail = new ArrayDeque<>();
        boolean checked = false;
        ProcessBuilder builder = new ProcessBuilder(tool.getPath(), pkg.getPath(), staging.getPath()).redirectErrorStream(true);
        // A Bionic program: no inherited library paths.
        builder.environment().remove("LD_LIBRARY_PATH");
        builder.environment().remove("LD_PRELOAD");
        final Process process;
        try { process = builder.start(); }
        catch (IOException e) { throw new Failure("Can't run the PKG tool: " + FsUtil.strerror(e)); }
        try { process.getOutputStream().close(); } catch (IOException ignored) {}
        // Cancel stops the tool, which ends the read below. SIGTERM first, SIGKILL if it hangs.
        Thread watch = new Thread(new Runnable() {
            @Override public void run() {
                while (process.isAlive()) {
                    if (cancelled.getAsBoolean()) {
                        process.destroy();
                        try { if (!process.waitFor(5, java.util.concurrent.TimeUnit.SECONDS)) process.destroyForcibly(); }
                        catch (InterruptedException e) { process.destroyForcibly(); }
                        return;
                    }
                    try { Thread.sleep(100); } catch (InterruptedException e) { return; }
                }
            }
        }, "PkgTool-cancel");
        watch.setDaemon(true);
        watch.start();
        int code;
        try {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream(), "UTF-8"))) {
                for (String line; (line = reader.readLine()) != null; ) {
                    Matcher match = PROGRESS.matcher(line);
                    long done = 0, total = 0;
                    boolean parsed = false;
                    if (match.matches()) {
                        try { done = Long.parseLong(match.group(1)); total = Long.parseLong(match.group(2)); parsed = true; }
                        catch (NumberFormatException e) { parsed = false; }
                    }
                    if (!parsed) {
                        if (!line.trim().isEmpty()) {
                            if (tail.size() >= 12) tail.removeFirst();
                            tail.addLast(line);
                            log.accept(line);
                        }
                        continue;
                    }
                    if (!checked && total != 0) {
                        checked = true;
                        long free = space.free(spaceDir);
                        if (free < total + margin) {
                            process.destroyForcibly();
                            throw new Failure("Not enough free space: " + gigabytes(total + margin) + " needed (the files take "
                                + gigabytes(total) + "), " + gigabytes(free) + " available");
                        }
                    }
                    progress.progress(done, total, "Extracting");
                }
            } catch (IOException e) {
                if (!cancelled.getAsBoolean()) throw new Failure("Can't run the PKG tool: " + FsUtil.strerror(e));
            }
            try { code = process.waitFor(); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new CancellationException(); }
        } finally {
            if (process.isAlive()) {
                process.destroyForcibly();
                try { process.waitFor(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
            }
            watch.interrupt();
        }
        if (cancelled.getAsBoolean()) throw new CancellationException();
        if (code != 0) {
            StringBuilder text = new StringBuilder();
            for (String l : tail) { if (text.length() > 0) text.append('\n'); text.append(l); }
            if (space.free(spaceDir) < 16 * MIB) throw new Failure("Not enough free space to unpack the PKG");
            String all = text.toString();
            if (all.contains("file size is different") || all.contains("bigger than pkg size"))
                throw new Failure("This PKG file is damaged or incomplete");
            throw new Failure(ENCRYPTED);
        }
    }

    private static String sha256(File path) throws IOException {
        MessageDigest digest;
        try { digest = MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
        try (java.io.InputStream in = new java.io.FileInputStream(path)) {
            byte[] buffer = new byte[1 << 20];
            for (int n; (n = in.read(buffer)) > 0; ) digest.update(buffer, 0, n);
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) hex.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return hex.toString();
    }

    /** True when the extracted file is byte-identical to the game's (case-insensitive path). */
    private static boolean sameAsGame(File extracted, File game, String relative) throws IOException {
        File target = game;
        for (String part : relative.split("/")) target = FsUtil.child(target, part);
        if (!target.isFile() || target.length() != extracted.length()) return false;
        return sha256(extracted).equals(sha256(target));
    }

    private static Map<String, Object> extractedSfo(File root) {
        try { return Sfo.parse(Files.readAllBytes(new File(new File(root, "sce_sys"), "param.sfo").toPath())); }
        catch (IOException | Sfo.Invalid e) { return null; }
    }

    /** Why the PKG is not for Bloodborne 1.09 (null when it is). Without param.sfo only the content ID can tell
     *  the title. */
    static String modIdentityProblem(Map<String, Object> sfo, String contentId) {
        String title = Sfo.text(sfo, "TITLE_ID");
        if (title == null) title = contentId.length() > 7 ? contentId.substring(7, Math.min(16, contentId.length())) : "";
        String version = Sfo.text(sfo, "APP_VER");
        if (!ModLoader.SERIALS.contains(title) || (version != null && !version.equals(MOD_VERSION)))
            return "This PKG is for " + title + " version " + (version == null ? "?" : version) + "; only Bloodborne 1.09 mods are supported";
        return null;
    }

    static String validModName(String name) throws Failure {
        name = FsUtil.strip(name);
        boolean bad = name.isEmpty() || name.startsWith(".") || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0;
        for (int i = 0; !bad && i < name.length(); ++i) if (name.charAt(i) < 32) bad = true;
        if (!bad) { try { bad = name.getBytes("UTF-8").length > 200; } catch (java.io.UnsupportedEncodingException e) { bad = true; } }
        if (bad) throw new Failure("This name can't be used as a folder name");
        return name;
    }

    /** Creates <modsDir>/<name>, or '<name> (2)', ... (case-insensitively unique) -> its path. */
    private static File makeModFolder(File modsDir, String name) throws IOException {
        Set<String> taken = new HashSet<>();
        for (String n : FsUtil.names(modsDir)) taken.add(FsUtil.casefold(n));
        int number = 1;
        while (true) {
            String candidate = number == 1 ? name : name + " (" + number + ")";
            ++number;
            if (taken.contains(FsUtil.casefold(candidate))) continue;
            File folder = new File(modsDir, candidate);
            try { Files.createDirectory(folder.toPath()); return folder; }
            catch (java.nio.file.FileAlreadyExistsException e) { /* the next number */ }
        }
    }

    private static String describe(Exception e) { return e.getClass().getSimpleName() + ": " + e.getMessage(); }

    // ---- install-pkg ----

    /** Unpacks a PS4 fake PKG and keeps its dvdroot_ps4 as the mod `name` (made unique) in `modsDir`. `game` (may be
     *  null) is the game folder an eboot.bin in the PKG is compared with. */
    ModResult installMod(File pkg, String name, File modsDir, File game, Progress progress, BooleanSupplier cancelled) {
        return installMod(pkg, name, modsDir, game, progress, cancelled, true);
    }

    /** As above; `check` false skips the check of the PKG against its own checksums ("Install anyway"). A damaged
     *  PKG is not installed: the result has `damaged` (and `problem`) set. */
    ModResult installMod(File pkg, String name, File modsDir, File game, Progress progress, BooleanSupplier cancelled, boolean check) {
        ModResult result = new ModResult();
        File[] staging = {null};
        try {
            installMod(result, staging, pkg, name, modsDir, game, progress, cancelled, check);
        } catch (Damaged damage) {
            result.damaged = result.problem = damage.getMessage();
        } catch (Failure failure) {
            result.problem = failure.getMessage();
        } catch (CancellationException e) {
            throw e;
        } catch (Exception e) { // the app shows the message; never a stack trace
            result.problem = "Install failed: " + describe(e);
        } finally {
            if (staging[0] != null) FsUtil.deleteTree(staging[0]);
        }
        return result;
    }

    private void installMod(ModResult result, File[] state, File pkg, String rawName, File modsDir, File game,
                            Progress progress, BooleanSupplier cancelled, boolean check) throws Exception {
        String name = validModName(rawName);
        Header info = readPkg(pkg);
        String early = modIdentityProblem(info.sfo, info.contentId);
        if (early != null) throw new Failure(early);
        if (!modsDir.isDirectory() && !modsDir.mkdirs()) throw new Failure("Can't create the mods folder: " + (modsDir.exists() ? "File exists" : "Permission denied"));
        removeStaging(modsDir);
        long need = info.size + MOD_MARGIN;
        if (space.free(modsDir) < need)
            throw new Failure("Not enough free space: " + gigabytes(need) + " needed, " + gigabytes(space.free(modsDir)) + " available");
        if (check) verifyPkg(pkg, progress, cancelled);
        File staging = state[0] = newStaging(modsDir);
        runTool(pkg, staging, MOD_MARGIN, modsDir, progress, cancelled);
        progress.progress(0, 0, "Checking");
        File root = extractedRoot(staging);
        Map<String, Object> sfo = root != null ? extractedSfo(root) : null;
        if (root == null || sfo == null) throw new Failure(info.sfo != null ? ENCRYPTED : "This PKG has no param.sfo");
        early = modIdentityProblem(sfo, info.contentId);
        if (early != null) throw new Failure(early);
        List<String> changed = new ArrayList<>();
        List<String> candidates = new ArrayList<>();
        candidates.add("eboot.bin");
        File modules = FsUtil.child(root, "sce_module");
        List<String> moduleFiles = new ArrayList<>();
        listFiles(modules, root, moduleFiles);
        Collections.sort(moduleFiles, new Comparator<String>() { // Path ordering: by components
            @Override public int compare(String a, String b) {
                String[] x = a.split("/"), y = b.split("/");
                for (int i = 0; i < Math.min(x.length, y.length); ++i) { int c = FsUtil.compare(x[i], y[i]); if (c != 0) return c; }
                return Integer.compare(x.length, y.length);
            }
        });
        candidates.addAll(moduleFiles);
        for (String relative : candidates) {
            File source = relative.equals("eboot.bin") ? FsUtil.child(root, "eboot.bin") : new File(root, relative);
            if (!source.isFile()) continue;
            if (game == null || !game.isDirectory() || !new File(game, "eboot.bin").isFile())
                throw new Failure("This PKG contains the game's executable, so it has to be compared with your game's: choose the game folder first");
            if (!sameAsGame(source, game, relative)) changed.add(relative);
        }
        if (changed.contains("eboot.bin")) throw new Failure("This PKG changes the game's executable (eboot.bin), which isn't supported");
        if (!changed.isEmpty()) throw new Failure("This PKG changes the game's modules (" + changed.get(0) + "), which isn't supported");
        File data = FsUtil.child(root, "dvdroot_ps4");
        if (!data.isDirectory()) throw new Failure("This PKG has no game data files (dvdroot_ps4)");
        File target = makeModFolder(modsDir, name);
        try {
            Files.move(data.toPath(), new File(target, "dvdroot_ps4").toPath());
            ModManager.Mod row = new ModManager(modsDir, null).inspectNew(target.getName());
            if (!row.valid || row.files == 0)
                throw new Failure(row.problem != null ? row.problem : "This PKG has no game data files (dvdroot_ps4)");
            result.ok = true; result.name = target.getName(); result.problem = null; result.mod = row;
        } catch (Throwable t) {
            FsUtil.deleteTree(target);
            throw t;
        }
    }
    private static void listFiles(File dir, File root, List<String> out) {
        for (String n : FsUtil.names(dir)) {
            File f = new File(dir, n);
            if (f.isDirectory()) listFiles(f, root, out);
            else if (f.isFile()) out.add(ModLoader.relative(root, f));
        }
    }

    // ---- install-game ----

    private static long treeBytes(File folder) {
        long total = 0;
        File[] entries = folder.listFiles();
        if (entries != null) for (File e : entries) {
            try {
                if (FsUtil.isSymlink(e) || !e.isDirectory()) total += Files.readAttributes(e.toPath(), java.nio.file.attribute.BasicFileAttributes.class,
                    java.nio.file.LinkOption.NOFOLLOW_LINKS).size();
                else total += treeBytes(e);
            } catch (IOException ignored) {}
        }
        return total;
    }

    private interface Moved { void moved(long size); }

    /** Moves the files of `source` over the folder `target` (rename each, replacing; same filesystem). Names match
     *  the existing ones case-insensitively; eboot.bin and sce_sys go last. `moved(size)` after each file. */
    private static void mergeTree(File source, File target, Moved moved, BooleanSupplier cancelled) throws IOException {
        if (FsUtil.isSymlink(target) || (target.exists() && !target.isDirectory())) Files.delete(target.toPath());
        if (!target.isDirectory() && !target.mkdir()) throw new IOException("Cannot create " + target);
        // One listing per folder: an update's ~13,000 files took many minutes over Android's shared storage when
        // every name listed the folder.
        Map<String, String> existing = new HashMap<>();
        for (String n : FsUtil.names(target)) if (!existing.containsKey(FsUtil.casefold(n))) existing.put(FsUtil.casefold(n), n);
        List<String> entries = FsUtil.names(source);
        Collections.sort(entries, new Comparator<String>() {
            @Override public int compare(String a, String b) {
                boolean x = LAST_NAMES.contains(FsUtil.casefold(a)), y = LAST_NAMES.contains(FsUtil.casefold(b));
                if (x != y) return x ? 1 : -1;
                return FsUtil.compare(a, b);
            }
        });
        for (String name : entries) {
            File entry = new File(source, name);
            String there = existing.get(FsUtil.casefold(name));
            File destination = new File(target, there != null ? there : name);
            if (entry.isDirectory() && !FsUtil.isSymlink(entry)) {
                mergeTree(entry, destination, moved, cancelled);
            } else {
                if (cancelled.getAsBoolean()) throw new CancellationException();
                long size = Files.readAttributes(entry.toPath(), java.nio.file.attribute.BasicFileAttributes.class,
                    java.nio.file.LinkOption.NOFOLLOW_LINKS).size();
                if (destination.isDirectory() && !FsUtil.isSymlink(destination)) FsUtil.deleteTree(destination);
                Files.move(entry.toPath(), destination.toPath(), StandardCopyOption.ATOMIC_MOVE);
                if (moved != null) moved.moved(size);
            }
        }
    }

    /** `progress` for mergeTree: bytes moved of `total`, every 32 MiB and at the end. */
    private static final class MoveProgress implements Moved {
        static final long STEP = 32 * MIB;
        final long total; final String label; final Progress progress; long done, shown = -1;
        MoveProgress(long total, String label, Progress progress) {
            this.total = total; this.label = label; this.progress = progress;
            progress.progress(0, total, label);
        }
        @Override public void moved(long size) {
            done += size;
            if (done - shown >= STEP || done >= total) {
                shown = done;
                progress.progress(Math.min(done, total), total, label);
            }
        }
    }

    /** {parent, game}: where the staging goes and the folder of the game. `dest` is the game folder itself (it holds
     *  eboot.bin, or is named like a serial) or the folder that holds it. */
    private static File[] gameLayout(File dest, String title) throws Failure {
        if (new File(dest, "eboot.bin").isFile() || ModLoader.SERIALS.contains(dest.getName())) {
            if (ModLoader.SERIALS.contains(dest.getName()) && !dest.getName().equals(title))
                throw new Failure("This PKG is for " + title + ", but the folder " + dest.getName() + " is for another release");
            File parent = dest.getAbsoluteFile().getParentFile();
            return new File[]{parent, dest};
        }
        return new File[]{dest, new File(dest, title)};
    }

    /** Unpacks a game PKG (base game "gd" or update "gp") into dest/<TITLE_ID> (dest may be that folder itself),
     *  merging an update into the installed game. */
    GameResult installGame(File pkg, File dest, boolean replace, Progress progress, BooleanSupplier cancelled) {
        return installGame(pkg, dest, replace, progress, cancelled, true);
    }

    /** As above; `check` false skips the check of the PKG against its own checksums ("Install anyway"). A damaged
     *  PKG is not installed: the result has `damaged` (and `problem`) set. */
    GameResult installGame(File pkg, File dest, boolean replace, Progress progress, BooleanSupplier cancelled, boolean check) {
        GameResult result = new GameResult();
        File[] state = {null, null};
        try {
            installGame(result, state, pkg, dest, replace, progress, cancelled, check);
            result.ok = true;
        } catch (Damaged damage) {
            result.damaged = result.problem = damage.getMessage();
        } catch (Failure failure) {
            result.problem = failure.getMessage();
        } catch (CancellationException e) {
            throw e;
        } catch (Exception e) {
            result.problem = "Install failed: " + describe(e);
        } finally {
            for (File f : state) if (f != null) FsUtil.deleteTree(f);
        }
        return result;
    }

    private String[] identify(GameResult result, Map<String, Object> sfo, String contentId) throws Failure {
        String title = Sfo.text(sfo, "TITLE_ID");
        if (title == null) title = contentId.length() > 7 ? contentId.substring(7, Math.min(16, contentId.length())) : "";
        String category = Sfo.text(sfo, "CATEGORY");
        result.titleId = title;
        result.category = "gd".equals(category) || "gp".equals(category) ? category : null;
        result.appVer = Sfo.text(sfo, "APP_VER");
        if (!ModLoader.SERIALS.contains(title)) throw new Failure("This PKG is for " + title + ", which isn't a Bloodborne release bbport supports");
        if (!"gd".equals(category) && !"gp".equals(category))
            throw new Failure("This PKG is " + (category == null ? "unknown" : category) + " content; only the game (gd) and its update (gp) can be installed");
        return new String[]{title, category};
    }

    private void installGame(GameResult result, File[] state, File pkg, File dest, boolean replace, Progress progress,
                             BooleanSupplier cancelled, boolean check) throws Exception {
        Header info = readPkg(pkg);
        if (info.sfo == null) throw new Failure("This PKG is damaged: its param.sfo can't be read");
        String[] id = identify(result, info.sfo, info.contentId);
        String title = id[0], category = id[1];
        File[] layout = gameLayout(dest, title);
        File parent = layout[0], game = layout[1];
        boolean installed = new File(game, "eboot.bin").isFile();
        if (installed) {
            String have = null;
            try { have = Sfo.text(Sfo.parse(Files.readAllBytes(new File(new File(game, "sce_sys"), "param.sfo").toPath())), "TITLE_ID"); }
            catch (IOException | Sfo.Invalid ignored) {}
            if (have != null && !have.equals(title)) throw new Failure("The game in " + game.getName() + " is " + have + ", but this PKG is for " + title);
        }
        if (category.equals("gd") && installed && !replace) throw new Failure("A game is already installed there");
        if (category.equals("gp") && !installed)
            throw new Failure("There is no game to update in " + game.getPath() + ": install the base game PKG for " + title + " first");
        if (!parent.isDirectory() && !parent.mkdirs()) throw new Failure("Can't create the folder: " + (parent.exists() ? "File exists" : "Permission denied"));
        removeStaging(parent);
        long need = info.size + GAME_MARGIN;
        if (space.free(parent) < need)
            throw new Failure("Not enough free space: " + gigabytes(need) + " needed, " + gigabytes(space.free(parent)) + " available");
        if (check) verifyPkg(pkg, progress, cancelled);
        File staging = state[0] = newStaging(parent);
        runTool(pkg, staging, GAME_MARGIN, parent, progress, cancelled);
        progress.progress(0, 0, "Installing");
        File root = extractedRoot(staging);
        Map<String, Object> sfo = root != null ? extractedSfo(root) : null;
        if (root == null || sfo == null) throw new Failure(info.sfo != null ? ENCRYPTED : "This PKG has no param.sfo");
        String[] again = identify(result, sfo, info.contentId);
        if (!again[0].equals(title) || !again[1].equals(category)) throw new Failure("The PKG changed while it was unpacked");
        if (category.equals("gd")) {
            File aside = null;
            if (game.exists() || FsUtil.isSymlink(game)) {
                aside = state[1] = new File(parent, ".staging-old-" + randomHex());
                Files.move(game.toPath(), aside.toPath());
            }
            try { Files.move(root.toPath(), game.toPath()); }
            catch (IOException e) {
                if (aside != null) Files.move(aside.toPath(), game.toPath());
                throw e;
            }
            result.gameDir = game.getPath();
        } else {
            mergeTree(root, game, new MoveProgress(treeBytes(root), "Installing", progress), cancelled);
            result.gameDir = game.getPath();
        }
        progress.progress(0, 0, "Checking");
        GameCheck.Found found;
        try { found = checker.problem(game); }
        catch (Exception e) { throw new Failure("Couldn't check the game files: " + describe(e)); }
        if (found == null) result.ready = true;
        else if (found.kind.equals("missing_update")) result.next = "Install the 1.09 update PKG for " + title;
        else throw new Failure(checker.explain(found));
    }
}
