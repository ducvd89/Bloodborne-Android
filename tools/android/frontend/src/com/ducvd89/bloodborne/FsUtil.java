package com.ducvd89.bloodborne;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;

/** File helpers of the mod and PKG code (no Android classes, so it is tested on a desktop JDK).
 *  They behave as the Python they replace (tools/android/mods_android.py, scripts/mods.py) did. */
final class FsUtil {
    private FsUtil() {}

    /** Python's str.casefold, close enough for file names ("ß" -> "ss"). */
    static String casefold(String s) { return s.toUpperCase(Locale.ROOT).toLowerCase(Locale.ROOT); }

    /** Orders by code points, as Python compares strings. */
    static int compare(String a, String b) {
        int i = 0, j = 0;
        while (i < a.length() && j < b.length()) {
            int x = a.codePointAt(i), y = b.codePointAt(j);
            if (x != y) return x < y ? -1 : 1;
            i += Character.charCount(x); j += Character.charCount(y);
        }
        return Integer.compare(a.length() - i, b.length() - j);
    }
    /** sorted(names, key=lambda n: (n.casefold(), n)). */
    static final Comparator<String> FOLDED = new Comparator<String>() {
        @Override public int compare(String a, String b) {
            int c = FsUtil.compare(casefold(a), casefold(b));
            return c != 0 ? c : FsUtil.compare(a, b);
        }
    };
    static final Comparator<String> CODEPOINTS = new Comparator<String>() {
        @Override public int compare(String a, String b) { return FsUtil.compare(a, b); }
    };

    /** The names in a folder, sorted by code points (a deterministic order where Python used the OS's); empty when
     *  it is not a folder. */
    static List<String> names(File dir) {
        String[] list = dir.list();
        List<String> out = new ArrayList<>();
        if (list != null) Collections.addAll(out, list);
        Collections.sort(out, CODEPOINTS);
        return out;
    }

    static boolean isSymlink(File f) { return Files.isSymbolicLink(f.toPath()); }
    /** Path.resolve(): the real path; where the platform cannot resolve a part of it (Android's shared storage
     *  shows /storage/emulated/0 but not always /storage/emulated), the path made absolute and normalized. */
    static File resolve(File f) {
        Path p = f.toPath().toAbsolutePath();
        try { return p.toRealPath().toFile(); }
        catch (IOException e) {
            p = p.normalize();
            // Follow the links of the last part by hand (those the app made point at absolute paths).
            for (int hops = 0; hops < 40 && Files.isSymbolicLink(p); ++hops) {
                try { p = p.getParent().resolve(Files.readSymbolicLink(p)).normalize(); }
                catch (IOException x) { break; }
            }
            return p.toFile();
        }
    }
    /** resolve(strict=True): the path must exist (a dangling link fails). */
    static File resolveStrict(File f) throws IOException {
        if (!f.exists()) throw new java.nio.file.NoSuchFileException(f.getPath());
        return resolve(f);
    }

    /** `name` inside `folder`, matching an existing entry case-insensitively (mods made on Windows often differ
     *  from the game's lower-case names); else folder/name. */
    static File child(File folder, String name) {
        if (folder.isDirectory()) {
            String folded = casefold(name);
            for (String entry : names(folder)) if (casefold(entry).equals(folded)) return new File(folder, entry);
        }
        return new File(folder, name);
    }

    /** Deletes a file or folder with everything in it, never following a link out of it. */
    static void deleteTree(File file) {
        if (file == null) return;
        if (!isSymlink(file) && file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) for (File c : children) deleteTree(c);
        }
        file.delete();
    }

    /** What Python's OSError.strerror would say, from a Java exception ("No such file or directory"). */
    static String strerror(IOException e) {
        String m = e.getMessage();
        if (m == null) return e.toString();
        int open = m.lastIndexOf(" ("), close = m.lastIndexOf(')');
        if (open >= 0 && close == m.length() - 1 && close > open) return m.substring(open + 2, close);
        return m;
    }

    /** Space free for this app where `dir` is (statvfs's f_bavail * f_frsize, as shutil.disk_usage). */
    static long freeSpace(File dir) { return dir.getUsableSpace(); }

    /** Python's str.strip(): Unicode whitespace at both ends. */
    static String strip(String s) {
        int a = 0, b = s.length();
        while (a < b && isSpace(s.charAt(a))) ++a;
        while (b > a && isSpace(s.charAt(b - 1))) --b;
        return s.substring(a, b);
    }
    private static boolean isSpace(char c) {
        return Character.isWhitespace(c) || Character.isSpaceChar(c) || c == 0x85;
    }
}
