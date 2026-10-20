package ro.cobrabm.fexdroid;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.json.JSONArray;
import org.json.JSONObject;

/** The feature data of the map pack (maps/features.json, schema 1) and the rules that turn the game's event flags
 *  into what the map shows: lit lamps, remaining pickups, conversations, and the boss fog. A port of
 *  bbport_map_features.cpp of Hunter's Atlas by MikeTat1988 (https://github.com/MikeTat1988/HuntersAtlas,
 *  GPL-2.0-or-later). Parsing is tolerant: an entry that does not fit is skipped. A flag value is -1 (unknown),
 *  0 or 1. */
final class AtlasFeatures {
    /** The flag values the game wrote (AtlasState) or a copy of them. */
    interface Flags { int get(int id); }

    static final class Lamp {
        String id="", name=""; float x, y, z;
        int unlock; int[] on=new int[0], off=new int[0];
        boolean discovered, available;
    }
    static final class Item { String name="", category=""; int quantity=1, collected; }
    static final class Pickup { float x, y, z; Item[] items=new Item[0]; int[] on=new int[0], off=new int[0]; }
    /** What a pickup shows now. */
    static final class PickupView { float x, y, z; String category="", label=""; }

    static final class Rule {
        float minY=-1e9f, maxY=1e9f;
        float[] bounds, centre; float radius;
        float[] tris; // 6 per triangle; null: no triangle condition
        int[] on=new int[0], off=new int[0], boss=new int[0];
        Object revealWhen;
        // a coarse grid over a large triangle list
        Map<Long,int[]> grid; float cell=8;

        boolean inside(float x,float y,float z) {
            if (y<minY || y>maxY) return false;
            if (bounds!=null && (x<bounds[0] || z<bounds[1] || x>bounds[2] || z>bounds[3])) return false;
            if (centre!=null) {
                float dx=x-centre[0], dz=z-centre[2];
                if (dx*dx+dz*dz>radius*radius) return false;
            }
            if (tris!=null) {
                if (grid!=null) {
                    int[] list=grid.get(key((int)Math.floor(x/cell),(int)Math.floor(z/cell)));
                    if (list==null) return false;
                    for (int i:list) if (hit(i,x,z)) return true;
                    return false;
                }
                for (int i=0;i<tris.length/6;++i) if (hit(i,x,z)) return true;
                return false;
            }
            return true;
        }
        private boolean hit(int i,float x,float z) {
            int o=i*6;
            float a=cross(o,o+2,x,z), b=cross(o+2,o+4,x,z), c=cross(o+4,o,x,z);
            return (a>=0 && b>=0 && c>=0) || (a<=0 && b<=0 && c<=0);
        }
        private float cross(int p,int q,float x,float z) {
            return (tris[q]-tris[p])*(z-tris[p+1])-(tris[q+1]-tris[p+1])*(x-tris[p]);
        }
        void index() {
            int n=tris.length/6;
            if (n<24) return;
            Map<Long,List<Integer>> cells=new HashMap<>();
            for (int i=0;i<n;++i) {
                float minx=Math.min(tris[i*6],Math.min(tris[i*6+2],tris[i*6+4])), maxx=Math.max(tris[i*6],Math.max(tris[i*6+2],tris[i*6+4]));
                float minz=Math.min(tris[i*6+1],Math.min(tris[i*6+3],tris[i*6+5])), maxz=Math.max(tris[i*6+1],Math.max(tris[i*6+3],tris[i*6+5]));
                int x0=(int)Math.floor(minx/cell), x1=(int)Math.floor(maxx/cell), z0=(int)Math.floor(minz/cell), z1=(int)Math.floor(maxz/cell);
                if ((long)(x1-x0+1)*(z1-z0+1)>4096) { grid=null; return; } // too large to index: scan
                for (int x=x0;x<=x1;++x) for (int z=z0;z<=z1;++z) {
                    List<Integer> l=cells.get(key(x,z)); if (l==null) cells.put(key(x,z),l=new ArrayList<>()); l.add(i);
                }
            }
            grid=new HashMap<>();
            for (Map.Entry<Long,List<Integer>> e:cells.entrySet()) {
                int[] a=new int[e.getValue().size()]; for (int i=0;i<a.length;++i) a[i]=e.getValue().get(i);
                grid.put(e.getKey(),a);
            }
        }
        private static long key(int x,int z) { return ((long)x<<32)|(z&0xffffffffL); }
    }

