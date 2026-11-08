package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.regex.Pattern;

/** Backup and restore of the game's save data folder (savedata/&lt;user id&gt;/&lt;title id&gt;/...). Plain
 *  folder copies, no Android classes beyond java.io/nio, so it is tested on a desktop JDK. */
final class SaveBackup {
    private static final Pattern USER=Pattern.compile("[0-9]+");
    private static final Pattern TITLE=Pattern.compile("[A-Z]{4}[0-9]{5}(\\.memory)?");
    private SaveBackup() {}

    /** The app's save data folder under the runtime's --user directory. */
    static File saves(File base) { return new File(new File(base,"user"),"savedata"); }

    /** Whether the folder holds at least one save: &lt;numeric user id&gt;/&lt;title id like CUSA03173&gt;. */
    static boolean looksLikeSavedata(File dir) {
        File[] users=dir==null ? null : dir.listFiles();
        if (users==null) return false;
        for (File user : users) {
            if (!user.isDirectory() || !USER.matcher(user.getName()).matches()) continue;
            File[] titles=user.listFiles();
            if (titles==null) continue;
            for (File title : titles) if (TITLE.matcher(title.getName()).matches()) return true;
        }
        return false;
    }
    /** The picked folder itself, or its "savedata" subfolder (a backup folder made by {@link #backup}), when it is
     *  save data; null otherwise. */
    static File find(File picked) {
        if (picked==null) return null;
        if (looksLikeSavedata(picked)) return picked;
        File inside=new File(picked,"savedata");
        return looksLikeSavedata(inside) ? inside : null;
    }
    static boolean hasSaves(File savedata) { return looksLikeSavedata(savedata); }

    /** Copies the saves to downloads/Bloodborne-saves-STAMP/savedata and returns that backup folder. */
    static File backup(File savedata,File downloads,Date when) throws IOException {
        String stamp=new SimpleDateFormat("yyyyMMdd-HHmmss",Locale.ROOT).format(when);
        File dest=new File(downloads,"Bloodborne-saves-"+stamp);
        for (int n=2; dest.exists(); n++) dest=new File(downloads,"Bloodborne-saves-"+stamp+"-"+n);
        File target=new File(dest,"savedata");
        try { copyTree(savedata,target); }
        catch (IOException | RuntimeException e) { deleteTree(dest); throw e; }
        return dest;
    }
    /** Replaces the saves with a copy of source: copied beside the folder first, so a failed copy leaves the
     *  current saves untouched. */
    static void restore(File savedata,File source) throws IOException {
        File parent=savedata.getParentFile();
        File fresh=new File(parent,"savedata.restoring"), old=new File(parent,"savedata.replaced");
        deleteTree(fresh); deleteTree(old);
        try { copyTree(source,fresh); }
        catch (IOException | RuntimeException e) { deleteTree(fresh); throw e; }
        if (savedata.exists() && !savedata.renameTo(old)) { deleteTree(fresh); throw new IOException("Cannot move the current saves aside"); }
        if (!fresh.renameTo(savedata)) {
            if (old.exists()) old.renameTo(savedata); // put the old saves back
            deleteTree(fresh);
            throw new IOException("Cannot put the restored saves in place");
        }
        deleteTree(old);
    }

    /** Where saves go for a picked folder: the folder itself when it is (or is named) savedata, else its "savedata" subfolder. */
    static File destinationFor(File picked) {
        return picked.getName().equals("savedata") || looksLikeSavedata(picked) ? picked : new File(picked,"savedata");
    }
    /** Whether a is b or inside b (canonical paths). */
    static boolean isInside(File a,File b) throws IOException {
        String x=a.getCanonicalPath(), y=b.getCanonicalPath();
        return x.equals(y) || x.startsWith(y.endsWith("/") ? y : y+"/");
    }
    /** Copies src into dst (merging) and checks that every file arrived with its size. */
    static void copyInto(File src,File dst) throws IOException {
        copyTree(src,dst);
        long[] a=count(src), b=count(dst);
        if (a[0]!=b[0] || a[1]!=b[1]) throw new IOException("The copy does not match the original ("+b[0]+" of "+a[0]+" files); nothing was changed.");
    }
    /** {files, bytes} under f. */
    static long[] count(File f) {
        long[] n={0,0};
        File[] children=f.isDirectory() ? f.listFiles() : null;
        if (children!=null) for (File c : children) { long[] m=count(c); n[0]+=m[0]; n[1]+=m[1]; }
        else if (f.isFile()) { n[0]=1; n[1]=f.length(); }
        return n;
    }
    static void copyTree(File from,File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("Cannot create "+to);
            File[] children=from.listFiles();
            if (children==null) throw new IOException("Cannot read "+from);
            for (File child : children) copyTree(child,new File(to,child.getName()));
            to.setLastModified(from.lastModified());
        } else {
            Files.copy(from.toPath(),to.toPath(),StandardCopyOption.REPLACE_EXISTING);
            to.setLastModified(from.lastModified());
        }
    }
    static void deleteTree(File f) {
        File[] children=f.isDirectory() && !Files.isSymbolicLink(f.toPath()) ? f.listFiles() : null;
        if (children!=null) for (File c : children) deleteTree(c);
        f.delete();
    }
}
