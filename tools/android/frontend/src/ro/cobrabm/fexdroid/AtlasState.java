package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** The live state of the game for the map: the player's place and the event flags the map's features read.
 *  The game process writes bbport/atlas.shm (8192 bytes, little endian; BB_ATLAS_FILE); this class maps it read
 *  only and reads it with its sequence number (odd while the game writes; a read that saw it change is retried):
 *    0 u32 magic 0x54414242 ("BBAT"), 4 u32 version 1, 8 u32 seq, 12 u32 valid, 16 f32 x, 20 f32 y, 24 f32 z,
 *    28 f32 yaw, 32 u32 block, 36 u32 menu_open, 40 u32 tick, 44 u32 flags_gen, 48 u32 flag_count,
 *    64.. i8 flags[] (-1 unreadable, 0, 1) in the order of bbport/atlas.flags.
 *  The app writes atlas.flags ("gen N", then one flag id per line) before it starts the game (BB_ATLAS_FLAGS_FILE);
 *  the values count only when flags_gen equals N. Main thread only. */
final class AtlasState implements AtlasFeatures.Flags {
    static final int SIZE=8192, FLAGS_AT=64, MAX_FLAGS=SIZE-FLAGS_AT;

    // the last consistent read
    boolean mapped, valid, menuOpen;
    float x, y, z, yaw;
    int block, tick, flagsGen, flagCount;
    private final byte[] flags=new byte[MAX_FLAGS];
    /** Bumped when the place or the flags changed since the previous poll() that returned true. */
    int flagsVersion;

    private final File file;
    private int gen; private int[] ids=new int[0]; private final Map<Integer,Integer> index=new HashMap<>();
    private ByteBuffer map; private FileChannel channel; private Object key;
    private long lastOpen;
    private boolean flagBytesChanged;
    private final byte[] scratch=new byte[MAX_FLAGS];

    AtlasState(File file) { this.file=file; }

    /** The flags the game was asked for (in this order) and their generation. */
    void expect(int gen,List<Integer> flagIds) {
        this.gen=gen;
        int n=Math.min(flagIds.size(),MAX_FLAGS);
        ids=new int[n]; index.clear();
        for (int i=0;i<n;++i) { ids[i]=flagIds.get(i); index.put(ids[i],i); }
        reset();
    }

    /** A new request for the running game's file (the pack was updated while it runs): like expect() but the mapping and the
     *  place stay (only the flag values are unknown until the game applies the new generation). */
    void expectLive(int gen,List<Integer> flagIds) {
        this.gen=gen;
        int n=Math.min(flagIds.size(),MAX_FLAGS);
        ids=new int[n]; index.clear();
        for (int i=0;i<n;++i) { ids[i]=flagIds.get(i); index.put(ids[i],i); }
        java.util.Arrays.fill(flags,(byte)-1); flagCount=0; ++flagsVersion;
    }

    /** Drops the mapping and what was read (a new game process makes a new file). */
    void reset() {
        close(); mapped=false; valid=false; menuOpen=false; tick=0; flagsGen=0; flagCount=0; ++flagsVersion;
        java.util.Arrays.fill(flags,(byte)-1);
    }
    void close() {
        map=null; key=null;
        if (channel!=null) { try { channel.close(); } catch (IOException ignored) {} channel=null; }
    }

    /** -1 unknown, 0, 1: the value of a flag of the list, once the game applied this generation. */
    @Override public int get(int id) {
        Integer i=index.get(id);
        if (i==null || flagsGen!=gen || i>=flagCount) return -1;
        return flags[i];
    }
    /** The flag snapshot (a copy that stays as it is). */
    AtlasFeatures.Flags snapshot() {
        final byte[] copy=flags.clone(); final int count=flagCount; final boolean ok=flagsGen==gen;
        final Map<Integer,Integer> idx=new HashMap<>(index);
        return id -> { Integer i=idx.get(id); return i==null || !ok || i>=count ? -1 : copy[i]; };
    }

