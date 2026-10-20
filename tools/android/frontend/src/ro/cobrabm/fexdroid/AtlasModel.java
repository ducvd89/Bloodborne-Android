package ro.cobrabm.fexdroid;

import android.graphics.Path;
import android.util.JsonReader;
import android.util.JsonToken;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import org.json.JSONArray;
import org.json.JSONObject;

/** The map data of the pack (atlas/maps): the areas of atlas.json, their floors read from maps/<area>.json (version 1
 *  rectangles or version 2 triangles with heights, outlines and walls), the names of catalog.json and the feature
 *  data (AtlasFeatures). A port of the data side of bbport_map.cpp of Hunter's Atlas by MikeTat1988
 *  (https://github.com/MikeTat1988/HuntersAtlas, GPL-2.0-or-later): the area and floor the player is on (CurrentArea,
 *  AutoLayer), the location names. Floors are read with a streaming reader (they are large) and kept in flat arrays
 *  sorted in space, so the view draws only what is on screen. Parsing is tolerant: what does not fit is skipped. */
final class AtlasModel {
    static final int CHUNK_TRIANGLES=2048, CHUNK_SEGMENTS=512;

    /** Cached results for one floor that depend on the fog: which triangles show, the outline and wall paths. */
    static final class Cache {
        Object key;
        byte[] mask; // 1 = shown; null: all
        Chunks outline, walls;
    }
    /** Paths of up to CHUNK_SEGMENTS segments with their world boxes (minx, minz, maxx, maxz). */
    static final class Chunks { Path[] paths=new Path[0]; float[] box=new float[0]; }

    static final class Layer {
        float height, span=6, anchor, minY, maxY; boolean hasVertexHeights;
        int triCount; float[] tri=new float[0]; float[] triY; // 6 and 3 floats per triangle
        float[] chunkBox=new float[0];                          // per CHUNK_TRIANGLES triangles
        float[] rects=new float[0];                             // x, z, w, h
        final List<float[]> outlines=new ArrayList<>();         // x, z pairs
        float[] walls=new float[0];                             // x1, z1, x2, z2
        // used by the view
        float[] texs; final int[][] colors=new int[3][]; final float[][] colorKey=new float[3][5]; final Object[] colorMask=new Object[3];
        volatile Cache cache;
        // version 3: where the data is in the .bin file ({offset, count} of triangles, heights, walls, outline) and the
        // ring sizes of the outline; only until loadGeometry has read them
        long[][] blocks; int[] rings;
    }
    static final class Marker { float x, y, z; String kind="", name=""; int entity; }
    static final class Landmark { float x, y, z, maxHorizontal=60, maxVertical=6; String name="", prefix="Near "; }
    static final class Region { String name=""; float minY=-1e9f, maxY=1e9f; boolean main; float[] tris=new float[0]; }

    static final int UNLOADED=0, LOADING=1, READY=2, FAILED=3;
    static final class Area {
        String id="", name=""; float[] bounds=new float[4];
        volatile int state; volatile String error; volatile long used;
        Layer[] layers=new Layer[0]; int[] floorOrder=new int[0]; // floorOrder: layers from the lowest anchor up
        Marker[] markers=new Marker[0];
        final List<AtlasFeatures.Lamp> mapLamps=new ArrayList<>();
        final List<Landmark> landmarks=new ArrayList<>();
        final List<Region> regions=new ArrayList<>();
        /** Route data (pack version 2): the area's gates, from maps/nav/&lt;id&gt;.gates.json; null when the pack has none. */
        NavGate[] gates;
        boolean navData; // maps/nav/<id>.bnv exists
    }

    final File root;
    final List<Area> areas=new ArrayList<>();
    AtlasFeatures features=new AtlasFeatures();
    String error;
    /** pack.json's pack_version (1 when absent) and the flags every gate of every area reads (pack version 2). */
    int packVersion=1;
    final java.util.LinkedHashSet<Integer> navFlagIds=new java.util.LinkedHashSet<>();

    private AtlasModel(File root) { this.root=root; }