    static final class Area {
        String id; boolean alwaysVisible;
        final List<Lamp> lamps=new ArrayList<>();
        final List<Pickup> pickups=new ArrayList<>();
        final Map<Integer,Object> npcRules=new HashMap<>();
        final List<Rule> rules=new ArrayList<>();
    }

    final Map<String,Area> areas=new HashMap<>();
    /** Every flag the features read, most important first (lamps, boss fog, pickups, conversations). */
    final LinkedHashSet<Integer> flagIds=new LinkedHashSet<>();
    boolean present;
    String error;

    static AtlasFeatures load(File file) {
        AtlasFeatures f=new AtlasFeatures();
        if (file==null || !file.isFile()) return f;
        try {
            JSONObject data=new JSONObject(new String(Files.readAllBytes(file.toPath()),StandardCharsets.UTF_8));
            JSONObject areas=data.optJSONObject("areas");
            if (areas==null) throw new Exception("no areas");
            f.present=true;
            Set<Integer> lampFlags=new LinkedHashSet<>(), fogFlags=new LinkedHashSet<>(), pickupFlags=new LinkedHashSet<>(), npcFlags=new LinkedHashSet<>();
            for (java.util.Iterator<String> it=areas.keys();it.hasNext();) {
                String id=it.next();
                JSONObject a=areas.optJSONObject(id); if (a==null) continue;
                Area area=new Area(); area.id=id; area.alwaysVisible=a.optBoolean("always_visible",false);
                JSONArray lamps=a.optJSONArray("lamps");
                if (lamps!=null) for (int i=0;i<lamps.length();++i) {
                    JSONObject l=lamps.optJSONObject(i); if (l==null) continue;
                    float[] p=floats(l.optJSONArray("pos"),3); if (p==null) continue;
                    Lamp lamp=new Lamp(); lamp.id=l.optString("id",""); lamp.name=l.optString("name","Lamp");
                    lamp.x=p[0]; lamp.y=p[1]; lamp.z=p[2]; lamp.unlock=l.optInt("unlock_flag",0);
                    lamp.on=ints(l.optJSONArray("available_on")); lamp.off=ints(l.optJSONArray("available_off"));
                    if (lamp.unlock!=0) lampFlags.add(lamp.unlock);
                    for (int v:lamp.on) lampFlags.add(v);
                    for (int v:lamp.off) lampFlags.add(v);
                    area.lamps.add(lamp);
                }
                JSONArray pickups=a.optJSONArray("pickups");
                if (pickups!=null) for (int i=0;i<pickups.length();++i) {
                    JSONObject o=pickups.optJSONObject(i); if (o==null) continue;
                    float[] p=floats(o.optJSONArray("pos"),3); if (p==null) continue;
                    Pickup pickup=new Pickup(); pickup.x=p[0]; pickup.y=p[1]; pickup.z=p[2];
                    pickup.on=ints(o.optJSONArray("available_on")); pickup.off=ints(o.optJSONArray("available_off"));
                    for (int v:pickup.on) pickupFlags.add(v);
                    for (int v:pickup.off) pickupFlags.add(v);
                    JSONArray contents=o.optJSONArray("contents");
                    List<Item> items=new ArrayList<>();
                    if (contents!=null) for (int k=0;k<contents.length();++k) {
                        JSONObject c=contents.optJSONObject(k); if (c==null || !c.has("collected_flag")) continue;
                        Item item=new Item(); item.name=c.optString("name",""); item.category=c.optString("category","");
                        item.quantity=c.optInt("quantity",1); item.collected=c.optInt("collected_flag",0);
                        pickupFlags.add(item.collected); items.add(item);
                    }
                    pickup.items=items.toArray(new Item[0]);
                    if (pickup.items.length>0) area.pickups.add(pickup);
                }
                JSONObject npc=a.optJSONObject("npc_rules");
                if (npc!=null) for (java.util.Iterator<String> it2=npc.keys();it2.hasNext();) {
                    String key=it2.next();
                    try { int entity=Integer.parseInt(key); Object rule=npc.get(key); area.npcRules.put(entity,rule); collect(rule,npcFlags,0); }
                    catch (Exception ignored) {}
                }
                JSONArray regions=a.optJSONArray("reveal_regions");
                if (regions!=null) for (int i=0;i<regions.length();++i) {
                    JSONObject r=regions.optJSONObject(i); if (r==null) continue;
                    Rule rule=new Rule();
                    rule.minY=(float)r.optDouble("min_y",-1e9); rule.maxY=(float)r.optDouble("max_y",1e9);
                    if (r.has("bounds")) { rule.bounds=floats(r.optJSONArray("bounds"),4); if (rule.bounds==null) rule.bounds=new float[]{1,1,0,0}; } // malformed: matches nothing
                    if (r.has("centre")) { rule.centre=floats(r.optJSONArray("centre"),3); if (rule.centre==null) rule.centre=new float[]{1e9f,0,1e9f}; rule.radius=(float)r.optDouble("radius",0); }
                    if (r.has("triangles")) {
                        JSONArray t=r.optJSONArray("triangles");
                        List<float[]> ok=new ArrayList<>();
                        if (t!=null) for (int k=0;k<t.length();++k) { float[] v=floats(t.optJSONArray(k),6); if (v!=null) ok.add(v); }
                        rule.tris=new float[ok.size()*6];
                        for (int k=0;k<ok.size();++k) System.arraycopy(ok.get(k),0,rule.tris,k*6,6);
                        rule.index();
                    }
                    rule.on=ints(r.optJSONArray("available_on")); rule.off=ints(r.optJSONArray("available_off"));
                    rule.boss=ints(r.optJSONArray("boss_flags"));
                    if (r.has("reveal_when")) { rule.revealWhen=r.get("reveal_when"); collect(rule.revealWhen,fogFlags,0); }
                    for (int v:rule.boss) fogFlags.add(v);
                    for (int v:rule.on) fogFlags.add(v);
                    for (int v:rule.off) fogFlags.add(v);
                    area.rules.add(rule);
                }
                f.areas.put(id,area);
            }
            f.flagIds.addAll(lampFlags); f.flagIds.addAll(fogFlags); f.flagIds.addAll(pickupFlags); f.flagIds.addAll(npcFlags);
        } catch (Throwable e) {
            f.error=String.valueOf(e); f.present=false; f.areas.clear(); f.flagIds.clear();
            android.util.Log.w("Bloodborne","atlas features: "+e);
        }
        return f;
    }