    /** Reads the file when it can. True when something changed (the tick, the validity, a flag). */
    boolean poll() {
        long now=System.nanoTime();
        if (map==null) {
            if (now-lastOpen<1_000_000_000L) return false; // retried every second
            lastOpen=now;
            if (!open()) return false;
        } else if (now-lastOpen>2_000_000_000L) {
            lastOpen=now; // the game may have made a new file
            try {
                Object k=java.nio.file.Files.readAttributes(file.toPath(),java.nio.file.attribute.BasicFileAttributes.class).fileKey();
                if (k!=null && key!=null && !k.equals(key)) { close(); mapped=false; return true; }
            } catch (Throwable ignored) {}
        }
        boolean was=mapped, wasValid=valid;
        int oldTick=tick, oldGen=flagsGen, oldCount=flagCount; float ox=x, oy=y, oz=z, oyaw=yaw; int oldBlock=block;
        flagBytesChanged=false;
        if (!read()) return false;
        boolean flagsChanged=oldGen!=flagsGen || oldCount!=flagCount || flagBytesChanged;
        if (flagsChanged) ++flagsVersion;
        return !was || wasValid!=valid || oldTick!=tick || flagsChanged || ox!=x || oy!=y || oz!=z || oyaw!=yaw || oldBlock!=block;
    }

    private boolean open() {
        try {
            if (!file.isFile() || file.length()<SIZE) return false;
            FileChannel c=FileChannel.open(file.toPath(),StandardOpenOption.READ);
            ByteBuffer b=c.map(FileChannel.MapMode.READ_ONLY,0,SIZE).order(ByteOrder.LITTLE_ENDIAN);
            if (b.getInt(0)!=0x54414242 || b.getInt(4)!=1) { c.close(); return false; }
            channel=c; map=b;
            try { key=java.nio.file.Files.readAttributes(file.toPath(),java.nio.file.attribute.BasicFileAttributes.class).fileKey(); }
            catch (Throwable e) { key=null; }
            return true;
        } catch (IOException | RuntimeException e) { return false; }
    }

    private boolean read() {
        ByteBuffer b=map; if (b==null) return false;
        for (int attempt=0;attempt<8;++attempt) {
            int seq=b.getInt(8);
            if ((seq&1)!=0) continue;
            int v=b.getInt(12); float fx=b.getFloat(16), fy=b.getFloat(20), fz=b.getFloat(24), fyaw=b.getFloat(28);
            int blk=b.getInt(32), menu=b.getInt(36), tk=b.getInt(40), fg=b.getInt(44), fc=b.getInt(48);
            int n=Math.max(0,Math.min(fc,Math.min(ids.length,MAX_FLAGS)));
            byte[] tmp=scratch;
            for (int i=0;i<n;++i) tmp[i]=b.get(FLAGS_AT+i);
            if (b.getInt(8)!=seq) continue;
            mapped=true; valid=v!=0; x=fx; y=fy; z=fz; yaw=fyaw; block=blk; menuOpen=menu!=0; tick=tk; flagsGen=fg; flagCount=n;
            for (int i=0;i<MAX_FLAGS;++i) { byte w=i<n ? tmp[i] : (byte)-1; if (flags[i]!=w) { flags[i]=w; flagBytesChanged=true; } }
            return true;
        }
        return false; // the game was writing every time: the next poll
    }

    // ---- atlas.flags ----

    /** Writes the list for the game ("gen N", then one id per line), whole and synced. The ids beyond the room the
     *  state file has are dropped. */
    static void writeFlags(File target,int gen,List<Integer> ids) throws IOException {
        StringBuilder s=new StringBuilder("gen ").append(Integer.toUnsignedString(gen)).append('\n');
        int n=0;
        for (int id:ids) { if (n++>=MAX_FLAGS) break; s.append(id).append('\n'); }
        SafeWrite.write(target,s.toString());
    }
    static List<Integer> limited(java.util.Collection<Integer> ids) {
        List<Integer> out=new ArrayList<>(ids);
        return out.size()>MAX_FLAGS ? out.subList(0,MAX_FLAGS) : out;
    }
}