    /** Reads atlas.json, catalog.json and features.json (not the floors). Blocks. */
    static AtlasModel load(File root) {
        AtlasModel m=new AtlasModel(root);
        File maps=new File(root,"maps");
        try {
            JSONObject atlas=new JSONObject(read(find(maps,root,"atlas.json")));
            JSONArray list=atlas.getJSONArray("maps");
            for (int i=0;i<list.length();++i) {
                JSONObject j=list.optJSONObject(i); if (j==null) continue;
                Area a=new Area(); a.id=j.optString("id",""); a.name=j.optString("name",a.id);
                if (a.id.length()!=12 || !a.id.matches("[m0-9_]+")) continue;
                JSONArray b=j.optJSONArray("bounds");
                if (b==null || b.length()<4) continue;
                for (int k=0;k<4;++k) a.bounds[k]=(float)b.optDouble(k,0);
                if (a.bounds[2]>a.bounds[0] && a.bounds[3]>a.bounds[1]) m.areas.add(a);
            }
        } catch (Exception e) { m.error="atlas.json: "+e.getMessage(); android.util.Log.w("Bloodborne","atlas: "+e); return m; }
        try {
            File file=find(maps,root,"catalog.json");
            if (file.isFile()) {
                JSONObject catalog=new JSONObject(read(file));
                java.nio.ByteBuffer catalogBin=null;
                for (Area a:m.areas) {
                    JSONObject entry=catalog.optJSONObject(a.id); if (entry==null) continue;
                    JSONArray landmarks=entry.optJSONArray("landmarks");
                    if (landmarks!=null) for (int i=0;i<landmarks.length();++i) {
                        JSONObject o=landmarks.optJSONObject(i); if (o==null || !o.has("name")) continue;
                        JSONArray p=o.optJSONArray("pos"); if (p==null || p.length()!=3) continue;
                        Landmark l=new Landmark(); l.name=o.optString("name"); l.x=(float)p.optDouble(0); l.y=(float)p.optDouble(1); l.z=(float)p.optDouble(2);
                        l.maxHorizontal=(float)o.optDouble("max_horizontal_distance",60); l.maxVertical=(float)o.optDouble("max_vertical_distance",6);
                        l.prefix=o.optString("subtitle_prefix","Near ");
                        if (finite(l.x) && finite(l.y) && finite(l.z) && l.maxHorizontal>0 && l.maxVertical>=0) a.landmarks.add(l);
                    }
                    JSONArray regions=entry.optJSONArray("regions");
                    if (regions!=null) for (int i=0;i<regions.length();++i) {
                        JSONObject o=regions.optJSONObject(i); if (o==null || !o.has("name")) continue;
                        if (o.has("shape") && !"footprint".equals(o.optString("shape"))) continue;
                        Region r=new Region(); r.name=o.optString("name");
                        r.minY=(float)o.optDouble("min_y",-1e9); r.maxY=(float)o.optDouble("max_y",1e9);
                        if (!finite(r.minY) || !finite(r.maxY) || r.minY>r.maxY) continue;
                        r.main=o.optBoolean("main_location",false);
                        JSONArray t=o.optJSONArray("triangles");
                        if (t!=null && t.length()==2 && !(t.opt(0) instanceof JSONArray)) { // [byteOffset, count] in catalog.bin
                            try {
                                if (catalogBin==null) {
                                    File bf=new File(file.getParentFile(),"catalog.bin");
                                    catalogBin=java.nio.ByteBuffer.wrap(Files.readAllBytes(bf.toPath())).order(java.nio.ByteOrder.LITTLE_ENDIAN);
                                }
                                r.tris=block(catalogBin,catalogBin.limit(),t.optLong(0),t.optLong(1)*6);
                                for (float v:r.tris) if (!finite(v)) { r.tris=new float[0]; break; }
                            } catch (Exception e) { android.util.Log.w("Bloodborne","atlas catalog.bin: "+e); }
                        } else if (t!=null) {
                            float[] buf=new float[t.length()*6]; int n=0;
                            for (int k=0;k<t.length();++k) {
                                JSONArray v=t.optJSONArray(k); if (v==null || v.length()!=6) continue;
                                boolean ok=true; float[] tri=new float[6];
                                for (int q=0;q<6;++q) { tri[q]=(float)v.optDouble(q,Double.NaN); if (!finite(tri[q])) ok=false; }
                                if (ok) { System.arraycopy(tri,0,buf,n*6,6); ++n; }
                            }
                            r.tris=Arrays.copyOf(buf,n*6);
                        }
                        if (r.tris.length>0) a.regions.add(r);
                    }
                }
            }
        } catch (Throwable e) { android.util.Log.w("Bloodborne","atlas catalog: "+e); }
        m.features=AtlasFeatures.load(find(maps,root,"features.json"));
        m.loadNav(maps);
        return m;
    }

    /** pack.json's version and the gate tables of maps/nav (pack version 2). Tolerant: what cannot be read is left out. */
    private void loadNav(File maps) {
        try {
            JSONObject pack=new JSONObject(read(new File(root,"pack.json")));
            packVersion=pack.optInt("pack_version",1);
        } catch (Exception e) { packVersion=1; }
        if (packVersion<2) return;
        for (Area a:areas) {
            File g=new File(new File(maps,"nav"),a.id+".gates.json");
            a.navData=new File(new File(maps,"nav"),a.id+".bnv").isFile();
            if (!g.isFile() || !a.navData) { a.navData=false; continue; }
            try {
                a.gates=NavGate.parse(read(g));
                for (NavGate gate:a.gates) for (NavGate.Flag f:gate.flags) navFlagIds.add(f.id);
            } catch (Exception e) { a.gates=null; a.navData=false; android.util.Log.w("Bloodborne","atlas gates "+a.id+": "+e); }
        }
    }
    /** The pack has route data for at least one area. */
    boolean navAvailable() { for (Area a:areas) if (a.navData) return true; return false; }

