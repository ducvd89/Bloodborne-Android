package ro.cobrabm.fexdroid;

import android.content.Context;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.FileVisitResult;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.security.MessageDigest;
import java.util.function.Consumer;

/** Installs the signed APK's Linux runtime in this app's private data directory. */
final class RuntimeInstaller {
    private static void deleteTree(File root) throws Exception {
        if (!root.exists()) return;
        Files.walkFileTree(root.toPath(), new SimpleFileVisitor<java.nio.file.Path>() {
            @Override public FileVisitResult visitFile(java.nio.file.Path path, BasicFileAttributes attrs)
                    throws java.io.IOException {
                Files.delete(path);
                return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(java.nio.file.Path path, java.io.IOException error)
                    throws java.io.IOException {
                if (error != null) throw error;
                Files.delete(path);
                return FileVisitResult.CONTINUE;
            }
        });
    }

    private static byte[] readAll(InputStream in) throws Exception {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
        return out.toByteArray();
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format("%02x", b & 255));
        return out.toString();
    }

    static void ensure(Context context, File files, Consumer<String> status) throws Exception {
        String expected;
        try (InputStream in = context.getAssets().open("runtime.sha256")) {
            expected = new String(readAll(in), StandardCharsets.US_ASCII).trim();
        } catch (java.io.FileNotFoundException missing) {
            if (new File(files, "rootfs/usr/bin/Xvfb").canExecute() &&
                    new File(files, "bbport/arm64/bin/bb-probe").canExecute()) return;
            throw new IllegalStateException("This APK does not contain the runtime", missing);
        }
        if (!expected.matches("[0-9a-f]{64}")) throw new IllegalStateException("Invalid runtime digest");
        File marker = new File(files, ".runtime.sha256");
        if (marker.isFile() && expected.equals(new String(Files.readAllBytes(marker.toPath()), StandardCharsets.US_ASCII).trim()) &&
                new File(files, "rootfs/usr/bin/Xvfb").canExecute() &&
                new File(files, "bbport/arm64/bin/bb-probe").canExecute()) return;

        status.accept("Installing Bloodborne runtime…");
        File archive = new File(files, "runtime.tar.gz.part");
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        try {
            try (InputStream in = context.getAssets().open("runtime.payload");
                 FileOutputStream out = new FileOutputStream(archive)) {
                byte[] buffer = new byte[1024 * 1024];
                int count;
                while ((count = in.read(buffer)) != -1) {
                    sha.update(buffer, 0, count);
                    out.write(buffer, 0, count);
                }
                out.getFD().sync();
            }
            if (!expected.equals(hex(sha.digest()))) throw new IllegalStateException("Runtime archive checksum mismatch");
            // Older installs have absolute links out of the rootfs. Android's tar safely
            // rejects overwriting them, so replace only the bundled runtime tree.
            deleteTree(new File(files, "rootfs"));
            Process tar = new ProcessBuilder("/system/bin/tar", "-oxzf", archive.getAbsolutePath(),
                    "-C", files.getAbsolutePath()).redirectErrorStream(true).start();
            byte[] log;
            try (InputStream in = tar.getInputStream()) { log = readAll(in); }
            if (tar.waitFor() != 0) throw new IllegalStateException("Runtime extraction failed: " +
                    new String(log, 0, Math.min(log.length, 2048), StandardCharsets.UTF_8));
            if (!new File(files, "rootfs/usr/bin/Xvfb").canExecute() ||
                    !new File(files, "bbport/arm64/bin/bb-probe").canExecute())
                throw new IllegalStateException("Runtime archive is incomplete");
            Files.write(marker.toPath(), (expected + "\n").getBytes(StandardCharsets.US_ASCII));
        } finally {
            archive.delete();
        }
    }

    private RuntimeInstaller() {}
}
