package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Enumeration;
import java.util.Locale;
import java.util.concurrent.CancellationException;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** The map pack of Hunter's Atlas (https://github.com/MikeTat1988/HuntersAtlas, GPL-2.0-or-later; this app's map is
 *  a port of its renderer): maps, art and feature data generated from the game, published as a release zip of this
 *  app. It is downloaded when the player turns the map on and lives in files/bbport/atlas/ (pack.json, maps/, art/).
 *  The zip is checked against ATLAS_PACK_SHA256 and unpacked into atlas.tmp, which is then renamed to atlas, so a
 *  stopped or failed install leaves no half pack. */
final class AtlasPack {
    private AtlasPack() {}

    /** Where the pack comes from and its SHA-256. "TODO": not published yet, the check is skipped (and logged). Pack version 2
     *  (atlas-maps-2) is version 1 plus the route graphs and gate tables of maps/nav/; atlas-maps-1 (v1, still readable:
     *  the map works on it without route finding) is no longer downloaded. */
    static final int PACK_VERSION=2;
    static final String ATLAS_PACK_URL="https://github.com/ducvd89/Bloodborne-Android/releases/download/atlas-maps-2/hunters-atlas-maps-2.zip";
    static final String ATLAS_PACK_SHA256="8d8c3f2a8aa46fbb4626ce0524a94a77d8c87fe7ce79b6c017bd889d99a84efc";

    private static final long MAX_UNPACKED=1536L*1024*1024;
    private static final int MAX_ENTRIES=20000;

    static File dir(File base) { return new File(base,"atlas"); }
    static boolean installed(File base) {
        File d=dir(base);
        return new File(d,"pack.json").isFile() && new File(d,"maps/atlas.json").isFile();
    }
    /** pack.json's pack_version of the installed pack (1 when it says none), 0 when nothing is installed. */
    static int installedVersion(File base) {
        if (!installed(base)) return 0;
        try {
            String text=new String(java.nio.file.Files.readAllBytes(new File(dir(base),"pack.json").toPath()),java.nio.charset.StandardCharsets.UTF_8);
            return new org.json.JSONObject(text).optInt("pack_version",1);
        } catch (Exception e) { return 1; }
    }
    /** A pack without route data is installed: the dialog offers the update. */
    static boolean updateAvailable(File base) { int v=installedVersion(base); return v>0 && v<PACK_VERSION; }
    /** Nothing usable is installed (none, or pack.json / maps/atlas.json missing) or the pack is older than this app expects (also a
     *  pack.json that cannot be read, which installedVersion counts as 1): the map pack should be (re)downloaded. */
    static boolean needsUpdate(File base) { return installedVersion(base)<PACK_VERSION; }
    /** The download's size for the player's notices (MB, rounded). */
    static final int APPROX_MB=34;
    static long size(File base) { return treeSize(dir(base)); }
    private static long treeSize(File f) {
        if (f.isFile()) return f.length();
        long n=0; File[] list=f.listFiles();
        if (list!=null) for (File c:list) n+=treeSize(c);
        return n;
    }
    static void delete(File base) {
        FsUtil.deleteTree(new File(base,"atlas.tmp"));
        FsUtil.deleteTree(dir(base));
    }

    interface Progress { void update(long done,long total); }

    /** One download and install; cancel() may be called from any thread. */
    static final class Install {
        private volatile boolean cancelled;
        private volatile HttpURLConnection connection;

        void cancel() {
            cancelled=true;
            HttpURLConnection c=connection;
            if (c!=null) { Thread t=new Thread(c::disconnect,"Atlas-cancel"); t.setDaemon(true); t.start(); }
        }
        private void check() { if (cancelled) throw new CancellationException(); }

        /** Downloads, verifies and unpacks the pack into base/atlas. Blocks. phase: "Downloading" / "Unpacking". */
        void run(File base,Progress download,Runnable unpacking) throws IOException {
            File zip=new File(base,"atlas.download.zip"), tmp=new File(base,"atlas.tmp");
            base.mkdirs();
            try {
                fetch(zip,download);
                check();
                if (unpacking!=null) unpacking.run();
                FsUtil.deleteTree(tmp);
                if (!tmp.mkdirs()) throw new IOException("Cannot create "+tmp);
                unzip(zip,tmp);
                check();
                if (!new File(tmp,"pack.json").isFile() || !new File(tmp,"maps/atlas.json").isFile())
                    throw new IOException("The map pack is incomplete (no pack.json or maps/atlas.json).");
                File dest=dir(base), old=new File(base,"atlas.old");
                FsUtil.deleteTree(old);
                if (dest.exists() && !dest.renameTo(old)) throw new IOException("Cannot replace the old map data");
                if (!tmp.renameTo(dest)) {
                    if (old.exists()) old.renameTo(dest);
                    throw new IOException("Cannot move the map data into place");
                }
                FsUtil.deleteTree(old);
            } finally {
                zip.delete();
                FsUtil.deleteTree(tmp);
            }
        }