    /** maps/name, else the pack's root (the C++ looks for the catalog beside the maps folder). */
    private static File find(File maps,File root,String name) {
        File f=new File(maps,name);
        return f.isFile() ? f : new File(root,name).isFile() ? new File(root,name) : f;
    }
    private static String read(File f) throws IOException { return new String(Files.readAllBytes(f.toPath()),StandardCharsets.UTF_8); }
    private static boolean finite(float v) { return !Float.isNaN(v) && !Float.isInfinite(v); }

    Area byId(String id) { for (Area a:areas) if (a.id.equals(id)) return a; return null; }
    int areaForBlock(int block) {
        String id=String.format(Locale.ROOT,"m%02d_%02d_%02d_%02d",block>>>24,(block>>>16)&255,(block>>>8)&255,block&255);
        for (int i=0;i<areas.size();++i) if (areas.get(i).id.equals(id)) return i;
        return -1;
    }

    // ---- floors ----

    /** Reads the area's floors and markers. Blocks; sets the area's state. */
    void loadGeometry(Area a) {
        synchronized (a) {
            if (a.state==READY || a.state==FAILED) return;
            a.state=LOADING;
        }
        try {
            File file=new File(new File(root,"maps"),a.id+".json");
            List<Layer> layers=new ArrayList<>(); List<Marker> markers=new ArrayList<>();
            int version=0; String geometry=null;
            try (JsonReader r=new JsonReader(new BufferedReader(new InputStreamReader(new FileInputStream(file),StandardCharsets.UTF_8),1<<16))) {
                r.setLenient(true);
                r.beginObject();
                while (r.hasNext()) {
                    String key=r.nextName();
                    if (key.equals("version")) version=r.nextInt();
                    else if (key.equals("geometry") && r.peek()==JsonToken.STRING) geometry=r.nextString();
                    else if (key.equals("layers") && r.peek()==JsonToken.BEGIN_ARRAY) {
                        r.beginArray();
                        while (r.hasNext()) layers.add(layer(r,version));
                        r.endArray();
                    } else if (key.equals("lamps") && r.peek()==JsonToken.BEGIN_ARRAY) {
                        r.beginArray();
                        while (r.hasNext()) {
                            AtlasFeatures.Lamp l=new AtlasFeatures.Lamp(); boolean pos=false;
                            r.beginObject();
                            while (r.hasNext()) {
                                String k=r.nextName();
                                if (k.equals("pos")) { float[] p=floats(r,3); if (p!=null) { l.x=p[0]; l.y=p[1]; l.z=p[2]; pos=true; } }
                                else if (k.equals("name") && r.peek()==JsonToken.STRING) l.name=r.nextString();
                                else r.skipValue();
                            }
                            r.endObject();
                            if (pos) { l.discovered=l.available=true; a.mapLamps.add(l); }
                        }
                        r.endArray();
                    } else if (key.equals("markers") && r.peek()==JsonToken.BEGIN_ARRAY) {
                        r.beginArray();
                        while (r.hasNext()) {
                            Marker m=new Marker(); boolean pos=false;
                            r.beginObject();
                            while (r.hasNext()) {
                                String k=r.nextName();
                                if (k.equals("pos")) { float[] p=floats(r,3); if (p!=null) { m.x=p[0]; m.y=p[1]; m.z=p[2]; pos=true; } }
                                else if (k.equals("kind") && r.peek()==JsonToken.STRING) m.kind=r.nextString();
                                else if (k.equals("name") && r.peek()==JsonToken.STRING) m.name=r.nextString();
                                else if (k.equals("entity_id") && r.peek()==JsonToken.NUMBER) m.entity=(int)r.nextLong();
                                else r.skipValue();
                            }
                            r.endObject();
                            if (pos && (m.kind.equals("ladder") || m.kind.equals("npc") || m.kind.equals("gate"))) markers.add(m);
                        }
                        r.endArray();
                    } else r.skipValue();
                }
                r.endObject();
            }
            if (layers.isEmpty()) throw new IOException("No floors in "+file.getName());
            boolean binary=false; for (Layer l:layers) if (l.blocks!=null) binary=true;
            if (binary) readBinary(new File(file.getParentFile(),geometry!=null ? geometry : a.id+".bin"),layers);
            a.layers=layers.toArray(new Layer[0]); a.markers=markers.toArray(new Marker[0]);
            Integer[] order=new Integer[a.layers.length]; for (int i=0;i<order.length;++i) order[i]=i;
            final Layer[] ls=a.layers;
            Arrays.sort(order,(p,q) -> Float.compare(ls[p].anchor,ls[q].anchor));
            a.floorOrder=new int[order.length]; for (int i=0;i<order.length;++i) a.floorOrder[i]=order[i];
            for (Layer l:a.layers) l.cache=buildCache(a,l,false,null,"plain");
            a.state=READY;
        } catch (Throwable e) {
            a.error=e.getMessage()!=null ? e.getMessage() : e.toString();
            a.state=FAILED;
            android.util.Log.w("Bloodborne","atlas area "+a.id+": "+e);
        }
    }