    private static float[] floats(JSONArray a,int n) {
        if (a==null || a.length()!=n) return null;
        float[] out=new float[n];
        for (int i=0;i<n;++i) { double d=a.optDouble(i,Double.NaN); if (Double.isNaN(d) || Double.isInfinite(d)) return null; out[i]=(float)d; }
        return out;
    }
    private static int[] ints(JSONArray a) {
        if (a==null) return new int[0];
        int[] out=new int[a.length()];
        for (int i=0;i<out.length;++i) out[i]=a.optInt(i,0);
        return out;
    }
    private static void collect(Object e,Set<Integer> out,int depth) {
        if (depth>32 || !(e instanceof JSONArray)) return;
        JSONArray a=(JSONArray)e;
        if (a.length()==0) return;
        if ("flag".equals(a.optString(0,"")) && a.length()==2) { out.add(a.optInt(1)); return; }
        for (int i=1;i<a.length();++i) collect(a.opt(i),out,depth+1);
    }

    // ---- the rules ----

    /** An expression of the reveal and conversation rules: true/false, an integer, ["flag", id], or
     *  [op, a, b] with and, or, ==, !=. -1: unknown. */
    static int evaluate(Object e,Flags flags,int depth) {
        if (depth>32) return -1;
        if (e instanceof Boolean) return ((Boolean)e) ? 1 : 0;
        if (e instanceof Integer) return (Integer)e;
        if (e instanceof Long) return (int)(long)(Long)e;
        if (!(e instanceof JSONArray)) return -1;
        JSONArray a=(JSONArray)e;
        if (a.length()==0 || !(a.opt(0) instanceof String)) return -1;
        String op=a.optString(0);
        if (op.equals("flag") && a.length()==2 && !(a.opt(1) instanceof JSONArray)) return flags.get(a.optInt(1));
        if (a.length()!=3) return -1;
        int x=evaluate(a.opt(1),flags,depth+1), y=evaluate(a.opt(2),flags,depth+1);
        if (op.equals("and")) { if (x==0 || y==0) return 0; return x<0 || y<0 ? -1 : 1; }
        if (op.equals("or")) { if (x>0 || y>0) return 1; return x<0 || y<0 ? -1 : 0; }
        if (x<0 || y<0) return -1;
        if (op.equals("==")) return x==y ? 1 : 0;
        if (op.equals("!=")) return x!=y ? 1 : 0;
        return -1;
    }

