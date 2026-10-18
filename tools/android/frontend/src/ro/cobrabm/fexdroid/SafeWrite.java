package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.List;

/** Small state files (bbport.ini, lsfg.state, cheats.state, mods.json) that survive a hard reset: written to
 *  a temporary file in the same folder, synced to storage, renamed over the target, and the folder synced.
 *  Without the sync the page cache is lost on a power cut and the file comes back as NUL bytes (the Thor did
 *  that twice on 2026-10-18); readText and repair treat such a file as missing. No Android classes besides
 *  the log, so it is tested on a desktop JDK. */
final class SafeWrite {
    private SafeWrite() {}

    /** The bytes in `target` whole or the old ones, whatever happens to the device. */
    static void write(File target, byte[] data) throws IOException {
        File temporary=new File(target.getPath()+".tmp");
        try (FileOutputStream out=new FileOutputStream(temporary)) {
            out.write(data);
            out.flush(); out.getFD().sync();
        } catch (IOException e) { temporary.delete(); throw e; }
        try { Files.move(temporary.toPath(),target.toPath(),StandardCopyOption.ATOMIC_MOVE,StandardCopyOption.REPLACE_EXISTING); }
        catch (IOException | RuntimeException e) {
            if (!temporary.renameTo(target)) { temporary.delete(); throw new IOException("Cannot write "+target+": "+e); }
        }
        syncFolder(target.getAbsoluteFile().getParentFile());
    }
    static void write(File target, String text) throws IOException { write(target,text.getBytes(StandardCharsets.UTF_8)); }
    static void writeLines(File target, List<String> lines) throws IOException {
        StringBuilder text=new StringBuilder();
        for (String line:lines) text.append(line).append('\n');
        write(target,text.toString());
    }
    /** Makes the rename itself durable; best effort (not every file system lets a folder be opened). */
    private static void syncFolder(File folder) {
        if (folder==null) return;
        try (FileChannel c=FileChannel.open(folder.toPath(),StandardOpenOption.READ)) { c.force(true); }
        catch (Throwable ignored) {}
    }

    /** The file's content, null when it is missing or nothing but NUL bytes (an unsynced file after a hard
     *  reset); NUL bytes among real text are dropped. Logged. */
    static String readText(File file) throws IOException {
        if (!file.isFile()) return null;
        byte[] data=Files.readAllBytes(file.toPath());
        int nul=0;
        for (byte b:data) if (b==0) ++nul;
        if (nul==0) return new String(data,StandardCharsets.UTF_8);
        byte[] kept=new byte[data.length-nul];
        for (int i=0,j=0;i<data.length;++i) if (data[i]!=0) kept[j++]=data[i];
        String text=new String(kept,StandardCharsets.UTF_8);
        if (text.trim().isEmpty()) {
            log(file.getName()+" is "+data.length+" NUL bytes (a hard reset before it reached storage): taken as missing");
            return null;
        }
        log(file.getName()+": "+nul+" NUL bytes of "+data.length+" ignored (a hard reset before it reached storage)");
        return text;
    }
    /** The lines of readText; empty when it is missing. */
    static List<String> readLines(File file) throws IOException {
        List<String> out=new ArrayList<>();
        String text=readText(file);
        if (text==null) return out;
        for (String line:text.split("\r?\n",-1)) out.add(line);
        if (!out.isEmpty() && out.get(out.size()-1).isEmpty()) out.remove(out.size()-1);
        return out;
    }
    /** Before a start: a file with NUL bytes is deleted when that is all it holds (the defaults then apply,
     *  as for a missing one), else rewritten without them. True when it changed the file. */
    static boolean repair(File file) {
        try {
            if (!file.isFile()) return false;
            byte[] data=Files.readAllBytes(file.toPath());
            boolean nul=false;
            for (byte b:data) if (b==0) { nul=true; break; }
            if (!nul) return false;
            String text=readText(file);
            if (text==null) { if (!file.delete()) write(file,new byte[0]); }
            else write(file,text);
            return true;
        } catch (IOException e) { log("cannot repair "+file.getName()+": "+e); return false; }
    }

    private static void log(String message) {
        try { // by reflection: no Android classes at compile time, so a desktop JDK builds this file
            Class.forName("android.util.Log").getMethod("w",String.class,String.class).invoke(null,"Bloodborne",message);
        } catch (Throwable t) { System.err.println("Bloodborne: "+message); }
    }
}