    private static float[] floats(JsonReader r,int n) throws IOException {
        if (r.peek()!=JsonToken.BEGIN_ARRAY) { r.skipValue(); return null; }
        float[] out=new float[n]; int i=0; boolean ok=true;
        r.beginArray();
        while (r.hasNext()) {
            if (r.peek()==JsonToken.NUMBER) { float v=(float)r.nextDouble(); if (i<n) out[i]=v; if (!finite(v)) ok=false; }
            else { r.skipValue(); ok=false; }
            ++i;
        }
        r.endArray();
        return ok && i==n ? out : null;
    }

    private static final class Buf {
        float[] a=new float[1024]; int n;
        void add(float v) { if (n==a.length) a=Arrays.copyOf(a,n*2); a[n++]=v; }
        float[] done() { return Arrays.copyOf(a,n); }
    }

    /** Appends the numbers of an array of fixed-size number arrays ([[x,z,...],...]); a row of another size or with
     *  a non-finite number is dropped. */
    private static int rows(JsonReader r,int width,Buf out) throws IOException {
        if (r.peek()!=JsonToken.BEGIN_ARRAY) { r.skipValue(); return 0; }
        r.beginArray();
        return rowsBody(r,width,out);
    }
    /** rows() after the array's beginning was read. */
    private static int rowsBody(JsonReader r,int width,Buf out) throws IOException {
        int count=0; float[] row=new float[width];
        while (r.hasNext()) {
            if (r.peek()!=JsonToken.BEGIN_ARRAY) { r.skipValue(); continue; }
            int i=0; boolean ok=true;
            r.beginArray();
            while (r.hasNext()) {
                if (r.peek()==JsonToken.NUMBER) { float v=(float)r.nextDouble(); if (i<width) row[i]=v; if (!finite(v)) ok=false; }
                else { r.skipValue(); ok=false; }
                ++i;
            }
            r.endArray();
            if (ok && i==width) { for (int k=0;k<width;++k) out.add(row[k]); ++count; }
        }
        r.endArray();
        return count;
    }
    /** Version 3: a block of the .bin file, [byteOffset, count], or rows of numbers (older versions). The pair when
     *  it is one; else null with the rows appended to out (the count in rowCount[0]). */
    private static long[] blockOrRows(JsonReader r,int width,Buf out,int[] rowCount) throws IOException {
        if (r.peek()!=JsonToken.BEGIN_ARRAY) { r.skipValue(); rowCount[0]=0; return null; }
        r.beginArray();
        if (r.hasNext() && r.peek()==JsonToken.NUMBER) {
            long[] pair=new long[2]; int i=0;
            while (r.hasNext()) { if (r.peek()==JsonToken.NUMBER) { long v=r.nextLong(); if (i<2) pair[i]=v; ++i; } else r.skipValue(); }
            r.endArray();
            return i==2 ? pair : new long[]{0,0};
        }
        rowCount[0]=rowsBody(r,width,out);
        return null;
    }