        private void fetch(File zip,Progress progress) throws IOException {
            HttpURLConnection c=(HttpURLConnection)new URL(ATLAS_PACK_URL).openConnection();
            c.setConnectTimeout(15000); c.setReadTimeout(30000); c.setInstanceFollowRedirects(true);
            c.setRequestProperty("User-Agent","Bloodborne-Android map pack");
            connection=c;
            try {
                check();
                int code=c.getResponseCode();
                if (code!=200) throw new IOException("The download failed: GitHub answered "+code);
                long total=c.getContentLengthLong();
                MessageDigest hash;
                try { hash=MessageDigest.getInstance("SHA-256"); } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e.toString()); }
                long done=0, last=0;
                byte[] buffer=new byte[1<<16];
                try (InputStream in=c.getInputStream(); FileOutputStream out=new FileOutputStream(zip)) {
                    for (int n; (n=in.read(buffer))>0;) {
                        done+=n;
                        if (done>MAX_UNPACKED) throw new IOException("The download is too large");
                        out.write(buffer,0,n); hash.update(buffer,0,n);
                        long now=System.nanoTime();
                        if (now-last>150_000_000L) { last=now; progress.update(done,total); }
                        check();
                    }
                } catch (IOException e) {
                    if (cancelled) throw new CancellationException();
                    throw e;
                }
                check();
                if (total>0 && done!=total) throw new IOException("The download is incomplete ("+done+" of "+total+" bytes). Try again.");
                progress.update(done,total>0 ? total : done);
                StringBuilder hex=new StringBuilder();
                for (byte b:hash.digest()) hex.append(String.format(Locale.ROOT,"%02x",b&0xff));
                if (ATLAS_PACK_SHA256.equals("TODO"))
                    android.util.Log.w("Bloodborne","map pack SHA-256 not checked: ATLAS_PACK_SHA256 is the placeholder (the pack's is "+hex+")");
                else if (!hex.toString().equalsIgnoreCase(ATLAS_PACK_SHA256))
                    throw new IOException("The download is damaged (its checksum does not match). Try again.");
            } finally { c.disconnect(); }
        }

        /** Unpacks the zip into dir; entries of the root folder atlas/ lose it. Names that leave dir are refused. */
        private void unzip(File zipFile,File dir) throws IOException {
            String root=dir.getCanonicalPath()+File.separator;
            long total=0; int entries=0;
            try (ZipFile zip=new ZipFile(zipFile)) {
                byte[] buffer=new byte[1<<16];
                for (Enumeration<? extends ZipEntry> e=zip.entries();e.hasMoreElements();) {
                    check();
                    ZipEntry entry=e.nextElement();
                    String name=entry.getName();
                    if (++entries>MAX_ENTRIES) throw new IOException("The map pack has too many files");
                    if (name.isEmpty() || name.startsWith("/") || name.indexOf('\\')>=0 || name.indexOf('\0')>=0)
                        throw new IOException("The map pack has an unsafe file name: "+name);
                    for (String part:name.split("/")) if (part.equals("..")) throw new IOException("The map pack has an unsafe file name: "+name);
                    if (name.startsWith("atlas/")) name=name.substring(6);
                    else if (name.equals("atlas")) continue;
                    if (name.isEmpty()) continue;
                    File target=new File(dir,name);
                    if (!target.getCanonicalPath().startsWith(root)) throw new IOException("The map pack has an unsafe file name: "+entry.getName());
                    if (entry.isDirectory()) { target.mkdirs(); continue; }
                    File parent=target.getParentFile();
                    if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create "+parent);
                    try (InputStream in=zip.getInputStream(entry); OutputStream out=new FileOutputStream(target)) {
                        for (int n; (n=in.read(buffer))>0;) {
                            total+=n;
                            if (total>MAX_UNPACKED) throw new IOException("The map pack unpacks too large");
                            out.write(buffer,0,n);
                        }
                    }
                }
            }
        }
    }

    /** A short reason for the player ("Offline" when GitHub cannot be reached). */
    static String message(Exception e) {
        String m=DriverCatalog.message(e);
        return m.equals("Offline") ? "Offline — the map data could not be downloaded." : m;
    }
}
