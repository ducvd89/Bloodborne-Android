package com.ducvd89.bloodborne;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CancellationException;
import java.util.function.BooleanSupplier;
import org.apache.commons.compress.archivers.sevenz.SevenZArchiveEntry;
import org.apache.commons.compress.archivers.sevenz.SevenZFile;
import org.apache.commons.compress.archivers.zip.ZipArchiveEntry;
import org.apache.commons.compress.archivers.zip.ZipFile;

/** Unpacks a mod archive (.zip or .7z, by its first bytes) into a new folder. No Android classes, so
 *  it runs on a desktop JDK for testing. Every entry is checked before anything is written: absolute
 *  paths, "..", links and anything that would land outside the folder are refused, and the declared
 *  sizes must fit the limits and the free space. A failed or cancelled unpack removes the folder. */
final class ModArchive {
    /** Bytes written so far (or copied) out of the total (-1: unknown). */
    interface Progress { void update(long done,long total); }
    /** The archive is not usable (the message says why, for the player). */
    static final class Rejected extends IOException { Rejected(String message) { super(message); } }
    static final class Result {
        final int files; final long bytes;
        Result(int files,long bytes) { this.files=files; this.bytes=bytes; }
    }

    static final String UNSUPPORTED="Only .zip, .7z and .pkg mods are supported";
    static final long MAX_BYTES=8L<<30;
    static final int MAX_ENTRIES=200000;
    /** Room left on the volume after unpacking. */
    private static final long SPARE=16L<<20;
    private static final int BUFFER=64<<10;

    private ModArchive() {}

    /** "zip", "7z" or "pkg" (a PS4 package, unpacked by the runtime) by the first bytes, else null. */
    static String kind(File file) {
        byte[] head=new byte[6];
        try (FileInputStream in=new FileInputStream(file)) {
            int n=0, r;
            while (n<head.length && (r=in.read(head,n,head.length-n))>0) n+=r;
            if (n>=4 && head[0]=='P' && head[1]=='K' && head[2]==3 && head[3]==4) return "zip";
            if (n>=4 && head[0]==0x7F && head[1]=='C' && head[2]=='N' && head[3]=='T') return "pkg";
            if (n>=6 && head[0]=='7' && head[1]=='z' && (head[2]&0xff)==0xBC && (head[3]&0xff)==0xAF
                    && head[4]==0x27 && head[5]==0x1C) return "7z";
        } catch (IOException ignored) {}
        return null;
    }

    /** A folder name for the mod from the archive's file name: no extension, no path characters. */
    static String modName(String archiveName) {
        String n=archiveName==null ? "" : archiveName.trim();
        int slash=Math.max(n.lastIndexOf('/'),n.lastIndexOf('\\'));
        if (slash>=0) n=n.substring(slash+1);
        String lower=n.toLowerCase(Locale.ROOT);
        if (lower.endsWith(".zip") || lower.endsWith(".7z") || lower.endsWith(".pkg")) n=n.substring(0,n.lastIndexOf('.'));
        n=n.replaceAll("[\\p{Cntrl}/\\\\:*?\"<>|]","_").trim();
        while (n.startsWith(".")) n=n.substring(1).trim();
        return n.isEmpty() ? "Mod" : n;
    }
    /** parent/base, or parent/base (2), (3)... when taken (also by a file, or differing in case only). */
    static File uniqueDir(File parent,String base) {
        File dir=new File(parent,base);
        for (int i=2;taken(parent,dir.getName());++i) dir=new File(parent,base+" ("+i+")");
        return dir;
    }
    private static boolean taken(File parent,String name) {
        String[] existing=parent.list();
        if (existing!=null) for (String e:existing) if (e.equalsIgnoreCase(name)) return true;
        return false;
    }

    /** Copies a stream to a file (the picked archive, for random access). Total -1: unknown. */
    static void copy(InputStream in,File out,long total,Progress progress,BooleanSupplier cancelled) throws IOException {
        byte[] buffer=new byte[BUFFER];
        long done=0, last=0;
        try (OutputStream o=new FileOutputStream(out)) {
            for (int n; (n=in.read(buffer))>0;) {
                if (cancelled.getAsBoolean()) throw new CancellationException();
                o.write(buffer,0,n); done+=n;
                long now=System.nanoTime();
                if (now-last>100_000_000L) { last=now; progress.update(done,total); }
            }
        }
        progress.update(done,total);
    }

    /** One entry that will be written. */
    private static final class Item {
        final String path; final boolean dir; final long size; final ZipArchiveEntry zip;
        Item(String path,boolean dir,long size,ZipArchiveEntry zip) {
            this.path=path; this.dir=dir; this.size=size; this.zip=zip;
        }
        /** The stored CRC-32 to verify, or -1 (7z checks its own). */
        long crc() { return zip==null ? -1 : zip.getCrc(); }
    }