    private static Layer layer(JsonReader r,int version) throws IOException {
        Layer l=new Layer(); Buf tri=new Buf(), heights=new Buf(), rects=new Buf(), walls=new Buf();
        boolean anchorSet=false; int heightRows=0, triRows=0; int[] rc=new int[1];
        long[] triBlock=null, heightBlock=null, wallBlock=null, outlineBlock=null; int[] rings=null;
        r.beginObject();
        while (r.hasNext()) {
            String key=r.nextName();
            if (key.equals("height") && r.peek()==JsonToken.NUMBER) l.height=(float)r.nextDouble();
            else if (key.equals("span") && r.peek()==JsonToken.NUMBER) l.span=(float)r.nextDouble();
            else if (key.equals("anchor") && r.peek()==JsonToken.NUMBER) { l.anchor=(float)r.nextDouble(); anchorSet=true; }
            else if (key.equals("triangles")) { triBlock=blockOrRows(r,6,tri,rc); triRows=rc[0]; }
            else if (key.equals("heights")) { heightBlock=blockOrRows(r,3,heights,rc); heightRows=rc[0]; }
            else if (key.equals("outline") && r.peek()==JsonToken.BEGIN_ARRAY) { outlineBlock=blockOrRows(r,1,new Buf(),rc); }
            else if (key.equals("rings") && r.peek()==JsonToken.BEGIN_ARRAY) {
                Buf b=new Buf(); r.beginArray(); while (r.hasNext()) { if (r.peek()==JsonToken.NUMBER) b.add((float)r.nextLong()); else r.skipValue(); } r.endArray();
                float[] f=b.done(); rings=new int[f.length]; for (int i=0;i<f.length;++i) rings[i]=(int)f[i];
            }
            else if (key.equals("rects")) { rows(r,4,rects); }
            else if (key.equals("walls")) { wallBlock=blockOrRows(r,4,walls,rc); }
            else if (key.equals("outlines") && r.peek()==JsonToken.BEGIN_ARRAY) {
                r.beginArray();
                while (r.hasNext()) {
                    Buf ring=new Buf(); int n=rows(r,2,ring);
                    if (n>1) l.outlines.add(ring.done());
                }
                r.endArray();
            } else r.skipValue();
        }
        r.endObject();
        l.blocks=triBlock==null && heightBlock==null && wallBlock==null && outlineBlock==null ? null
            : new long[][]{triBlock,heightBlock,wallBlock,outlineBlock};
        l.rings=rings;
        if (version==1 || !anchorSet) { if (version==1) l.span=6; if (!anchorSet) l.anchor=l.height+3; }
        if (!(l.span>0)) l.span=6;
        // rects with no positive size are dropped
        float[] rr=rects.done(); int kept=0;
        for (int i=0;i<rr.length/4;++i) if (rr[i*4+2]>0 && rr[i*4+3]>0) { System.arraycopy(rr,i*4,rr,kept*4,4); ++kept; }
        l.rects=Arrays.copyOf(rr,kept*4);
        l.walls=walls.done();
        if (l.blocks!=null) { l.triCount=0; l.tri=new float[0]; return l; } // filled from the .bin file (loadGeometry)
        float[] t=tri.done(); float[] h=heightRows==triRows && heightRows>0 ? heights.done() : null;
        l.triCount=triRows;
        if (h!=null) {
            l.minY=Float.POSITIVE_INFINITY; l.maxY=Float.NEGATIVE_INFINITY;
            for (float y:h) { if (y<l.minY) l.minY=y; if (y>l.maxY) l.maxY=y; }
            l.hasVertexHeights=true;
        }
        sortTriangles(l,t,h);
        return l;
    }

    /** Reads the floors of a version 3 area from its .bin file: little-endian float32 blocks at the offsets the JSON
     *  lists (triangles 6 floats each, heights 3, walls 4, the outline as x, z pairs split into rings). */
    private static void readBinary(File bin,List<Layer> layers) throws IOException {
        try (java.nio.channels.FileChannel c=java.nio.channels.FileChannel.open(bin.toPath(),java.nio.file.StandardOpenOption.READ)) {
            long size=c.size();
            java.nio.ByteBuffer b=c.map(java.nio.channels.FileChannel.MapMode.READ_ONLY,0,size).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            for (Layer l:layers) {
                if (l.blocks==null) continue;
                long[] tb=l.blocks[0], hb=l.blocks[1], wb=l.blocks[2], ob=l.blocks[3];
                float[] t=tb!=null ? block(b,size,tb[0],tb[1]*6) : new float[0];
                float[] h=hb!=null && tb!=null && hb[1]==tb[1] ? block(b,size,hb[0],hb[1]*3) : null;
                l.triCount=t.length/6;
                if (h!=null) {
                    l.minY=Float.POSITIVE_INFINITY; l.maxY=Float.NEGATIVE_INFINITY;
                    for (float y:h) { if (y<l.minY) l.minY=y; if (y>l.maxY) l.maxY=y; }
                    l.hasVertexHeights=true;
                }
                l.walls=wb!=null ? block(b,size,wb[0],wb[1]*4) : new float[0];
                if (ob!=null && l.rings!=null) {
                    float[] o=block(b,size,ob[0],ob[1]); int at=0;
                    for (int points:l.rings) {
                        if (points<0 || (at+points)*2>o.length) break;
                        if (points>1) l.outlines.add(Arrays.copyOfRange(o,at*2,(at+points)*2));
                        at+=points;
                    }
                }
                sortTriangles(l,t,h);
                l.blocks=null; l.rings=null;
            }
        }
    }
    private static float[] block(java.nio.ByteBuffer b,long size,long offset,long floats) throws IOException {
        if (offset<0 || floats<0 || offset+floats*4>size || floats>Integer.MAX_VALUE/4) throw new IOException("A block lies outside the .bin file");
        float[] out=new float[(int)floats];
        java.nio.ByteBuffer view=b.duplicate().order(java.nio.ByteOrder.LITTLE_ENDIAN);
        view.position((int)offset);
        view.asFloatBuffer().get(out);
        return out;
    }

