package ro.cobrabm.fexdroid;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** A gate of an area's route graph: a door, a key door, a one-way door, a lever door, a fog wall, a lift or a kick-down
 *  ladder (maps/nav/&lt;area&gt;.gates.json of map pack version 2, written by tools/atlas/build_pack.py; the index is the
 *  mechanism number the .bnv's gated links carry). Plain Java (no Android classes: tests/android-nav runs it on a desktop JDK).
 *
 *  A gate is open when ANY of its candidate state flags reads 1; with none set it is in its default state: ordinary doors
 *  and fog walls can be walked through (the player opens them), one-way and lever doors with a known operable side can be
 *  crossed from that side only, key, story, static, lever doors without a known side, lifts and ladders are closed.
 *  A flag that reads unknown (-1) counts as not set. */
final class NavGate {
    static final int OPEN=0, CLOSED=1, ONEWAY=2;
    static final String DOOR="door", KEY="key", ONE_WAY="oneway", LEVER="lever", SCRIPTED="scripted", SPECIAL="special",
        STATIC="static", FOG="fog", LIFT="lift", LADDER="ladder";

    interface Flags { int get(int id); }

    static final class Flag { int id; String kind="", means="", conf=""; }

    int index, entity; String type="", name="", model="";
    float x, y, z;
    int def=CLOSED;
    Flag[] flags=new Flag[0];
    /** The aperture of a door or fog wall (x0, z0, x1, z1) and its height range; null for lifts and ladders. */
    float[] seg; float yLo, yHi;
    /** Where a one-way or lever door is opened from (x, z pairs). */
    float[] operable=new float[0];
    /** The side of the door plane the action button works from: FRONT (the +normal side, see center / normal), BACK, BOTH or
     *  UNKNOWN. A door with a known single side may be crossed starting from that side only while it is closed. */
    static final int UNKNOWN=0, FRONT=1, BACK=2, BOTH=3;
    int side=UNKNOWN;
    float cx, cz, nx, nz; boolean hasFrame;
    /** Interaction regions where the action button appears (10 floats each: kind 0 wedge / 1 box, x, y, z, forward x, forward z,
     *  radius or depth, angle (deg) or width, y0, y1). */
    float[] regions=new float[0];
    /** Key doors: the goods id and name of the key item (name may be empty). */
    int keyId; String keyName="";
    /** Levers that operate a lever door, a kick-down ladder or a lift: x, y, z, stop (lift stop index, else -1) per lever. */
    float[] levers=new float[0];
    /** The stops of a lift (x, y, z triples). */
    float[] stops=new float[0];

    /** True when the gate's state is decided by its flags alone, never by walking up to it (key, story, lift...). */
    boolean needsFlag() { return def==CLOSED; }

    /** Which side of the door plane the point is on: 1 front, -1 back, 0 on the plane or no frame. */
    int sideOf(float px,float pz) {
        if (!hasFrame) return 0;
        float v=(px-cx)*nx+(pz-cz)*nz;
        return v>0.05f ? 1 : v<-0.05f ? -1 : 0;
    }
    /** The action button works from where the player is (the side of a door, or inside a region): true / false, or null when the
     *  gate has no known side. */
    Boolean operableFrom(float px,float pz) {
        if (side==BOTH) return Boolean.TRUE;
        if (side==FRONT || side==BACK) { int s=sideOf(px,pz); return s==0 ? null : (s==1)==(side==FRONT); }
        return null;
    }
    /** The player is inside an interaction region (where the button prompt appears). */
    boolean inRegion(float px,float py,float pz) {
        for (int i=0;i+9<regions.length;i+=10) {
            float dx=px-regions[i+1], dz=pz-regions[i+3];
            if (py<regions[i+8]-0.5f || py>regions[i+9]+0.5f) continue;
            float fx=regions[i+4], fz=regions[i+5];
            if (regions[i]==0) {
                float d=(float)Math.hypot(dx,dz);
                if (d>regions[i+6]+0.3f) continue;
                if (d<0.3f) return true;
                double a=Math.toDegrees(Math.acos(Math.max(-1,Math.min(1,(dx*fx+dz*fz)/d))));
                if (a<=Math.max(regions[i+7],60)+0.5) return true;
            } else {
                float along=dx*fx+dz*fz, across=-dx*fz+dz*fx;
                if (along>=-0.3f && along<=regions[i+6]+0.3f && Math.abs(across)<=regions[i+7]*0.5f+0.3f) return true;
            }
        }
        return false;
    }

    /** Any candidate flag reads 1. */
    boolean flagOpen(Flags f) {
        for (Flag fl:flags) if (f.get(fl.id)==1) return true;
        return false;
    }
    /** The routing state: OPEN, CLOSED or ONEWAY (passable only from the operable side). */
    int state(Flags f) { return flagOpen(f) ? OPEN : def; }

    /** The state of every gate (index = gate index). keyRetry: key doors that are not open count as passable from their
     *  operable side (open when it is unknown), for a second search when the first found no route. */
    static byte[] states(NavGate[] gates,Flags f,boolean keyRetry) {
        byte[] out=new byte[gates.length];
        for (int i=0;i<gates.length;++i) {
            NavGate g=gates[i];
            int s=g.state(f);
            if (keyRetry && s==CLOSED && g.type.equals(KEY)) s=g.side==FRONT || g.side==BACK ? ONEWAY : OPEN;
            out[i]=(byte)s;
        }
        return out;
    }
    static byte[] states(NavGate[] gates,Flags f) { return states(gates,f,false); }

    static NavGate[] parse(String json) throws MiniJson.ParseException {
        Object root=MiniJson.parse(json);
        if (!(root instanceof Map)) return new NavGate[0];
        Object list=((Map<?,?>)root).get("gates");
        if (!(list instanceof List)) return new NavGate[0];
        List<NavGate> out=new ArrayList<>();
        for (Object o:(List<?>)list) {
            if (!(o instanceof Map)) continue;
            Map<?,?> m=(Map<?,?>)o;
            NavGate g=new NavGate();
            g.index=(int)num(m.get("i"),out.size());
            if (g.index!=out.size()) throw new MiniJson.ParseException("gate "+g.index+" is out of order");
            g.type=str(m.get("type")); g.name=str(m.get("name")); g.model=str(m.get("model")); g.entity=(int)num(m.get("entity"),-1);
            float[] p=floats(m.get("pos"),3); if (p!=null) { g.x=p[0]; g.y=p[1]; g.z=p[2]; }
            String d=str(m.get("default"));
            g.def=d.equals("open") ? OPEN : d.equals("oneway") ? ONEWAY : CLOSED;
            List<Flag> flags=new ArrayList<>();
            Object fl=m.get("flags");
            if (fl instanceof List) for (Object f:(List<?>)fl) {
                if (!(f instanceof Map)) continue;
                Map<?,?> fm=(Map<?,?>)f;
                Flag flag=new Flag(); flag.id=(int)num(fm.get("id"),0);
                if (flag.id==0) continue;
                flag.kind=str(fm.get("kind")); flag.means=str(fm.get("means")); flag.conf=str(fm.get("conf"));
                flags.add(flag);
            }
            g.flags=flags.toArray(new Flag[0]);
            float[] seg=floats(m.get("seg"),4); g.seg=seg;
            float[] y=floats(m.get("y"),2); if (y!=null) { g.yLo=y[0]; g.yHi=y[1]; }
            g.operable=flat(m.get("operable"),2);
            String sd=str(m.get("side"));
            g.side=sd.equals("front") ? FRONT : sd.equals("back") ? BACK : sd.equals("both") ? BOTH : UNKNOWN;
            float[] c=floats(m.get("center"),2), n=floats(m.get("normal"),2);
            if (c!=null && n!=null) { g.cx=c[0]; g.cz=c[1]; g.nx=n[0]; g.nz=n[1]; g.hasFrame=true; }
            g.regions=regions(m.get("regions"));
            Object key=m.get("key");
            if (key instanceof Map) { g.keyId=(int)num(((Map<?,?>)key).get("id"),0); g.keyName=str(((Map<?,?>)key).get("name")); }
            Object lv=m.get("levers");
            if (lv instanceof List) {
                List<?> levers=(List<?>)lv; float[] l=new float[levers.size()*4]; int ln=0;
                for (Object o2:levers) {
                    if (!(o2 instanceof Map)) continue;
                    Map<?,?> lm=(Map<?,?>)o2; float[] pp=floats(lm.get("pos"),3);
                    if (pp==null) continue;
                    l[ln*4]=pp[0]; l[ln*4+1]=pp[1]; l[ln*4+2]=pp[2]; l[ln*4+3]=(float)num(lm.get("stop"),-1); ++ln;
                }
                g.levers=java.util.Arrays.copyOf(l,ln*4);
            }
            g.stops=flat(m.get("stops"),3);
            out.add(g);
        }
        return out.toArray(new NavGate[0]);
    }

    private static float[] regions(Object o) {
        if (!(o instanceof List)) return new float[0];
        List<?> list=(List<?>)o; float[] out=new float[list.size()*10]; int n=0;
        for (Object e:list) {
            if (!(e instanceof Map)) continue;
            Map<?,?> m=(Map<?,?>)e;
            boolean box="box".equals(str(m.get("kind")));
            float[] r={box?1:0,(float)num(m.get("x"),0),(float)num(m.get("y"),0),(float)num(m.get("z"),0),(float)num(m.get("fx"),0),(float)num(m.get("fz"),1),
                (float)num(box?m.get("depth"):m.get("radius"),1.2),(float)num(box?m.get("width"):m.get("angle"),60),(float)num(m.get("y0"),-1e9),(float)num(m.get("y1"),1e9)};
            System.arraycopy(r,0,out,n*10,10); ++n;
        }
        return java.util.Arrays.copyOf(out,n*10);
    }
    private static String str(Object o) { return o instanceof String ? (String)o : ""; }
    private static double num(Object o,double fallback) { return o instanceof Number ? ((Number)o).doubleValue() : fallback; }
    private static float[] floats(Object o,int n) {
        if (!(o instanceof List) || ((List<?>)o).size()!=n) return null;
        float[] out=new float[n]; int i=0;
        for (Object v:(List<?>)o) {
            if (!(v instanceof Number)) return null;
            out[i++]=((Number)v).floatValue();
        }
        return out;
    }
    private static float[] flat(Object o,int width) {
        if (!(o instanceof List)) return new float[0];
        List<?> list=(List<?>)o; float[] out=new float[list.size()*width]; int n=0;
        for (Object row:list) {
            float[] r=floats(row,width);
            if (r==null) continue;
            System.arraycopy(r,0,out,n*width,width); ++n;
        }
        return java.util.Arrays.copyOf(out,n*width);
    }
}