    /** Unpacks `archive` into `target` (which must not exist yet) with the default limits.
     *  free: bytes free where target is (-1: unchecked). Throws Rejected for an unusable archive,
     *  CancellationException when cancelled. On any failure `target` is removed. */
    static Result extract(File archive,File target,long free,Progress progress,BooleanSupplier cancelled) throws IOException {
        return extract(archive,target,free,MAX_BYTES,MAX_ENTRIES,progress,cancelled);
    }
    static Result extract(File archive,File target,long free,long maxBytes,int maxEntries,Progress progress,
                          BooleanSupplier cancelled) throws IOException {
        String kind=kind(archive);
        if (!"zip".equals(kind) && !"7z".equals(kind)) throw new Rejected(UNSUPPORTED);
        if (target.exists()) throw new Rejected(target.getName()+" already exists.");
        boolean ok=false;
        try {
            Result result="zip".equals(kind) ? unzip(archive,target,free,maxBytes,maxEntries,progress,cancelled)
                                             : un7z(archive,target,free,maxBytes,maxEntries,progress,cancelled);
            ok=true; return result;
        } catch (CancellationException | Rejected e) {
            throw e;
        } catch (OutOfMemoryError e) {
            throw new Rejected("Not enough memory to unpack this archive.");
        } catch (IOException e) {
            String m=String.valueOf(e.getMessage());
            throw new Rejected(m.contains("No space left") ? "The phone ran out of storage space." : "Could not unpack the archive: "+m);
        } catch (RuntimeException e) { // corrupt data that a decoder trips over
            throw new Rejected("The archive is damaged or not supported ("+e+").");
        } finally {
            if (!ok) deleteTree(target);
        }
    }

    /** Checks one entry name and returns its normalized relative path ("" for the root). */
    static String safePath(String raw) throws Rejected {
        String n=raw.replace('\\','/');
        if (n.indexOf('\0')>=0) throw new Rejected("The archive has an invalid file name.");
        if (n.startsWith("/") || n.matches("(?s)^[A-Za-z]:.*")) throw new Rejected("The archive has an absolute path ("+raw+").");
        StringBuilder out=new StringBuilder();
        for (String part:n.split("/")) {
            if (part.isEmpty() || part.equals(".")) continue;
            if (part.equals("..")) throw new Rejected("The archive has a path that leaves the mod folder ("+raw+").");
            if (out.length()>0) out.append('/');
            out.append(part);
        }
        return out.toString();
    }
    /** Junk the Mac Finder puts in zips. */
    private static boolean junk(String path) {
        return path.equals("__MACOSX") || path.startsWith("__MACOSX/") || path.endsWith("/.DS_Store") || path.equals(".DS_Store");
    }
    private static void checkTotals(int entries,int files,long total,long free,long maxBytes,int maxEntries) throws Rejected {
        if (files==0) throw new Rejected("The archive has no files in it.");
        if (entries>maxEntries) throw new Rejected("The archive has too many files ("+entries+").");
        if (total>maxBytes) throw new Rejected("The archive is too large to unpack ("+megabytes(total)+").");
        if (free>=0 && total+SPARE>free)
            throw new Rejected("Not enough free space: the mod needs "+megabytes(total)+" and "+megabytes(Math.max(0,free))+" is free.");
    }
    static String megabytes(long bytes) {
        if (bytes>=1L<<30) return String.format(Locale.ROOT,"%.1f GB",bytes/1073741824.0);
        return Math.max(1,Math.round(bytes/1048576.0))+" MB";
    }

    /** Where the entry goes; refuses a destination that does not stay inside the target folder. */
    private static File destination(File target,String canonical,String path) throws IOException {
        File dest=new File(target,path);
        if (!dest.getCanonicalPath().startsWith(canonical)) throw new Rejected("The archive has a path that leaves the mod folder ("+path+").");
        return dest;
    }

    private static final class Writer {
        final File target; final String canonical; final long declared; final Progress progress; final BooleanSupplier cancelled;
        final byte[] buffer=new byte[BUFFER];
        long done, last; int files;
        Writer(File target,long declared,Progress progress,BooleanSupplier cancelled) throws IOException {
            if (!target.mkdirs()) throw new IOException("Cannot create "+target);
            this.target=target; canonical=target.getCanonicalPath()+File.separator; this.declared=declared;
            this.progress=progress; this.cancelled=cancelled;
        }
        void directory(Item item) throws IOException {
            File d=destination(target,canonical,item.path);
            if (!d.isDirectory() && !d.mkdirs()) throw new IOException("Cannot create "+item.path);
        }
        void file(Item item,InputStream in) throws IOException {
            File d=destination(target,canonical,item.path), parent=d.getParentFile();
            if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create "+parent.getName());
            java.util.zip.CRC32 crc=new java.util.zip.CRC32();
            try (OutputStream out=new FileOutputStream(d)) {
                if (in!=null) for (int n; (n=in.read(buffer))>0;) {
                    if (cancelled.getAsBoolean()) throw new CancellationException();
                    // The sizes in the headers must be true: a bomb declares little and inflates a lot.
                    if ((done+=n)>declared) throw new Rejected("The archive's sizes do not match its contents.");
                    out.write(buffer,0,n); crc.update(buffer,0,n);
                    long now=System.nanoTime();
                    if (now-last>100_000_000L) { last=now; progress.update(done,declared); }
                }
            }
            // The zip decoder does not check it; a damaged download would otherwise unpack as garbage.
            if (item.crc()>=0 && crc.getValue()!=item.crc()) throw new Rejected("The archive is damaged ("+item.path+" fails its checksum).");
            ++files;
        }
    }