    /** Frees the floors of the areas used longest ago, keeping at most `keep` areas loaded (and never `protectA`/`protectB`).
     *  Main thread. */
    void evict(int keep,Area protectA,Area protectB) {
        List<Area> loaded=new ArrayList<>();
        for (Area a:areas) if (a.state==READY) loaded.add(a);
        if (loaded.size()<=keep) return;
        Collections.sort(loaded,(x,y) -> Long.compare(x.used,y.used));
        for (Area a:loaded) {
            if (loaded.size()<=keep) break;
            if (a==protectA || a==protectB) continue;
            a.layers=new Layer[0]; a.floorOrder=new int[0]; a.markers=new Marker[0]; a.mapLamps.clear(); a.state=UNLOADED;
            loaded.remove(a); break; // one per call is enough as areas load one at a time
        }
    }

    // ---- spatial order ----

    private static long morton(int x,int z) {
        long r=0;
        for (int i=0;i<16;++i) r|=((long)((x>>i)&1)<<(2*i))|((long)((z>>i)&1)<<(2*i+1));
        return r;
    }
    /** Indices of n points ordered along a Z curve (cells of 16 world units). */
    static int[] mortonOrder(float[] mx,float[] mz,int n) {
        float minx=Float.POSITIVE_INFINITY, minz=Float.POSITIVE_INFINITY;
        for (int i=0;i<n;++i) { if (mx[i]<minx) minx=mx[i]; if (mz[i]<minz) minz=mz[i]; }
        long[] keys=new long[n];
        for (int i=0;i<n;++i) {
            int cx=Math.max(0,Math.min(65535,(int)Math.floor((mx[i]-minx)/16))), cz=Math.max(0,Math.min(65535,(int)Math.floor((mz[i]-minz)/16)));
            keys[i]=(morton(cx,cz)<<32)|i; // 32 bits of curve, then the index
        }
        Arrays.sort(keys);
        int[] order=new int[n];
        for (int i=0;i<n;++i) order[i]=(int)(keys[i]&0xffffffffL);
        return order;
    }

    private static void sortTriangles(Layer l,float[] t,float[] h) {
        int n=l.triCount;
        float[] mx=new float[n], mz=new float[n];
        for (int i=0;i<n;++i) { mx[i]=(t[i*6]+t[i*6+2]+t[i*6+4])/3; mz[i]=(t[i*6+1]+t[i*6+3]+t[i*6+5])/3; }
        int[] order=mortonOrder(mx,mz,n);
        l.tri=new float[n*6]; if (h!=null) l.triY=new float[n*3];
        for (int i=0;i<n;++i) {
            System.arraycopy(t,order[i]*6,l.tri,i*6,6);
            if (h!=null) System.arraycopy(h,order[i]*3,l.triY,i*3,3);
        }
        int chunks=(n+CHUNK_TRIANGLES-1)/CHUNK_TRIANGLES;
        l.chunkBox=new float[chunks*4];
        for (int c=0;c<chunks;++c) {
            float x0=Float.POSITIVE_INFINITY, z0=x0, x1=Float.NEGATIVE_INFINITY, z1=x1;
            for (int i=c*CHUNK_TRIANGLES;i<Math.min(n,(c+1)*CHUNK_TRIANGLES);++i)
                for (int k=0;k<3;++k) {
                    float x=l.tri[i*6+k*2], z=l.tri[i*6+k*2+1];
                    if (x<x0) x0=x; if (x>x1) x1=x; if (z<z0) z0=z; if (z>z1) z1=z;
                }
            l.chunkBox[c*4]=x0; l.chunkBox[c*4+1]=z0; l.chunkBox[c*4+2]=x1; l.chunkBox[c*4+3]=z1;
        }
    }

    // ---- fog: what shows ----

