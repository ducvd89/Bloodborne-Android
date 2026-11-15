package com.ducvd89.bloodborne;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;

/** A tiny test runner (no JUnit in this repository's offline toolchain): tests are lambdas; a failed assertion or an
 *  exception fails the test; the process exits 1 when any failed. */
final class T {
    interface Body { void run() throws Exception; }
    private static int passed, failed, skipped;
    private static final List<String> failures = new ArrayList<>();

    static void test(String name, Body body) {
        try { body.run(); ++passed; System.out.println("ok   " + name); }
        catch (Skip s) { ++skipped; System.out.println("skip " + name + ": " + s.getMessage()); }
        catch (Throwable t) {
            ++failed; failures.add(name);
            System.out.println("FAIL " + name + ": " + t);
            StackTraceElement[] st = t.getStackTrace();
            for (int i = 0; i < Math.min(st.length, 6); ++i) System.out.println("       at " + st[i]);
        }
    }
    static final class Skip extends RuntimeException { Skip(String m) { super(m); } }
    static void skip(String why) { throw new Skip(why); }

    static int finish() {
        System.out.println(passed + " passed, " + failed + " failed, " + skipped + " skipped");
        for (String f : failures) System.out.println("  failed: " + f);
        return failed == 0 ? 0 : 1;
    }

    static void eq(Object expected, Object actual) { eq(expected, actual, ""); }
    static void eq(Object expected, Object actual, String note) {
        if (!Objects.equals(expected, actual)) throw new AssertionError("expected <" + expected + "> but was <" + actual + ">" + (note.isEmpty() ? "" : " (" + note + ")"));
    }
    static void yes(boolean condition) { yes(condition, ""); }
    static void yes(boolean condition, String note) { if (!condition) throw new AssertionError("expected true" + (note.isEmpty() ? "" : ": " + note)); }
    static void no(boolean condition) { yes(!condition); }
    static void no(boolean condition, String note) { yes(!condition, note); }
    static void contains(String haystack, String needle) {
        if (haystack == null || !haystack.contains(needle)) throw new AssertionError("<" + haystack + "> does not contain <" + needle + ">");
    }

    // ---- files ----
    static File temp() throws IOException { return Files.createTempDirectory("bbmods").toFile(); }
    static void write(File f, byte[] data) throws IOException {
        f.getParentFile().mkdirs();
        Files.write(f.toPath(), data);
    }
    static void write(File f, String text) throws IOException { write(f, text.getBytes(StandardCharsets.UTF_8)); }
    static byte[] read(File f) throws IOException { return Files.readAllBytes(f.toPath()); }
    static String text(File f) throws IOException { return new String(read(f), StandardCharsets.UTF_8); }
    static List<String> names(File dir) {
        String[] n = dir.list();
        List<String> out = new ArrayList<>(n == null ? new ArrayList<String>() : Arrays.asList(n));
        java.util.Collections.sort(out);
        return out;
    }
    static void delete(File root) {
        try {
            Files.walkFileTree(root.toPath(), new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path p, BasicFileAttributes a) throws IOException { Files.delete(p); return FileVisitResult.CONTINUE; }
                @Override public FileVisitResult postVisitDirectory(Path p, IOException e) throws IOException { Files.delete(p); return FileVisitResult.CONTINUE; }
            });
        } catch (IOException ignored) {}
    }
    @SafeVarargs static <X> List<X> list(X... xs) { return Arrays.asList(xs); }
}