    private static Result unzip(File archive,File target,long free,long maxBytes,int maxEntries,Progress progress,
                                BooleanSupplier cancelled) throws IOException {
        try (ZipFile zip=ZipFile.builder().setFile(archive).get()) {
            List<Item> items=new ArrayList<>();
            long total=0; int files=0;
            for (Enumeration<ZipArchiveEntry> e=zip.getEntries();e.hasMoreElements();) {
                ZipArchiveEntry entry=e.nextElement();
                if (entry.isUnixSymlink()) throw new Rejected("The archive has a link ("+entry.getName()+"), which is not allowed.");
                String path=safePath(entry.getName());
                if (path.isEmpty() || junk(path)) continue;
                if (!entry.isDirectory()) {
                    if (!zip.canReadEntryData(entry)) throw new Rejected("The archive is encrypted or uses an unsupported method ("+path+").");
                    if (entry.getSize()<0) throw new Rejected("The archive is damaged ("+path+").");
                    total+=entry.getSize(); ++files;
                }
                items.add(new Item(path,entry.isDirectory(),entry.isDirectory() ? 0 : entry.getSize(),entry));
                if (items.size()>maxEntries || total>maxBytes) break; // reported below
            }
            checkTotals(items.size(),files,total,free,maxBytes,maxEntries);
            Writer w=new Writer(target,total,progress,cancelled);
            for (Item item:items) {
                if (item.dir) { w.directory(item); continue; }
                try (InputStream in=zip.getInputStream(item.zip)) { w.file(item,in); }
            }
            progress.update(w.done,total);
            return new Result(w.files,w.done);
        }
    }

    private static Result un7z(File archive,File target,long free,long maxBytes,int maxEntries,Progress progress,
                               BooleanSupplier cancelled) throws IOException {
        try (SevenZFile sz=SevenZFile.builder().setFile(archive).get()) {
            List<Item> items=new ArrayList<>(); // one per archive entry, in stream order (null: skipped)
            long total=0; int count=0, files=0;
            for (SevenZArchiveEntry entry:sz.getEntries()) {
                if (isLink(entry)) throw new Rejected("The archive has a link ("+entry.getName()+"), which is not allowed.");
                String path=safePath(entry.getName());
                if (entry.isAntiItem() || path.isEmpty() || junk(path)) { items.add(null); continue; }
                long size=entry.isDirectory() ? 0 : entry.getSize();
                if (size<0) throw new Rejected("The archive is damaged ("+path+").");
                total+=size; ++count;
                if (!entry.isDirectory()) ++files;
                items.add(new Item(path,entry.isDirectory(),size,null));
                if (count>maxEntries || total>maxBytes) break;
            }
            checkTotals(count,files,total,free,maxBytes,maxEntries);
            Writer w=new Writer(target,total,progress,cancelled);
            for (Item item:items) {
                SevenZArchiveEntry entry=sz.getNextEntry();
                if (entry==null) throw new Rejected("The archive is damaged.");
                if (item==null) continue;
                if (item.dir) { w.directory(item); continue; }
                // Solid blocks are read in order; an entry without a stream is an empty file.
                w.file(item,entry.hasStream() ? new java.io.InputStream() {
                    final byte[] one=new byte[1];
                    @Override public int read() throws IOException { int n=sz.read(one,0,1); return n<=0 ? -1 : one[0]&0xff; }
                    @Override public int read(byte[] b,int off,int len) throws IOException { return sz.read(b,off,len); }
                } : null);
            }
            progress.update(w.done,total);
            return new Result(w.files,w.done);
        }
    }
    /** A symlink (Unix mode in the high attribute bits) or a Windows reparse point. */
    private static boolean isLink(SevenZArchiveEntry entry) {
        if (!entry.getHasWindowsAttributes()) return false;
        int a=entry.getWindowsAttributes();
        return (a&0x400)!=0 || ((a&0x8000)!=0 && ((a>>>16)&0170000)==0120000);
    }

    /** Deletes a folder with everything in it, never following a link out of it. */
    static void deleteTree(File file) {
        if (file==null) return;
        if (!Files.isSymbolicLink(file.toPath()) && file.isDirectory()) {
            File[] children=file.listFiles();
            if (children!=null) for (File c:children) deleteTree(c);
        }
        file.delete();
    }
}
