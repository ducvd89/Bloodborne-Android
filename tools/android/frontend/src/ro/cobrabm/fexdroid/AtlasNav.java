package ro.cobrabm.fexdroid;

import android.content.SharedPreferences;
import android.os.Handler;
import android.os.SystemClock;
import java.io.File;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Route finding for the map (pack version 2): the destination (kept in the preferences, shared by the second-screen map
 *  and the mini map), the route to it, and the live state of the gates (doors, lifts...).
 *
 *  The route graphs (NavData, maps/nav/&lt;area&gt;.bnv) are read and searched on one background thread; at most two areas stay
 *  in memory. A search runs when the destination is set or changes, when the player leaves the route by more than 4 m, when the
 *  gate states derived from the game's event flags changed and when the player enters the destination's area. The UI thread never
 *  waits: onState() only compares numbers and posts jobs; a finished search is delivered on the main thread and bumps
 *  {@link #version}, which the views compare to redraw (and rebuild their cached route path).
 *
 *  Gates: a gate is open when any candidate flag reads 1, else in its default state (NavGate). A closed door with a known
 *  operable side can be crossed starting from that side only (it is opened on the way); key doors are closed unless the flag
 *  says open, but when no route exists a second search lets the route pass them ("Route needs [key]"); a lift or kick-down
 *  ladder that is not unlocked can be used starting from its lever (the route then carries a "pull the lever" marker).
 *  Main thread unless noted. */
final class AtlasNav {
    interface Listener { void changed(); }
    static final class Dest { String area=""; float x, y, z; String name=""; boolean marked; }
    static final class Snap { float x, y, z; }
    interface SnapCallback { void done(Snap snap,String problem); }

    static final String PREF_DEST="atlas_dest";
    private static final int MAX_EXPANSIONS=400_000;
    private static final long MAX_NANOS=300_000_000L, JOB_NANOS=700_000_000L;
    static final float ARRIVE=3f, OFF_ROUTE=4f;
    private static final long MIN_GAP_MS=700, RETRY_MS=2500;
    /** How a gate is drawn. */
    static final int S_OPEN=0, S_BLOCKED=1, S_ONEWAY=2, S_FREE=3;

    private final File base; private final SharedPreferences prefs; private final Handler main;
    private final ExecutorService exec=Executors.newSingleThreadExecutor(r -> { Thread t=new Thread(r,"Atlas-nav"); t.setDaemon(true); return t; });
    private final LinkedHashMap<String,NavData> cache=new LinkedHashMap<String,NavData>(4,.75f,true);
    private volatile AtlasModel model;
    private final List<Listener> listeners=new ArrayList<>();
    private boolean destroyed;

    private Dest dest;
    private volatile NavData.Route route;
    private String status="";          // "No route found", "Go to <area>", "Calculating route…" ...
    private String needsKey="";        // "Route needs <key>" text, or empty
    private byte[] routeBase;          // the (non key-retry) gate states the route was made with
    private boolean searching; private int token; private long lastStart, lastFail; private float failX=Float.NaN, failY, failZ; private byte[] failStates;
    private float progress, routeDistance, remaining=-1;
    private float px, py, pz; private boolean pvalid; private String playerArea="";
    private long arrivedUntil;
    /** Bumped when anything the views draw from here changed. */
    volatile int version;

    // live gate states per area (for drawing), valid for one flags version
    private final Map<String,byte[]> shown=new LinkedHashMap<>(); private int shownFlags=-1;
    private int statesFlags=-2; private byte[] liveStates, liveKeyStates; private String liveArea="";

    AtlasNav(File base,SharedPreferences prefs,Handler main) {
        this.base=base; this.prefs=prefs; this.main=main;
        dest=readDest();
    }

    void setListener(Listener l) { listeners.add(l); }
    void clearListeners() { listeners.clear(); }
    private void changed() { ++version; for (Listener l:listeners) l.changed(); }

    /** The pack's data (null: no pack). Frees the route graphs. */
    void setModel(AtlasModel m) {
        model=m;
        synchronized (cache) { cache.clear(); }
        route=null; status=""; needsKey=""; searching=false; ++token; remaining=-1; shown.clear(); shownFlags=-1; statesFlags=-2;
        changed();
    }
    /** The model of the pack in use (set again only when it is another one). */
    void useModel(AtlasModel m) { if (model!=m) setModel(m); }
    void destroy() { destroyed=true; exec.shutdownNow(); }

    boolean available() { AtlasModel m=model; return m!=null && m.navAvailable(); }
    Dest dest() { return dest; }
    NavData.Route route() { return route; }
    String status() { return status; }
    String needsKey() { return needsKey; }
    /** Remaining route length in metres, -1 when there is no route. */
    float remaining() { return route==null ? -1 : remaining; }
    boolean arrived() { return dest==null && SystemClock.uptimeMillis()<arrivedUntil; }
    boolean hasDestination() { return dest!=null; }
    /** Where the destination lies relative to the player's last known place (straight line), -1 when unknown. */
    float straightLine() {
        if (dest==null || !pvalid || !dest.area.equals(playerArea)) return -1;
        return (float)Math.sqrt((dest.x-px)*(dest.x-px)+(dest.y-py)*(dest.y-py)+(dest.z-pz)*(dest.z-pz));
    }
    String playerArea() { return playerArea; }
    /** Length of the route travelled up to the player's nearest point (m). */
    float progressLength() { return progress; }
    String destAreaName() {
        AtlasModel m=model;
        if (m==null || dest==null) return "";
        AtlasModel.Area a=m.byId(dest.area);
        return a!=null ? a.name : dest.area;
    }

    // ---- the destination ----

    void setDest(String area,float x,float y,float z,String name,boolean marked) {
        Dest d=new Dest(); d.area=area; d.x=x; d.y=y; d.z=z; d.name=name==null ? "" : name; d.marked=marked;
        dest=d; ++token; route=null; needsKey=""; status=""; searching=false; remaining=-1; arrivedUntil=0; failX=Float.NaN; lastStart=0;
        saveDest(); changed();
    }
    void clear() {
        if (dest==null && route==null) return;
        dest=null; ++token; route=null; needsKey=""; status=""; searching=false; remaining=-1; failX=Float.NaN;
        saveDest(); changed();
    }
    private void saveDest() {
        Dest d=dest;
        prefs.edit().putString(PREF_DEST,d==null ? "" : String.format(Locale.ROOT,"%s|%.2f|%.2f|%.2f|%d|%s",d.area,d.x,d.y,d.z,d.marked?1:0,d.name)).apply();
    }
    private Dest readDest() {
        try {
            String v=prefs.getString(PREF_DEST,"");
            if (v==null || v.isEmpty()) return null;
            String[] p=v.split("\\|",6);
            if (p.length<5) return null;
            Dest d=new Dest(); d.area=p[0]; d.x=Float.parseFloat(p[1]); d.y=Float.parseFloat(p[2]); d.z=Float.parseFloat(p[3]); d.marked=p[4].equals("1");
            d.name=p.length>5 ? p[5] : "";
            if (d.area.length()!=12 || Float.isNaN(d.x+d.y+d.z)) return null;
            return d;
        } catch (RuntimeException e) { return null; }
    }

    // ---- the game's state: arrival, progress, re-routing ----

    /** Called after every poll of the game's state (about 30 times a second): cheap unless a search is needed. */
    void onState(AtlasState st) {
        AtlasModel m=model;
        if (m==null || destroyed) return;
        boolean valid=st.mapped && st.valid;
        if (valid) {
            px=st.x; py=st.y; pz=st.z; pvalid=true;
            int idx=m.areaForBlock(st.block);
            playerArea=idx>=0 ? m.areas.get(idx).id : "";
        } else if (!st.mapped) pvalid=false;
        Dest d=dest;
        if (d==null || !available()) return;
        if (!pvalid || playerArea.isEmpty()) return;
        if (!d.area.equals(playerArea)) {
            String want="Go to "+destAreaName();
            if (route!=null || !want.equals(status)) { route=null; needsKey=""; status=want; remaining=-1; searching=false; ++token; changed(); }
            return;
        }
        AtlasModel.Area area=m.byId(playerArea);
        if (area==null || !area.navData || area.gates==null) { if (!"No route data for this area".equals(status)) { status="No route data for this area"; route=null; changed(); } return; }
        // arrival
        float plan=(float)Math.hypot(d.x-px,d.z-pz);
        if (plan<ARRIVE && Math.abs(d.y-py)<4f) {
            clear(); arrivedUntil=SystemClock.uptimeMillis()+3000; main.postDelayed(this::changed,3100); changed(); return;
        }
        refreshStates(st,area);
        NavData.Route r=route;
        long now=SystemClock.uptimeMillis();
        boolean need=false;
        if (r!=null) {
            float[] at=new float[1];
            float dist=r.project(px,py,pz,at);
            if (Math.abs(dist-routeDistance)>0.05f || Math.abs(at[0]-progress)>0.05f) {
                routeDistance=dist; progress=at[0]; remaining=Math.max(0,r.length-at[0]);
                ++version; for (Listener l:listeners) l.changed();
            }
            if (dist>OFF_ROUTE) need=true;
            else if (!Arrays.equals(routeBase,liveStates)) need=true;
        } else if (!searching) {
            boolean moved=Float.isNaN(failX) || Math.hypot(px-failX,pz-failZ)>5 || !Arrays.equals(failStates,liveStates);
            need=Float.isNaN(failX) ? now-lastFail>400 : (moved && now-lastFail>RETRY_MS) || now-lastFail>12000;
        }
        if (need && !searching && now-lastStart>=MIN_GAP_MS) startSearch(area,d,now);
    }

    /** The gate states of the area from the current flags (once per flags version). */
    private void refreshStates(AtlasState st,AtlasModel.Area area) {
        if (statesFlags==st.flagsVersion && area.id.equals(liveArea)) return;
        statesFlags=st.flagsVersion; liveArea=area.id;
        AtlasFeatures.Flags f=st;
        NavGate.Flags nf=f::get;
        liveStates=NavGate.states(area.gates,nf,false);
        liveKeyStates=NavGate.states(area.gates,nf,true);
    }

    private void startSearch(final AtlasModel.Area area,final Dest d,long now) {
        searching=true; lastStart=now;
        if (route==null) { status="Calculating route…"; changed(); }
        final int tk=token; final float sx=px, sy=py, sz=pz;
        final byte[] states=liveStates.clone(), keyStates=liveKeyStates.clone();
        final NavGate[] gates=area.gates; final String id=area.id;
        exec.execute(() -> {
            final Result res=search(id,gates,sx,sy,sz,d,states,keyStates);
            main.post(() -> {
                if (destroyed || tk!=token) return;
                searching=false;
                lastFail=SystemClock.uptimeMillis();
                if (res.route!=null) {
                    route=res.route; routeBase=states; needsKey=res.needsKey; status="";
                    failX=Float.NaN; progress=0; routeDistance=0; remaining=route.length;
                    float[] at=new float[1]; routeDistance=route.project(px,py,pz,at); progress=at[0]; remaining=Math.max(0,route.length-at[0]);
                } else if (res.problem!=null) {
                    // a search that failed for lack of a node under the player keeps the old route; others clear it
                    if (res.noSource && route!=null) return;
                    route=null; routeBase=null; needsKey=""; status=res.problem; failX=sx; failY=sy; failZ=sz; failStates=states;
                }
                changed();
            });
        });
    }

    private static final class Result { NavData.Route route; String problem; String needsKey=""; boolean noSource; }

    /** Background thread. */
    private Result search(String id,NavGate[] gates,float sx,float sy,float sz,Dest d,byte[] states,byte[] keyStates) {
        Result res=new Result();
        NavData data=data(id,gates.length);
        if (data==null) { res.problem="Route data could not be read"; return res; }
        int[] src=data.snap(sx,sy,sz,2.5f,2);
        if (src.length==0) src=data.snap(sx,sy,sz,6f,2);
        if (src.length==0) { res.problem="Not on walkable ground"; res.noSource=true; return res; }
        int[] dst=data.snap(d.x,d.y,d.z,1.5f,3);
        if (dst.length==0) dst=data.snap(d.x,d.y,d.z,6f,3);
        if (dst.length==0) { res.problem="No walkable floor at the destination"; return res; }
        long t0=System.nanoTime();
        boolean limited=false;
        for (int pass=0;pass<2 && res.route==null;++pass) {
            byte[] st=pass==0 ? states : keyStates;
            if (pass==1 && Arrays.equals(states,keyStates)) break;
            for (int di=0;di<dst.length && res.route==null;++di) {
                for (int si=0;si<src.length && res.route==null;++si) {
                    if (si>0 && di>0) continue;
                    if (System.nanoTime()-t0>JOB_NANOS) break;
                    NavData.Path p=data.route(src[si],dst[di],st,MAX_EXPANSIONS,MAX_NANOS,1f);
                    if (p.status==NavData.LIMIT) limited=true;
                    if (p.status!=NavData.OK) continue;
                    NavData.Route r=data.toRoute(p,st);
                    resolveLevers(r,gates);
                    res.route=r;
                    if (pass==1) res.needsKey=keyNames(r,gates,states);
                }
            }
        }
        if (res.route==null) res.problem=limited ? "No route found (too far to search)" : "No route found";
        return res;
    }

    /** A "pull the lever" marker goes to the lever of the gate nearest to where the route would use the lift or ladder. */
    private static void resolveLevers(NavData.Route r,NavGate[] gates) {
        for (int i=0;i<r.marks;++i) {
            if (r.mkind[i]!=NavData.M_LEVER) continue;
            NavGate g=r.mgate[i]>=0 && r.mgate[i]<gates.length ? gates[r.mgate[i]] : null;
            if (g==null || g.levers.length==0) continue;
            float best=Float.MAX_VALUE; int bi=-1;
            for (int k=0;k<g.levers.length/4;++k) {
                float dx=g.levers[k*4]-r.mx[i], dy=g.levers[k*4+1]-r.my[i], dz=g.levers[k*4+2]-r.mz[i], dd=dx*dx+dy*dy+dz*dz;
                if (dd<best) { best=dd; bi=k; }
            }
            if (bi>=0) { r.mx[i]=g.levers[bi*4]; r.my[i]=g.levers[bi*4+1]; r.mz[i]=g.levers[bi*4+2]; }
        }
    }
    private static String keyNames(NavData.Route r,NavGate[] gates,byte[] states) {
        List<String> names=new ArrayList<>();
        for (int i=0;i<r.marks;++i) {
            if (r.mkind[i]!=NavData.M_GATE || r.mgate[i]<0 || r.mgate[i]>=gates.length) continue;
            NavGate g=gates[r.mgate[i]];
            if (!g.type.equals(NavGate.KEY) || states[g.index]==NavGate.OPEN) continue;
            String n=g.keyName.isEmpty() ? "a key" : g.keyName;
            if (!names.contains(n)) names.add(n);
        }
        StringBuilder s=new StringBuilder();
        for (String n:names) { if (s.length()>0) s.append(", "); s.append(n); }
        return s.toString();
    }

    /** The area's graph (loaded when first needed, two areas at most in memory). Background thread. */
    private NavData data(String id,int gateCount) {
        synchronized (cache) {
            NavData d=cache.get(id);
            if (d!=null) return d;
        }
        try {
            byte[] file=Files.readAllBytes(new File(new File(AtlasPack.dir(base),"maps/nav"),id+".bnv").toPath());
            NavData d=NavData.load(id,file);
            if (d.gateCount!=gateCount) { android.util.Log.w("Bloodborne","atlas nav "+id+": gate table does not match the graph"); return null; }
            synchronized (cache) {
                cache.put(id,d);
                while (cache.size()>2) { String eldest=cache.keySet().iterator().next(); cache.remove(eldest); }
            }
            return d;
        } catch (Throwable e) { // includes OutOfMemoryError: no route finding then
            android.util.Log.w("Bloodborne","atlas nav "+id+": "+e);
            synchronized (cache) { cache.clear(); }
            return null;
        }
    }

    /** Nearest walkable spot to (x, z) on a floor (heights yLo..yHi) within 6 m, for "Set destination here"; the callback runs
     *  on the main thread with the node's position, or a problem text. */
    void snapPoint(final String areaId,final float x,final float z,final float yLo,final float yHi,final SnapCallback cb) {
        final AtlasModel m=model;
        final AtlasModel.Area area=m!=null ? m.byId(areaId) : null;
        if (area==null || area.gates==null || !area.navData) { cb.done(null,"No route data here"); return; }
        exec.execute(() -> {
            NavData d=data(areaId,area.gates.length);
            Snap s=null; String problem=null;
            if (d==null) problem="Route data could not be read";
            else {
                int[] n=d.snapBand(x,z,yLo,yHi,6f,1);
                if (n.length==0) problem="No walkable floor here";
                else { s=new Snap(); s.x=d.x[n[0]]; s.y=d.y[n[0]]; s.z=d.z[n[0]]; }
            }
            final Snap fs=s; final String fp=problem;
            main.post(() -> { if (!destroyed) cb.done(fs,fp); });
        });
    }

    // ---- gates for drawing and the cards ----

    NavGate[] gates(String areaId) {
        AtlasModel m=model;
        AtlasModel.Area a=m!=null ? m.byId(areaId) : null;
        return a!=null ? a.gates : null;
    }
    /** How each gate of the area is drawn (S_*), from the flags; recomputed when the flags changed. */
    byte[] drawStates(String areaId,AtlasState st) {
        NavGate[] gates=gates(areaId);
        if (gates==null) return null;
        if (shownFlags!=st.flagsVersion) { shown.clear(); shownFlags=st.flagsVersion; }
        byte[] s=shown.get(areaId);
        if (s!=null) return s;
        s=new byte[gates.length];
        NavGate.Flags f=st::get;
        for (int i=0;i<gates.length;++i) {
            NavGate g=gates[i];
            if (g.flagOpen(f)) s[i]=S_OPEN;
            else { int state=g.state(f); s[i]=(byte)(state==NavGate.CLOSED ? S_BLOCKED : state==NavGate.ONEWAY ? S_ONEWAY : S_FREE); }
        }
        shown.put(areaId,s);
        return s;
    }

    /** The label card of a gate: {title, lines}. */
    static String[] gateCard(NavGate g,AtlasState st,boolean playerValid,float px,float py,float pz) {
        NavGate.Flags f=st::get;
        boolean open=g.flagOpen(f);
        String kind=g.type.equals(NavGate.KEY) ? "Key door" : g.type.equals(NavGate.ONE_WAY) ? "One-way door" : g.type.equals(NavGate.LEVER) ? "Lever door"
            : g.type.equals(NavGate.FOG) ? "Fog wall" : g.type.equals(NavGate.LIFT) ? "Lift" : g.type.equals(NavGate.LADDER) ? "Kick-down ladder"
            : g.type.equals(NavGate.STATIC) ? "Door" : g.type.equals(NavGate.SCRIPTED) || g.type.equals(NavGate.SPECIAL) ? "Door" : "Door";
        StringBuilder lines=new StringBuilder();
        String line;
        if (open) line="Open";
        else if (g.type.equals(NavGate.FOG)) line="Can be walked through";
        else if (g.type.equals(NavGate.STATIC)) line="Does not open";
        else if (g.type.equals(NavGate.SCRIPTED) || g.type.equals(NavGate.SPECIAL)) line="Opened by story events";
        else if (g.type.equals(NavGate.LIFT) || g.type.equals(NavGate.LADDER)) {
            line=g.levers.length>0 ? "Operated by a lever nearby" : "Unknown";
        } else if (g.type.equals(NavGate.LEVER) && g.side!=NavGate.BOTH && g.side!=NavGate.UNKNOWN) {
            Boolean can=playerValid ? g.operableFrom(px,pz) : null;
            line=can==null ? "Opened by a lever" : can ? "Can open from here (lever on this side)" : "Lever is on the other side";
        } else {
            Boolean can=playerValid ? g.operableFrom(px,pz) : null;
            if (g.side==NavGate.BOTH) line="Can open from either side";
            else if (g.side==NavGate.UNKNOWN) line="Unknown";
            else if (can==null) line=g.side==NavGate.FRONT || g.side==NavGate.BACK ? "Opens from one side" : "Unknown";
            else if (can) line=playerValid && g.inRegion(px,py,pz) ? "Can open from here (in range)" : "Can open from here";
            else line="Opens from the other side";
        }
        lines.append(line);
        if (g.type.equals(NavGate.KEY) && !open) lines.append("\nNeeds ").append(g.keyName.isEmpty() ? "a key" : g.keyName).append(" (if you have it)");
        int n=0;
        for (NavGate.Flag fl:g.flags) {
            if (n++>=4) { lines.append("\n…"); break; }
            int v=f.get(fl.id);
            lines.append('\n').append(fl.id).append(' ').append(fl.kind).append(" (").append(fl.conf).append("): ").append(v<0 ? "?" : String.valueOf(v));
        }
        return new String[]{kind+(open ? " · open" : ""),lines.toString()};
    }

    /** Text for the destination chip's second part: distance, status or arrival. */
    String chipDetail() {
        if (dest==null) return arrived() ? "Arrived" : "";
        if (!status.isEmpty()) return status;
        if (route!=null) {
            String d=remaining()>=1000 ? String.format(Locale.ROOT,"%.1f km",remaining()/1000) : String.format(Locale.ROOT,"%d m",Math.round(remaining()));
            return needsKey.isEmpty() ? d : d+" · Route needs "+needsKey;
        }
        float line=straightLine();
        return line>=0 ? String.format(Locale.ROOT,"%d m",Math.round(line)) : "";
    }
}