    /** The triangle (index i) of the layer is shown: its centre and its three corners are revealed. */
    private boolean triangleRevealed(Area a,Layer l,int i,Object flagsObj) {
        AtlasFeatures.Flags flags=(AtlasFeatures.Flags)flagsObj;
        float[] t=l.tri; int o=i*6;
        float fy=l.hasVertexHeights ? (l.triY[i*3]+l.triY[i*3+1]+l.triY[i*3+2])/3 : l.anchor;
        if (!features.revealed(a.id,(t[o]+t[o+2]+t[o+4])/3,fy,(t[o+1]+t[o+3]+t[o+5])/3,true,flags)) return false;
        for (int k=0;k<3;++k)
            if (!features.revealed(a.id,t[o+2*k],l.hasVertexHeights ? l.triY[i*3+k] : l.anchor,t[o+2*k+1],true,flags)) return false;
        return true;
    }
    private boolean segmentRevealed(Area a,Layer l,float x1,float z1,float x2,float z2,AtlasFeatures.Flags flags) {
        return features.revealed(a.id,(x1+x2)*.5f,l.anchor,(z1+z2)*.5f,true,flags)
            && features.revealed(a.id,x1,l.anchor,z1,true,flags) && features.revealed(a.id,x2,l.anchor,z2,true,flags);
    }

    /** The mask and the paths of a floor, for the fog (flags) or without it (fog false). Blocks: runs off the UI thread. */
    Cache buildCache(Area a,Layer l,boolean fog,AtlasFeatures.Flags flags,Object key) {
        Cache c=new Cache(); c.key=key;
        boolean useFog=fog && features.present && flags!=null;
        if (useFog) {
            c.mask=new byte[l.triCount];
            for (int i=0;i<l.triCount;++i) c.mask[i]=(byte)(triangleRevealed(a,l,i,flags) ? 1 : 0);
        }
        // outline segments (rings closed)
        int segs=0; for (float[] ring:l.outlines) segs+=ring.length/2;
        float[] seg=new float[segs*4]; int n=0;
        for (float[] ring:l.outlines) {
            int m=ring.length/2;
            for (int i=0;i<m;++i) {
                int j=(i+1)%m;
                seg[n*4]=ring[i*2]; seg[n*4+1]=ring[i*2+1]; seg[n*4+2]=ring[j*2]; seg[n*4+3]=ring[j*2+1]; ++n;
            }
        }
        c.outline=chunks(a,l,seg,n,useFog ? flags : null);
        c.walls=chunks(a,l,l.walls,l.walls.length/4,useFog ? flags : null);
        return c;
    }

    private Chunks chunks(Area a,Layer l,float[] seg,int n,AtlasFeatures.Flags fogFlags) {
        Chunks out=new Chunks();
        if (n==0) return out;
        float[] mx=new float[n], mz=new float[n];
        for (int i=0;i<n;++i) { mx[i]=(seg[i*4]+seg[i*4+2])*.5f; mz[i]=(seg[i*4+1]+seg[i*4+3])*.5f; }
        int[] order=mortonOrder(mx,mz,n);
        List<Path> paths=new ArrayList<>(); List<float[]> boxes=new ArrayList<>();
        Path path=null; float x0=0, z0=0, x1=0, z1=0; int inPath=0;
        for (int k=0;k<n;++k) {
            int i=order[k];
            float ax=seg[i*4], az=seg[i*4+1], bx=seg[i*4+2], bz=seg[i*4+3];
            if (fogFlags!=null && !segmentRevealed(a,l,ax,az,bx,bz,fogFlags)) continue;
            if (path==null) { path=new Path(); x0=Math.min(ax,bx); z0=Math.min(az,bz); x1=Math.max(ax,bx); z1=Math.max(az,bz); inPath=0; }
            path.moveTo(ax,az); path.lineTo(bx,bz);
            x0=Math.min(x0,Math.min(ax,bx)); z0=Math.min(z0,Math.min(az,bz)); x1=Math.max(x1,Math.max(ax,bx)); z1=Math.max(z1,Math.max(az,bz));
            if (++inPath==CHUNK_SEGMENTS) { paths.add(path); boxes.add(new float[]{x0,z0,x1,z1}); path=null; }
        }
        if (path!=null) { paths.add(path); boxes.add(new float[]{x0,z0,x1,z1}); }
        out.paths=paths.toArray(new Path[0]); out.box=new float[boxes.size()*4];
        for (int i=0;i<boxes.size();++i) System.arraycopy(boxes.get(i),0,out.box,i*4,4);
        return out;
    }

    // ---- the player's place ----