    static boolean requirements(int[] on,int[] off,Flags flags) {
        for (int id:on) if (flags.get(id)!=1) return false;
        for (int id:off) if (flags.get(id)!=0) return false;
        return true;
    }

    /** Whether the map shows the point of the area (always, when the fog is off or there is no feature data). */
    boolean revealed(String areaId,float x,float y,float z,boolean fog,Flags flags) {
        if (!fog || !present) return true;
        Area a=areas.get(areaId);
        if (a==null) return false;
        if (a.alwaysVisible) return true;
        for (Rule rule:a.rules) if (rule.inside(x,y,z)) {
            if (!requirements(rule.on,rule.off,flags)) return false;
            if (rule.revealWhen!=null && evaluate(rule.revealWhen,flags,0)!=1) return false;
            for (int id:rule.boss) if (flags.get(id)!=1) return false;
            return true;
        }
        return false;
    }

    /** Lamp state from the flags (Refresh of bbport_map_features.cpp). */
    void refreshLamps(Area a,boolean valid,Flags flags) {
        for (Lamp d:a.lamps) {
            d.discovered=valid && (d.unlock==0 || flags.get(d.unlock)==1);
            d.available=d.discovered;
            for (int id:d.on) d.available=d.available && flags.get(id)==1;
            for (int id:d.off) d.available=d.available && flags.get(id)==0;
            // Hunter's Dream has no unlock flag: its visit conditions give the discovery state.
            if (d.unlock==0) d.discovered=d.available;
        }
    }

    /** mode: "all", "new" (new dialogue only) or "hidden". */
    boolean npcVisible(String areaId,int entity,String mode,Flags flags) {
        if (mode.equals("hidden")) return false;
        if (mode.equals("all")) return true;
        Area a=areas.get(areaId);
        if (a==null) return false;
        Object rule=a.npcRules.get(entity);
        return rule!=null && evaluate(rule,flags,0)==1;
    }

    /** The remaining pickups of the area in the enabled categories. */
    List<PickupView> pickups(Area a,boolean valid,boolean fog,Flags flags,Set<String> categories) {
        List<PickupView> out=new ArrayList<>();
        if (!valid) return out;
        for (Pickup p:a.pickups) {
            if (!requirements(p.on,p.off,flags)) continue;
            if (!revealed(a.id,p.x,p.y,p.z,fog,flags)) continue;
            PickupView v=new PickupView(); v.x=p.x; v.y=p.y; v.z=p.z;
            StringBuilder label=new StringBuilder();
            for (Item item:p.items) {
                if (!categories.contains(item.category) || flags.get(item.collected)!=0) continue;
                if (v.category.isEmpty()) v.category=item.category;
                if (label.length()>0) label.append('\n');
                label.append(item.name).append(" x").append(item.quantity);
            }
            if (!v.category.isEmpty()) { v.label=label.toString(); out.add(v); }
        }
        return out;
    }
}
