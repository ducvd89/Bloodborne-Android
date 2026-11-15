package com.ducvd89.bloodborne;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.function.Consumer;

/** Removes what the former Linux (glibc) runtime left in the app's files directory when the app is updated:
 *  the Debian rootfs, the glibc game bundle, its scripts and the Linux GPU drivers. No Android classes, so it
 *  is tested on a desktop JDK (tests/android-mods).
 *
 *  Safety: only the fixed paths below (relative to files/) are deleted, links are removed without being
 *  followed (the old rootfs holds absolute symlinks that point out of the app's folder, to /system, /proc and
 *  so on), and a path whose parent folder resolves outside files/ is skipped. Saves (bbport/user), the
 *  settings (bbport.ini), mods.json, the game folders (shared storage, never under files/ here) and the
 *  Bionic GPU drivers (drivers/<name>/ with driver.lib) are never touched: PROTECTED is checked for every
 *  path before it is deleted. */
final class RuntimeCleanup {
    /** Paths of the Linux runtime, relative to the app's files directory. */
    static final String[] LEGACY = {
        "rootfs",                      // the Debian rootfs (FEX, Xvfb, Mesa, Python, ...)
        "bbport/arm64",                // the glibc game bundle: bin, lib, patch-parts (now bbport/patch-parts), lsfg
        "bbport/rootfs",               // a link to the rootfs's tmp, made by the old deploy script
        "bbport/scripts",              // the Python scripts (game preparation, mods)
        "bbport/bin", "bbport/lib",    // the x86-64 bundle of the first releases
        "bbport/run-thor.sh", "bbport/run-common.sh", "bbport/mods.sh",
        "bbport/thor-gamecontrollerdb.txt",
        "bbport/lsfg-vk.toml",         // written by the old launch script for the Lossless Scaling layer
        "drivers/.catalog.json",       // the list of Linux Turnip builds (the Android one is .catalog-android.json)
    };
    /** Never deleted, whatever LEGACY says: a path equal to one of these or below it. */
    static final String[] PROTECTED = {
        "bbport/user", "bbport/bbport.ini", "bbport/bbport.default.ini", "bbport/mods.json", "bbport/game",
        "bbport/data", "bbport/bionic", "bbport/patch-parts", "bbport/audio.fifo", "home",
        "bbport/Lossless.dll",         // the player's own copy (Steam sign-in or picker), for Lossless Scaling
    };

    private RuntimeCleanup() {}

    private static boolean isProtected(String relative) {
        for (String keep : PROTECTED) if (relative.equals(keep) || relative.startsWith(keep + "/")) return true;
        return false;
    }

    /** Whether something of the Linux runtime is still there. Cheap: a few stat calls and one folder listing. */
    static boolean pending(File files) {
        for (String relative : LEGACY) if (exists(new File(files, relative))) return true;
        return !linuxDrivers(files).isEmpty();
    }

    private static boolean exists(File file) { return file.exists() || Files.isSymbolicLink(file.toPath()); }

    /** The driver folders of the Linux runtime: an icd.json and no driver.lib (those with driver.lib are Android
     *  builds the Bionic runtime loads). */
    static List<String> linuxDrivers(File files) {
        List<String> out = new ArrayList<>();
        String[] names = new File(files, "drivers").list();
        if (names == null) return out;
        Arrays.sort(names);
        for (String name : names) {
            if (name.startsWith(".")) continue;
            File folder = new File(new File(files, "drivers"), name);
            if (!Files.isSymbolicLink(folder.toPath()) && folder.isDirectory()
                    && new File(folder, "icd.json").isFile() && !new File(folder, "driver.lib").exists())
                out.add("drivers/" + name);
        }
        return out;
    }

    /** Deletes everything pending. Failures are logged, not thrown: leftovers only take space, and the next start
     *  (pending() is still true) tries again. Returns the number of paths that could not be removed. */
    static int run(File files, Consumer<String> log) {
        List<String> targets = new ArrayList<>(Arrays.asList(LEGACY));
        targets.addAll(linuxDrivers(files));
        int failed = 0;
        for (String relative : targets) {
            if (isProtected(relative)) throw new IllegalStateException("protected path in the cleanup list: " + relative);
            File target = new File(files, relative);
            if (!exists(target)) continue;
            if (!parentInside(files, target)) { log.accept("Cleanup: skipped " + relative + " (outside the app's folder)"); ++failed; continue; }
            if (deleteTree(target)) log.accept("Cleanup: removed " + relative);
            else { log.accept("Cleanup: could not remove " + relative); ++failed; }
        }
        return failed;
    }

    /** The folder holding the target is inside files/ once links are resolved (a link in the middle of the path
     *  must not lead out of the app's folder). The target itself is not resolved: a link is removed as a link. */
    private static boolean parentInside(File files, File target) {
        try {
            File root = files.getCanonicalFile(), parent = target.getParentFile().getCanonicalFile();
            return parent.equals(root) || parent.getPath().startsWith(root.getPath() + File.separator);
        } catch (IOException e) { return false; }
    }

    /** Deletes a file or folder with everything in it, never following a link out of it (a link is deleted as a
     *  link). Read-only folders are made writable first. Returns whether nothing is left. */
    static boolean deleteTree(File file) {
        boolean ok = true;
        if (!Files.isSymbolicLink(file.toPath()) && file.isDirectory()) {
            file.setReadable(true, true); file.setWritable(true, true); file.setExecutable(true, true);
            File[] children = file.listFiles();
            if (children != null) for (File child : children) ok &= deleteTree(child);
            else ok = false;
        }
        return (file.delete() || !exists(file)) && ok;
    }
}