    private static boolean inTriangle(float[] t,int o,float x,float z) {
        if (x<Math.min(t[o],Math.min(t[o+2],t[o+4])) || x>Math.max(t[o],Math.max(t[o+2],t[o+4]))
            || z<Math.min(t[o+1],Math.min(t[o+3],t[o+5])) || z>Math.max(t[o+1],Math.max(t[o+3],t[o+5]))) return false;
        float ab=(t[o+2]-t[o])*(z-t[o+1])-(t[o+3]-t[o+1])*(x-t[o]);
        float bc=(t[o+4]-t[o+2])*(z-t[o+3])-(t[o+5]-t[o+3])*(x-t[o+2]);
        float ca=(t[o]-t[o+4])*(z-t[o+5])-(t[o+1]-t[o+5])*(x-t[o+4]);
        return (ab>=-.0001f && bc>=-.0001f && ca>=-.0001f) || (ab<=.0001f && bc<=.0001f && ca<=.0001f);
    }
    private static boolean triangleHeightAt(Layer l,int i,float x,float z,float[] y) {
        float[] t=l.tri; int o=i*6;
        float x0=t[o], z0=t[o+1], x1=t[o+2], z1=t[o+3], x2=t[o+4], z2=t[o+5];
        float d=(z1-z2)*(x0-x2)+(x2-x1)*(z0-z2);
        if (Math.abs(d)<1.0e-8f) return false;
        float p=((z1-z2)*(x-x2)+(x2-x1)*(z-z2))/d, q=((z2-z0)*(x-x2)+(x0-x2)*(z-z2))/d, r=1-p-q;
        y[0]=p*l.triY[i*3]+q*l.triY[i*3+1]+r*l.triY[i*3+2];
        return !Float.isNaN(y[0]) && !Float.isInfinite(y[0]);
    }

    /** The floor the player stands on: the one whose surface under (x, z) is closest in height, else the layer whose
     *  anchor is closest (AutoLayer of bbport_map.cpp). -1 when the area has no floors. */
    static int autoLayer(Area a,float x,float y,float z) {
        int nearest=-1, local=-1; float distance=Float.POSITIVE_INFINITY, localDistance=Float.POSITIVE_INFINITY;
        float[] surface=new float[1];
        for (int li=0;li<a.layers.length;++li) {
            Layer l=a.layers[li];
            float d=Math.abs(y-l.anchor);
            if (d<distance) { distance=d; nearest=li; }
            for (int i=0;i<l.triCount;++i) {
                if (!inTriangle(l.tri,i*6,x,z)) continue;
                surface[0]=l.anchor;
                if (l.hasVertexHeights && !triangleHeightAt(l,i,x,z,surface)) continue;
                float sd=Math.abs(y-surface[0]);
                if (sd<localDistance) { localDistance=sd; local=li; }
            }
            if (l.triCount==0)
                for (int r=0;r<l.rects.length/4;++r)
                    if (x>=l.rects[r*4] && x<=l.rects[r*4]+l.rects[r*4+2] && z>=l.rects[r*4+1] && z<=l.rects[r*4+1]+l.rects[r*4+3]) {
                        if (d<localDistance) { localDistance=d; local=li; }
                        break;
                    }
        }
        return local>=0 ? local : nearest;
    }
    /** The floor with the most area (what a previewed area shows first). */
    static int primaryLayer(Area a) {
        if (a.layers.length==0) return -1;
        int best=0; float footprint=0;
        for (int li=0;li<a.layers.length;++li) {
            Layer l=a.layers[li]; float area=0;
            for (int i=0;i<l.triCount;++i) {
                float[] t=l.tri; int o=i*6;
                area+=Math.abs((t[o+2]-t[o])*(t[o+5]-t[o+1])-(t[o+4]-t[o])*(t[o+3]-t[o+1]))*.5f;
            }
            for (int r=0;r<l.rects.length/4;++r) area+=l.rects[r*4+2]*l.rects[r*4+3];
            if (area>footprint) { best=li; footprint=area; }
        }
        return best;
    }

    // ---- names ----

    private static boolean inRegion(Region r,float x,float z) {
        for (int i=0;i<r.tris.length/6;++i) if (inTriangle(r.tris,i*6,x,z)) return true;
        return false;
    }
    static String title(Area a,float x,float y,float z) {
        for (Region r:a.regions) if (r.main && y>=r.minY-1 && y<=r.maxY+2 && inRegion(r,x,z)) return r.name;
        return a.name;
    }
    static String subtitle(Area a,float x,float y,float z) {
        String title=title(a,x,y,z);
        for (Region r:a.regions) if (!r.name.equals(title) && y>=r.minY-1 && y<=r.maxY+2 && inRegion(r,x,z)) return r.name;
        Landmark nearest=null; float best=Float.POSITIVE_INFINITY;
        for (Landmark l:a.landmarks) {
            float dx=l.x-x, dz=l.z-z, dy=Math.abs(l.y-y);
            float horizontal=(float)Math.hypot(dx,dz);
            if (horizontal>l.maxHorizontal || dy>l.maxVertical) continue;
            float distance=(float)Math.hypot(horizontal,dy);
            if (distance<best) { best=distance; nearest=l; }
        }
        return nearest!=null ? nearest.prefix+nearest.name : "";
    }
}
