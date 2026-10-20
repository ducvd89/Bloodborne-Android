package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.Executor;

/** Calibration aid for the gate flags (Atlas dialog, Debug: "Log door flags"). While on, every change of a candidate state flag
 *  of a gate (any area) between 0 and 1 is appended to files/bbport/atlas-doors.log: time, the area the player is in, the gate
 *  (entity id, model, type), the flag (id, kind, confidence), old and new value, the player's position. Open a door in the
 *  game and see which flag flips. The log is part of the "Report a problem" zip. Main thread; the file is written on the
 *  executor. */
final class AtlasDoorLog {
    static final String PREF="atlas_door_log";
    private static final long MAX_BYTES=1L<<20;

    private final File file; private final Executor io;
    private final SimpleDateFormat time=new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS",Locale.ROOT);
    private Map<Integer,List<String>> byFlag; private Map<Integer,String> areaOfFlag; private int[] ids; private byte[] last;
    private AtlasModel built; private int seenVersion=-1; private boolean seeded;

    AtlasDoorLog(File base,Executor io) { file=new File(base,"atlas-doors.log"); this.io=io; }

    /** Call after every poll while the setting is on. `areaId` is the player's area ("" unknown). */
    void poll(AtlasModel model,AtlasState st,String areaId) {
        if (model==null || !st.mapped) return;
        if (built!=model) build(model);
        if (ids.length==0 || seenVersion==st.flagsVersion) return;
        seenVersion=st.flagsVersion;
        List<String> out=null;
        for (int i=0;i<ids.length;++i) {
            int v=st.get(ids[i]);
            int old=last[i];
            last[i]=(byte)v;
            if (!seeded || old<0 || v<0 || v==old) continue; // unknown values (the game not yet applying the list) are not changes
            if (out==null) out=new ArrayList<>();
            String now=time.format(new Date());
            for (String gate:byFlag.get(ids[i]))
                out.add(String.format(Locale.ROOT,"%s uptime=%d area=%s gate=%s flag=%d %d->%d pos=(%.2f,%.2f,%.2f) block=0x%08x",
                    now,android.os.SystemClock.uptimeMillis(),areaId.isEmpty() ? "?" : areaId,gate,ids[i],old,v,st.x,st.y,st.z,st.block));
        }
        if (!seeded && st.get(ids[0])>=0) seeded=true; else if (!seeded) { for (int i=0;i<ids.length;++i) if (st.get(ids[i])>=0) { seeded=true; break; } }
        if (out!=null) write(out);
    }

    /** The game restarted or the setting was turned off and on: values are read as a fresh baseline. */
    void reset() { seeded=false; seenVersion=-1; if (last!=null) java.util.Arrays.fill(last,(byte)-1); }

    private void build(AtlasModel model) {
        built=model; byFlag=new HashMap<>(); areaOfFlag=new HashMap<>(); seeded=false; seenVersion=-1;
        List<Integer> all=new ArrayList<>();
        for (AtlasModel.Area a:model.areas) {
            if (a.gates==null) continue;
            for (NavGate g:a.gates) for (NavGate.Flag f:g.flags) {
                List<String> l=byFlag.get(f.id);
                if (l==null) { l=new ArrayList<>(); byFlag.put(f.id,l); all.add(f.id); }
                l.add(a.id+":"+g.entity+"("+g.type+" "+g.model+") "+f.kind+"/"+f.conf);
            }
        }
        ids=new int[all.size()]; for (int i=0;i<ids.length;++i) ids[i]=all.get(i);
        last=new byte[ids.length]; java.util.Arrays.fill(last,(byte)-1);
    }

    private void write(final List<String> lines) {
        io.execute(() -> {
            try {
                if (file.length()>MAX_BYTES) { // keep it small: start again
                    try (FileWriter w=new FileWriter(file,false)) { w.write("[log restarted: it passed 1 MB]\n"); }
                }
                try (FileWriter w=new FileWriter(file,true)) { for (String l:lines) w.write(l+"\n"); }
            } catch (IOException e) { android.util.Log.w("Bloodborne","atlas door log: "+e); }
        });
    }

    static File file(File base) { return new File(base,"atlas-doors.log"); }
}
