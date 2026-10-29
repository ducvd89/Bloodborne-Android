// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import android.content.Context;
import android.content.res.AssetManager;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/** The Rebuild Tool's files the APK carries (assets/recomp/: bundle/, variants/ and VERSION, from the tool's Android package; the program
 *  itself is lib/arm64-v8a/libbbrecomp.so). The tool reads them as plain files, so they are copied out of the APK once into
 *  files/bbport/recomp/<tool version>/ (again when the APK carries another build of the tool). */
final class RecompAssets {
    private RecompAssets() {}
    static final String ASSET = "recomp", PROGRAM = "libbbrecomp.so";

    /** The VERSION file of the package ("bb-recomp 1.2.0", "git ...", "abi_version 2", ...), or null when this APK has none. */
    static String versionText(Context c) {
        try (InputStream in = c.getAssets().open(ASSET + "/VERSION")) { return new String(readAll(in), StandardCharsets.UTF_8); }
        catch (IOException e) { return null; }
    }
    /** The tool's version number from that text ("1.2.0"), or null. */
    static String version(String versionText) {
        if (versionText == null) return null;
        for (String line : versionText.split("\n")) {
            String t = line.trim();
            if (t.startsWith("bb-recomp ")) { String v = t.substring(10).trim(); if (v.matches("[0-9][0-9A-Za-z._-]*")) return v; }
        }
        return null;
    }
    /** variants/CATALOG_HASH of the APK (the catalog hash of the catalogs it ships, one hex line), or null when this APK has none. */
    static String catalogHash(Context c) {
        try (InputStream in = c.getAssets().open(ASSET + "/variants/CATALOG_HASH")) {
            String t = new String(readAll(in), StandardCharsets.UTF_8).trim();
            int nl = t.indexOf('\n');
            if (nl >= 0) t = t.substring(0, nl).trim();
            return t.matches("[0-9A-Fa-f]+") ? t.toLowerCase(java.util.Locale.ROOT) : null;
        } catch (IOException e) { return null; }
    }
    /** The optional shard manifest the APK carries (recomp/manifest/*.json, made on a PC), copied out next to bundle/: its path, or null. */
    static File manifest(File recompRoot) {
        File[] f = new File(recompRoot, "manifest").listFiles((d, n) -> n.endsWith(".json"));
        if (f == null || f.length == 0) return null;
        java.util.Arrays.sort(f);
        return f[0];
    }

    /** The program: the APK's extracted native library. Null when it is not there (no arm64 build). */
    static File program(Context c) {
        File f = new File(c.getApplicationInfo().nativeLibraryDir, PROGRAM);
        return f.isFile() ? f : null;
    }

    /** Copies bundle/ and variants/ out of the APK when they are not there yet; returns the folder (it holds bundle/ and variants/). Blocking. */
    static synchronized File ensure(Context c, File base) throws IOException {
        String text = versionText(c), version = version(text);
        if (version == null) throw new IOException("This APK has no Rebuild Tool files (assets/recomp).");
        File root = new File(new File(base, "recomp"), version), marker = new File(root, ".installed");
        AssetManager am = c.getAssets();
        if (marker.isFile() && text.equals(SafeWrite.readText(marker)) && complete(am, root)) return root;
        FsUtil.deleteTree(root);
        for (String dir : new String[]{"bundle", "variants"}) {
            String[] names = am.list(ASSET + "/" + dir);
            if (names == null || names.length == 0) throw new IOException("This APK's Rebuild Tool files are incomplete (" + dir + ").");
            File out = new File(root, dir);
            if (!out.isDirectory() && !out.mkdirs()) throw new IOException("Cannot create " + out);
            for (String name : names) copy(am, ASSET + "/" + dir + "/" + name, new File(out, name));
        }
        String[] manifests = am.list(ASSET + "/manifest"); // optional
        if (manifests != null && manifests.length > 0) {
            File out = new File(root, "manifest");
            if (!out.isDirectory() && !out.mkdirs()) throw new IOException("Cannot create " + out);
            for (String name : manifests) copy(am, ASSET + "/manifest/" + name, new File(out, name));
        }
        SafeWrite.write(marker, text);
        return root;
    }

    private static boolean complete(AssetManager am, File root) {
        try {
            for (String dir : new String[]{"bundle", "variants"}) {
                String[] names = am.list(ASSET + "/" + dir);
                if (names == null || names.length == 0) return false;
                for (String name : names) {
                    long size;
                    try (android.content.res.AssetFileDescriptor fd = am.openFd(ASSET + "/" + dir + "/" + name)) { size = fd.getLength(); }
                    catch (IOException compressed) { continue; } // a compressed asset has no length without reading it: the marker vouches for it
                    if (new File(new File(root, dir), name).length() != size) return false;
                }
            }
            return true;
        } catch (IOException e) { return false; }
    }

    private static void copy(AssetManager am, String asset, File dst) throws IOException {
        File part = new File(dst.getPath() + ".part");
        try (InputStream in = am.open(asset); FileOutputStream out = new FileOutputStream(part)) {
            byte[] buf = new byte[1 << 16]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush(); out.getFD().sync();
        }
        if (!part.renameTo(dst)) { part.delete(); throw new IOException("Cannot write " + dst); }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192]; int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toByteArray();
    }
}
