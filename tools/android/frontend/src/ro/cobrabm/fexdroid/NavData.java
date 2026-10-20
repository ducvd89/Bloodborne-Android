package ro.cobrabm.fexdroid;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/** The route graph of one area (maps/nav/&lt;area&gt;.bnv of map pack version 2, format BNV1 of tools/atlas/nav/pack.py) and
 *  the route search over it. Plain Java (no Android classes: tests/android-nav runs it on a desktop JDK).
 *
 *  The graph is a 0.5 m grid with one node per distinct floor in a cell; edges between neighbouring cells are stored as
 *  four link bytes per node (forward half of the 8-neighbourhood: target floor, ledge flag), plus one-way drop links,
 *  gated links (a door lies across them) and a JSON block with the virtual nodes and the ladder / lift / portal edges.
 *  Costs are not stored: a step costs its 3D length, a ledge +ledge_cost, a drop +drop_cost, ladders and lifts their own.
 *  Loaded into a CSR adjacency (int per arc: target node, 2 bits of arc type; arcs that need more, the gated ones and the
 *  ladder / lift / portal edges, are listed in a small sorted table keyed by arc index).
 *
 *  Search: A* with a binary heap on primitive arrays (no per-node objects), the straight 3D line as heuristic,
 *  the gates' states (NavGate) deciding which gated arcs may be used. One search at a time (route() is synchronized). */
final class NavData {
    static final int WALK=0, LEDGE=1, DROP=2, SPECIAL=3;                       // arc types (2 bits)
    static final int K_WALK=0, K_LEDGE=1, K_DROP=2, K_LADDER=3, K_LIFT=4, K_PORTAL=5; // kinds of a route segment
    private static final int NODE_BITS=21, NODE_MASK=(1<<NODE_BITS)-1;
    static final int OK=0, NO_ROUTE=1, LIMIT=2;
    /** Route markers besides K_DROP / K_LADDER / K_LIFT: a gate (door) the route goes through, a lever to operate before a lift or ladder. */
    static final int M_GATE=6, M_LEVER=7;

    final String id;
    final float cell, ox, oz, yRes;
    final int w, h, gridCount, count, gateCount;
    final float[] x, y, z;
    final int[] key;                    // grid nodes: cell index (ci*H+cj), sorted
    final int[] off, adj;               // CSR
    final int[] spArc, spGate; final byte[] spSide, spKind; final float[] spCost; // special arcs, sorted by arc index
    final float ledgeCost, dropCost;
    /** Memory of the arrays in bytes (rough). */
    long bytes;

    private NavData(String id,float cell,float ox,float oz,float yRes,int w,int h,int gridCount,int count,int gateCount,float[] x,float[] y,float[] z,int[] key,
                    int[] off,int[] adj,int[] spArc,int[] spGate,byte[] spSide,byte[] spKind,float[] spCost,float ledgeCost,float dropCost) {
        this.id=id; this.cell=cell; this.ox=ox; this.oz=oz; this.yRes=yRes; this.w=w; this.h=h; this.gridCount=gridCount; this.count=count; this.gateCount=gateCount;
        this.x=x; this.y=y; this.z=z; this.key=key; this.off=off; this.adj=adj;
        this.spArc=spArc; this.spGate=spGate; this.spSide=spSide; this.spKind=spKind; this.spCost=spCost; this.ledgeCost=ledgeCost; this.dropCost=dropCost;
        bytes=4L*(x.length*3+key.length+off.length+adj.length+spArc.length*4+(long)count*4);
    }

    // ---------------------------------------------------------------- reading

    /** Decodes a .bnv file. */
    static NavData load(String id,byte[] file) throws IOException {
        if (file.length<9 || file[0]!='B' || file[1]!='N' || file[2]!='V' || file[3]!='1') throw new IOException("Not a route graph file");
        ByteBuffer head=ByteBuffer.wrap(file).order(ByteOrder.LITTLE_ENDIAN);
        int version=file[4]&255;
        if (version!=1) throw new IOException("Route graph version "+version+" is not supported");
        long sections=head.getInt(5)&0xffffffffL;
        if (sections<1 || sections>32) throw new IOException("Damaged route graph (sections)");
        int at=9; int[] sid=new int[(int)sections], comp=new int[(int)sections], raw=new int[(int)sections];
        for (int i=0;i<sections;++i) { sid[i]=file[at]&255; comp[i]=head.getInt(at+1); raw[i]=head.getInt(at+5); at+=9; }
        byte[][] sec=new byte[16][];
        for (int i=0;i<sections;++i) {
            if (comp[i]<0 || raw[i]<0 || at+(long)comp[i]>file.length || sid[i]>=16) throw new IOException("Damaged route graph (section "+sid[i]+")");
            sec[sid[i]]=inflate(file,at,comp[i],raw[i]); at+=comp[i];
        }
        for (int s=1;s<=8;++s) if (sec[s]==null) throw new IOException("Damaged route graph (no section "+s+")");

        ByteBuffer hd=ByteBuffer.wrap(sec[1]).order(ByteOrder.LITTLE_ENDIAN);
        float cell=hd.getFloat(0), ox=hd.getFloat(4), oz=hd.getFloat(8);
        int w=hd.getInt(12), h=hd.getInt(16), n=hd.getInt(20);
        float ymin=hd.getFloat(24), yres=hd.getFloat(28);
        if (!(cell>0) || w<=0 || h<=0 || n<0 || (long)w*h>Integer.MAX_VALUE/2 || n>NODE_MASK-4096) throw new IOException("Damaged route graph (header)");

        int[] key=new int[n]; float[] gx=new float[n], gy=new float[n], gz=new float[n];
        long[] pos2={0}, pos3={0};
        long k=0, q=0;
        for (int i=0;i<n;++i) {
            k+=varint(sec[2],pos2); key[i]=(int)k;
            long zz=varint(sec[3],pos3); q+=(zz>>>1)^-(zz&1);
            gy[i]=ymin+q*yres;
            int ci=(int)(k/h), cj=(int)(k%h);
            gx[i]=ox+(ci+.5f)*cell; gz[i]=oz+(cj+.5f)*cell;
        }
        // the JSON block: virtual nodes, extra edges, parameters
        Object extraObj;
        try { extraObj=MiniJson.parse(new String(sec[7],"UTF-8")); }
        catch (MiniJson.ParseException e) { throw new IOException("Damaged route graph (extra): "+e.getMessage()); }
        if (!(extraObj instanceof Map)) throw new IOException("Damaged route graph (extra)");
        Map<?,?> extra=(Map<?,?>)extraObj;
        List<?> virt=list(extra.get("virt")), edges=list(extra.get("edges")), mechs=list(extra.get("mechs"));
        Map<?,?> params=extra.get("params") instanceof Map ? (Map<?,?>)extra.get("params") : new HashMap<Object,Object>();
        float ledgeCost=(float)number(params.get("ledge_cost"),8), dropCost=(float)number(params.get("drop_cost"),6);
        int count=n+virt.size();
        if (count>NODE_MASK) throw new IOException("Damaged route graph (too many nodes)");
        float[] x=Arrays.copyOf(gx,count), y=Arrays.copyOf(gy,count), z=Arrays.copyOf(gz,count);
        for (int i=0;i<virt.size();++i) {
            List<?> p=list(virt.get(i));
            if (p.size()!=3) throw new IOException("Damaged route graph (virtual node)");
            x[n+i]=(float)number(p.get(0),0); y[n+i]=(float)number(p.get(1),0); z[n+i]=(float)number(p.get(2),0);
        }

        // gated walk links: (u, direction, rank) -> gate and side
        ByteBuffer gb=ByteBuffer.wrap(sec[6]).order(ByteOrder.LITTLE_ENDIAN);
        long[] gp={0};
        int gcount=(int)varint(sec[6],gp);
        int[] gu=new int[gcount]; long acc=0;
        for (int i=0;i<gcount;++i) { acc+=varint(sec[6],gp); gu[i]=(int)acc; }
        int gAt=(int)gp[0];
        if (gAt+4L*gcount>sec[6].length) throw new IOException("Damaged route graph (gates)");
        HashMap<Long,Integer> gated=new HashMap<>();
        boolean[] gatedNode=new boolean[n];
        for (int i=0;i<gcount;++i) {
            int dir=sec[6][gAt+i]&255, rank=sec[6][gAt+gcount+i]&255, gate=sec[6][gAt+2*gcount+i]&255, side=sec[6][gAt+3*gcount+i];
            if (gu[i]<0 || gu[i]>=n || dir>3) throw new IOException("Damaged route graph (gate link)");
            gated.put(((long)gu[i]*4+dir)*256+rank,gate*4+(side+1));
            gatedNode[gu[i]]=true;
        }
        // multi links (a second target in the same neighbour cell)
        long[] mp={0};
        int mcount=(int)varint(sec[8],mp);
        int[] mu=new int[mcount]; acc=0;
        for (int i=0;i<mcount;++i) { acc+=varint(sec[8],mp); mu[i]=(int)acc; }
        int mAt=(int)mp[0];
        if (mAt+2L*mcount>sec[8].length) throw new IOException("Damaged route graph (multi links)");
        // drops
        long[] dp={0};
        int dcount=(int)varint(sec[5],dp);
        int[] du=new int[dcount]; acc=0;
        for (int i=0;i<dcount;++i) { acc+=varint(sec[5],dp); du[i]=(int)acc; }
        int dAt=(int)dp[0];
        if (dAt+4L*dcount>sec[5].length) throw new IOException("Damaged route graph (drops)");
        if (sec[4].length<4*n) throw new IOException("Damaged route graph (links)");

        Builder b=new Builder(count);
        // special edges collected on the first pass: u, v, kind, gate, side, cost, mode (0 both, 1 forward, -1 backward)
        IntList su=new IntList(), sv=new IntList(), sk=new IntList(), sg=new IntList(), ss=new IntList(), sm=new IntList();
        FloatList sc=new FloatList();
        for (int pass=0;pass<2;++pass) {
            b.pass(pass);
            if (pass==0) {
                for (int dir=0;dir<4;++dir) {
                    final int da=dir==0?1:dir==1?0:1, db=dir==0?0:dir==1?1:dir==2?1:-1;
                    final int step=da*h+db; int p=0;
                    for (int u=0;u<n;++u) {
                        int nib=sec[4][4*u+dir]&255;
                        if (nib==0) continue;
                        int v=target(key,n,p,key[u]+step,(nib&127)-1);
                        p=lower(key,n,p,key[u]+step);
                        if (v<0) throw new IOException("Damaged route graph (link target)");
                        int type=(nib&128)!=0 ? LEDGE : WALK;
                        Integer gt=gatedNode[u] ? gated.get(((long)u*4+dir)*256+((nib&127)-1)) : null;
                        if (gt!=null) { su.add(u); sv.add(v); sk.add(type); sg.add(gt>>2); ss.add((gt&3)-1); sm.add(0); sc.add(-1); }
                        else b.edge(u,v,type);
                    }
                }
                for (int i=0;i<mcount;++i) {
                    int u=mu[i], dir=sec[8][mAt+i]&255, nib=sec[8][mAt+mcount+i]&255;
                    if (u<0 || u>=n || dir>3 || (nib&127)==0) throw new IOException("Damaged route graph (multi link)");
                    int da=dir==0?1:dir==1?0:1, db=dir==0?0:dir==1?1:dir==2?1:-1;
                    int nk=key[u]+da*h+db;
                    int v=target(key,n,lower(key,n,0,nk),nk,(nib&127)-1);
                    if (v<0) throw new IOException("Damaged route graph (multi link target)");
                    int type=(nib&128)!=0 ? LEDGE : WALK;
                    Integer gt=gatedNode[u] ? gated.get(((long)u*4+dir)*256+((nib&127)-1)) : null;
                    if (gt!=null) { su.add(u); sv.add(v); sk.add(type); sg.add(gt>>2); ss.add((gt&3)-1); sm.add(0); sc.add(-1); }
                    else b.edge(u,v,type);
                }
                for (int i=0;i<dcount;++i) {
                    int u=du[i]; int o=sec[5][dAt+i]&255, rank=sec[5][dAt+dcount+i]&255, gate=(sec[5][dAt+2*dcount+i]&255)-1, side=sec[5][dAt+3*dcount+i];
                    if (u<0 || u>=n) throw new IOException("Damaged route graph (drop)");
                    int nk=key[u]+((o>>4)-8)*h+((o&15)-8);
                    int v=target(key,n,lower(key,n,0,nk),nk,rank);
                    if (v<0) throw new IOException("Damaged route graph (drop target)");
                    if (gate>=0) { su.add(u); sv.add(v); sk.add(K_DROP); sg.add(gate); ss.add(side); sm.add(1); sc.add(-1); }
                    else b.arc(u,v,DROP);
                }
                for (int i=0;i<edges.size();++i) {
                    List<?> e=list(edges.get(i));
                    if (e.size()<6) throw new IOException("Damaged route graph (edge)");
                    int u=(int)number(e.get(0),-1), v=(int)number(e.get(1),-1), t=(int)number(e.get(3),0), gate=(int)number(e.get(4),-1), mode=(int)number(e.get(5),0);
                    if (u<0 || v<0 || u>=count || v>=count) throw new IOException("Damaged route graph (edge nodes)");
                    int kind=t==2 ? K_LADDER : t==3 ? K_LIFT : K_PORTAL;
                    su.add(u); sv.add(v); sk.add(kind); sg.add(gate); ss.add(e.size()>6 ? (int)number(e.get(6),0) : 0); sm.add(mode); sc.add((float)number(e.get(2),1));
                }
                b.count(su,sv,sm);
            } else {
                // second pass: the same links, filled in
                for (int dir=0;dir<4;++dir) {
                    final int da=dir==0?1:dir==1?0:1, db=dir==0?0:dir==1?1:dir==2?1:-1;
                    final int step=da*h+db; int p=0;
                    for (int u=0;u<n;++u) {
                        int nib=sec[4][4*u+dir]&255;
                        if (nib==0) continue;
                        int v=target(key,n,p,key[u]+step,(nib&127)-1);
                        p=lower(key,n,p,key[u]+step);
                        if (gatedNode[u] && gated.containsKey(((long)u*4+dir)*256+((nib&127)-1))) continue;
                        b.edge(u,v,(nib&128)!=0 ? LEDGE : WALK);
                    }
                }
                for (int i=0;i<mcount;++i) {
                    int u=mu[i], dir=sec[8][mAt+i]&255, nib=sec[8][mAt+mcount+i]&255;
                    int da=dir==0?1:dir==1?0:1, db=dir==0?0:dir==1?1:dir==2?1:-1;
                    int nk=key[u]+da*h+db;
                    int v=target(key,n,lower(key,n,0,nk),nk,(nib&127)-1);
                    if (gatedNode[u] && gated.containsKey(((long)u*4+dir)*256+((nib&127)-1))) continue;
                    b.edge(u,v,(nib&128)!=0 ? LEDGE : WALK);
                }
                for (int i=0;i<dcount;++i) {
                    int u=du[i]; int o=sec[5][dAt+i]&255, rank=sec[5][dAt+dcount+i]&255, gate=(sec[5][dAt+2*dcount+i]&255)-1;
                    if (gate>=0) continue;
                    int nk=key[u]+((o>>4)-8)*h+((o&15)-8);
                    b.arc(u,lowerTarget(key,n,nk,rank),DROP);
                }
                b.specials(su,sv,sk,sg,ss,sm,sc);
            }
        }
        b.sortSpecials();
        NavData d=new NavData(id,cell,ox,oz,yres,w,h,n,count,mechs.size(),x,y,z,key,b.off,b.adj,b.spArc,b.spGate,b.spSide,b.spKind,b.spCost,ledgeCost,dropCost);
        return d;
    }

    /** First index p' >= p with key[p'] >= nk (the keys are sorted: the pointer only moves forward). */
    private static int lower(int[] key,int n,int p,int nk) {
        if (p>0 && key[p-1]>=nk) return lowerBin(key,n,nk); // the caller's pointer is ahead
        if (p==0) return lowerBin(key,n,nk);                // no useful start: binary search (a scan from 0 would be quadratic)
        while (p<n && key[p]<nk) ++p;
        return p;
    }
    private static int lowerBin(int[] key,int n,int nk) {
        int lo=0, hi=n;
        while (lo<hi) { int m=(lo+hi)>>>1; if (key[m]<nk) lo=m+1; else hi=m; }
        return lo;
    }
    /** Node index of floor `rank` of the cell nk, advancing the pointer first; -1 when the cell or floor does not exist. */
    private static int target(int[] key,int n,int p,int nk,int rank) {
        p=lower(key,n,p,nk);
        int v=p+rank;
        return p<n && key[p]==nk && v<n && key[v]==nk ? v : -1;
    }
    private static int lowerTarget(int[] key,int n,int nk,int rank) {
        int p=lower(key,n,0,nk); return p+rank;
    }

    private static byte[] inflate(byte[] file,int at,int comp,int raw) throws IOException {
        Inflater in=new Inflater();
        try {
            in.setInput(file,at,comp);
            byte[] out=new byte[raw];
            int done=0;
            while (done<raw) {
                int got=in.inflate(out,done,raw-done);
                if (got==0 && (in.finished() || in.needsInput() || in.needsDictionary())) break;
                done+=got;
            }
            if (done!=raw) throw new IOException("Damaged route graph (a section is short)");
            return out;
        } catch (DataFormatException e) { throw new IOException("Damaged route graph: "+e.getMessage()); }
        finally { in.end(); }
    }
    private static long varint(byte[] a,long[] pos) throws IOException {
        long v=0; int shift=0; int p=(int)pos[0];
        for (;;) {
            if (p>=a.length || shift>56) throw new IOException("Damaged route graph (varint)");
            int b=a[p++]&255;
            v|=(long)(b&0x7f)<<shift;
            if ((b&0x80)==0) break;
            shift+=7;
        }
        pos[0]=p;
        return v;
    }
    private static List<?> list(Object o) { return o instanceof List ? (List<?>)o : new ArrayList<Object>(); }
    private static double number(Object o,double fallback) { return o instanceof Number ? ((Number)o).doubleValue() : fallback; }

    /** Two-pass CSR builder (count, then fill) with the special-arc table. */
    private static final class Builder {
        final int nodes; int[] deg, off, adj, pos; int pass; int[] spArc, spGate; byte[] spSide, spKind; float[] spCost; int specials;
        Builder(int nodes) { this.nodes=nodes; deg=new int[nodes+1]; }
        void pass(int p) {
            pass=p;
            if (p==1) {
                off=new int[nodes+1];
                long total=0;
                for (int i=0;i<nodes;++i) { off[i]=(int)total; total+=deg[i]; }
                off[nodes]=(int)total;
                adj=new int[(int)total]; pos=Arrays.copyOf(off,nodes+1);
            }
        }
        /** An undirected walk or ledge link. */
        void edge(int u,int v,int type) {
            if (pass==0) { ++deg[u]; ++deg[v]; }
            else { adj[pos[u]++]=v|(type<<NODE_BITS); adj[pos[v]++]=u|(type<<NODE_BITS); }
        }
        /** A one-way arc. */
        void arc(int u,int v,int type) {
            if (pass==0) ++deg[u]; else adj[pos[u]++]=v|(type<<NODE_BITS);
        }
        /** Pass 0: the degrees of the special edges (mode: 0 both, 1 forward, -1 backward). */
        void count(IntList su,IntList sv,IntList sm) {
            for (int i=0;i<su.n;++i) {
                int m=sm.a[i];
                if (m>=0) ++deg[su.a[i]];
                if (m<=0) ++deg[sv.a[i]];
            }
        }
        /** Pass 1: the special arcs, with their table entries. */
        void specials(IntList su,IntList sv,IntList sk,IntList sg,IntList ss,IntList sm,FloatList sc) {
            int total=0;
            for (int i=0;i<su.n;++i) total+=sm.a[i]==0 ? 2 : 1;
            spArc=new int[total]; spGate=new int[total]; spSide=new byte[total]; spKind=new byte[total]; spCost=new float[total];
            int at=0;
            for (int i=0;i<su.n;++i) {
                int m=sm.a[i], u=su.a[i], v=sv.a[i];
                if (m>=0) { int k=pos[u]++; adj[k]=v|(SPECIAL<<NODE_BITS); spArc[at]=k; spGate[at]=sg.a[i]; spSide[at]=(byte)ss.a[i]; spKind[at]=(byte)sk.a[i]; spCost[at]=sc.a[i]; ++at; }
                if (m<=0) { int k=pos[v]++; adj[k]=u|(SPECIAL<<NODE_BITS); spArc[at]=k; spGate[at]=sg.a[i]; spSide[at]=(byte)-ss.a[i]; spKind[at]=(byte)sk.a[i]; spCost[at]=sc.a[i]; ++at; }
            }
            specials=at;
        }
        void sortSpecials() {
            Integer[] order=new Integer[specials];
            for (int i=0;i<specials;++i) order[i]=i;
            final int[] arcs=spArc;
            Arrays.sort(order,(p,q) -> Integer.compare(arcs[p],arcs[q]));
            int[] a=new int[specials], g=new int[specials]; byte[] s=new byte[specials], k=new byte[specials]; float[] c=new float[specials];
            for (int i=0;i<specials;++i) { int o=order[i]; a[i]=spArc[o]; g[i]=spGate[o]; s[i]=spSide[o]; k[i]=spKind[o]; c[i]=spCost[o]; }
            spArc=a; spGate=g; spSide=s; spKind=k; spCost=c;
        }
    }
    private static final class IntList {
        int[] a=new int[64]; int n;
        void add(int v) { if (n==a.length) a=Arrays.copyOf(a,n*2); a[n++]=v; }
    }
    private static final class FloatList {
        float[] a=new float[64]; int n;
        void add(float v) { if (n==a.length) a=Arrays.copyOf(a,n*2); a[n++]=v; }
    }

    // ---------------------------------------------------------------- nodes

    int cellX(float px) { return (int)Math.floor((px-ox)/cell); }
    int cellZ(float pz) { return (int)Math.floor((pz-oz)/cell); }
    /** Index of the first grid node with key >= k. */
    private int first(int k) {
        int lo=0, hi=gridCount;
        while (lo<hi) { int m=(lo+hi)>>>1; if (key[m]<k) lo=m+1; else hi=m; }
        return lo;
    }

    /** Nearest grid nodes (at most `max`, nearest first) to a point in 3D within `radius` (plain Euclidean distance).
     *  Used for the player and for markers. */
    int[] snap(float px,float py,float pz,float radius,int max) {
        return snap(px,py,pz,radius,Float.NEGATIVE_INFINITY,Float.POSITIVE_INFINITY,true,max);
    }
    /** Nearest grid nodes in plan view whose height is in [yLo, yHi] (a floor of the map), within `radius`. */
    int[] snapBand(float px,float pz,float yLo,float yHi,float radius,int max) {
        return snap(px,0,pz,radius,yLo,yHi,false,max);
    }
    private int[] snap(float px,float py,float pz,float radius,float yLo,float yHi,boolean use3d,int max) {
        int c0=cellX(px), j0=cellZ(pz), r=(int)Math.ceil(radius/cell)+1;
        float[] bestD=new float[max]; int[] best=new int[max]; int found=0;
        float r2=radius*radius;
        for (int ci=Math.max(0,c0-r);ci<=Math.min(w-1,c0+r);++ci) {
            for (int cj=Math.max(0,j0-r);cj<=Math.min(h-1,j0+r);++cj) {
                int k=ci*h+cj;
                for (int i=first(k);i<gridCount && key[i]==k;++i) {
                    if (y[i]<yLo || y[i]>yHi) continue;
                    float dx=x[i]-px, dz=z[i]-pz, d=dx*dx+dz*dz;
                    if (use3d) { float dy=y[i]-py; d+=dy*dy; }
                    if (d>r2) continue;
                    int pos=found;
                    while (pos>0 && bestD[pos-1]>d) --pos;
                    if (pos>=max) continue;
                    int last=Math.min(found,max-1);
                    for (int m=last;m>pos;--m) { bestD[m]=bestD[m-1]; best[m]=best[m-1]; }
                    bestD[pos]=d; best[pos]=i;
                    if (found<max) ++found;
                }
            }
        }
        return Arrays.copyOf(best,found);
    }

    // ---------------------------------------------------------------- search

    /** The result of a search: the nodes of the path with the arc each was entered by. */
    static final class Path {
        int status; int[] nodes=new int[0], arcs=new int[0]; float cost; int expansions; long nanos;
    }

    private float[] g, hk; private int[] prev, prevArc, stamp, hv; private int hs, epoch;

    private void ensureSearch() {
        if (g!=null) return;
        g=new float[count]; prev=new int[count]; prevArc=new int[count]; stamp=new int[count];
        hk=new float[1<<14]; hv=new int[1<<14];
    }
    private void push(float f,int v) {
        if (hs==hk.length) { hk=Arrays.copyOf(hk,hs*2); hv=Arrays.copyOf(hv,hs*2); }
        int i=hs++;
        while (i>0) {
            int p=(i-1)>>1;
            if (hk[p]<=f) break;
            hk[i]=hk[p]; hv[i]=hv[p]; i=p;
        }
        hk[i]=f; hv[i]=v;
    }
    private void popTop() {
        int n=--hs;
        if (n==0) return;
        float f=hk[n]; int v=hv[n]; int i=0;
        for (;;) {
            int c=2*i+1;
            if (c>=n) break;
            if (c+1<n && hk[c+1]<hk[c]) ++c;
            if (hk[c]>=f) break;
            hk[i]=hk[c]; hv[i]=hv[c]; i=c;
        }
        hk[i]=f; hv[i]=v;
    }

    private float dist(int a,int b) {
        float dx=x[a]-x[b], dy=y[a]-y[b], dz=z[a]-z[b];
        return (float)Math.sqrt(dx*dx+dy*dy+dz*dz);
    }

    /** The cost of arc k (index into adj) from u, or -1 when the gates forbid it. */
    private float arcCost(int u,int k,int v,byte[] gateState) {
        int t=(adj[k]>>>NODE_BITS)&3;
        if (t==WALK) return dist(u,v);
        if (t==LEDGE) return dist(u,v)+ledgeCost;
        if (t==DROP) return dist(u,v)+dropCost;
        int s=Arrays.binarySearch(spArc,k);
        if (s<0) return -1;
        int gate=spGate[s], kind=spKind[s];
        if (gate>=0 && gateState!=null && gate<gateState.length) {
            int st=gateState[gate];
            if (st==NavGate.CLOSED) return -1;
            if (st==NavGate.ONEWAY && spSide[s]<=0) return -1; // closed: passable only starting from the operable side (it is opened on the way)
        }
        float c=spCost[s];
        if (c>=0) return c;
        return dist(u,v)+(kind==K_LEDGE ? ledgeCost : kind==K_DROP ? dropCost : 0);
    }

    /** A* from src to dst. gateState: per gate NavGate.OPEN / CLOSED / ONEWAY (null: all open). weight: heuristic factor
     *  (1 = optimal for the admissible part). Stops after maxExpansions or maxNanos (status LIMIT). */
    synchronized Path route(int src,int dst,byte[] gateState,int maxExpansions,long maxNanos,float weight) {
        long t0=System.nanoTime();
        Path out=new Path();
        ensureSearch();
        if (++epoch==Integer.MAX_VALUE) { Arrays.fill(stamp,0); epoch=1; }
        hs=0;
        g[src]=0; prev[src]=-1; prevArc[src]=-1; stamp[src]=epoch;
        push(weight*dist(src,dst),src);
        int expansions=0;
        boolean found=false;
        while (hs>0) {
            float f=hk[0]; int u=hv[0]; popTop();
            float gu=g[u];
            if (f>gu+weight*dist(u,dst)+1e-3f) continue; // stale entry
            if (u==dst) { found=true; break; }
            if ((++expansions&1023)==0 && (expansions>maxExpansions || System.nanoTime()-t0>maxNanos)) { out.status=LIMIT; break; }
            for (int k=off[u];k<off[u+1];++k) {
                int code=adj[k]; int v=code&NODE_MASK;
                float c=arcCost(u,k,v,gateState);
                if (c<0) continue;
                float ng=gu+c;
                if (stamp[v]!=epoch || ng<g[v]) {
                    stamp[v]=epoch; g[v]=ng; prev[v]=u; prevArc[v]=k;
                    push(ng+weight*dist(v,dst),v);
                }
            }
        }
        out.expansions=expansions; out.nanos=System.nanoTime()-t0;
        if (!found) { if (out.status!=LIMIT) out.status=NO_ROUTE; return out; }
        int len=1;
        for (int n=dst;n!=src;n=prev[n]) ++len;
        out.nodes=new int[len]; out.arcs=new int[len];
        int i=len-1;
        for (int n=dst;;n=prev[n]) { out.nodes[i]=n; out.arcs[i]=prevArc[n]; --i; if (n==src) break; }
        out.arcs[0]=-1;
        out.cost=g[dst]; out.status=OK;
        return out;
    }

    // ---------------------------------------------------------------- the route

    /** A route as a polyline (smoothed within a floor), with the kind of each segment and the places where it changes level. */
    static final class Route {
        String area=""; int n; float[] x, y, z; byte[] kind;   // kind[i]: how point i is reached from point i-1
        float[] cum; float length; float cost;
        byte[] gateState;                                      // the gate states the route was made with
        int marks; float[] mx=new float[0], my=new float[0], mz=new float[0]; byte[] mkind=new byte[0]; int[] mgate=new int[0];
        float destX, destY, destZ;

        /** The route's nearest point to (px, py, pz); returns the distance and puts the length travelled up to that point in out[0]. */
        float project(float px,float py,float pz,float[] out) {
            float best=Float.MAX_VALUE, at=0;
            for (int i=1;i<n;++i) {
                float ax=x[i-1], ay=y[i-1], az=z[i-1], bx=x[i], by=y[i], bz=z[i];
                float dx=bx-ax, dy=by-ay, dz=bz-az, l2=dx*dx+dy*dy+dz*dz;
                float t=l2>1e-9f ? ((px-ax)*dx+(py-ay)*dy+(pz-az)*dz)/l2 : 0;
                t=Math.max(0,Math.min(1,t));
                float qx=ax+dx*t-px, qy=ay+dy*t-py, qz=az+dz*t-pz, d=qx*qx+qy*qy+qz*qz;
                if (d<best) { best=d; at=cum[i-1]+(cum[i]-cum[i-1])*t; }
            }
            if (n==1) { float qx=x[0]-px, qy=y[0]-py, qz=z[0]-pz; best=qx*qx+qy*qy+qz*qz; }
            out[0]=at;
            return (float)Math.sqrt(best);
        }
    }

    /** Turns a search result into a route: the path's nodes become points, runs of plain walk steps are smoothed by line of
     *  sight on the grid (a straight line is used when every grid step along it has a plain walk link), the places where
     *  the route climbs, rides, drops or crosses a gate keep their points. */
    Route toRoute(Path p,byte[] gateState) {
        Route r=new Route(); r.area=id; r.cost=p.cost; r.gateState=gateState==null ? null : gateState.clone();
        int m=p.nodes.length;
        // the kind of the arc entering each node, and whether it is a plain walk arc
        byte[] kind=new byte[m]; boolean[] plain=new boolean[m]; int[] gate=new int[m];
        Arrays.fill(gate,-1);
        for (int i=1;i<m;++i) {
            int k=p.arcs[i], t=(adj[k]>>>NODE_BITS)&3;
            if (t==SPECIAL) {
                int s=Arrays.binarySearch(spArc,k);
                kind[i]=s>=0 ? spKind[s] : K_WALK; gate[i]=s>=0 ? spGate[s] : -1;
                plain[i]=false;
            } else { kind[i]=(byte)(t==DROP ? K_DROP : t==LEDGE ? K_LEDGE : K_WALK); plain[i]=t==WALK; }
        }
        // which points to keep
        boolean[] keep=new boolean[m];
        keep[0]=true; keep[m-1]=true;
        for (int i=1;i<m;++i) if (!plain[i]) { keep[i]=true; keep[i-1]=true; }
        int i=0;
        while (i<m-1) {
            if (!plain[i+1]) { ++i; continue; }
            int j=i+1; while (j+1<m && plain[j+1]) ++j; // the run i..j of plain steps
            int from=i;
            while (from<j) {
                int far=Math.min(j,from+48), next=from+1;
                for (int to=far;to>from+1;--to) if (lineOfSight(p.nodes[from],p.nodes[to])) { next=to; break; }
                keep[next]=true; from=next;
            }
            i=j;
        }
        int cnt=0; for (int q=0;q<m;++q) if (keep[q]) ++cnt;
        r.n=cnt; r.x=new float[cnt]; r.y=new float[cnt]; r.z=new float[cnt]; r.kind=new byte[cnt]; r.cum=new float[cnt];
        int[] markIdx=new int[cnt]; int marks=0;
        int at=0, last=-1;
        List<float[]> mk=new ArrayList<>(); List<Integer> mg=new ArrayList<>(); List<Byte> mkd=new ArrayList<>();
        for (int q=0;q<m;++q) {
            if (!keep[q]) continue;
            int node=p.nodes[q];
            r.x[at]=x[node]; r.y[at]=y[node]; r.z[at]=z[node];
            // the kind of the segment arriving here: the worst of the steps skipped over
            byte kd=K_WALK;
            for (int s=last+1;s<=q;++s) if (s>0 && kind[s]!=K_WALK && kind[s]!=K_LEDGE && kind[s]!=K_PORTAL) kd=kind[s];
            r.kind[at]=at==0 ? K_WALK : kd;
            if (at>0) { float dx=r.x[at]-r.x[at-1], dy=r.y[at]-r.y[at-1], dz=r.z[at]-r.z[at-1]; r.cum[at]=r.cum[at-1]+(float)Math.sqrt(dx*dx+dy*dy+dz*dz); }
            if (at>0 && (kd==K_DROP || kd==K_LADDER || kd==K_LIFT)) { mk.add(new float[]{r.x[at-1],r.y[at-1],r.z[at-1]}); mkd.add(kd); mg.add(-1); }
            for (int s=last+1;s<=q;++s) if (s>0 && gate[s]>=0) {
                boolean opened=gateState==null || gate[s]>=gateState.length || gateState[gate[s]]==NavGate.OPEN;
                float[] at3={x[p.nodes[s-1]],y[p.nodes[s-1]],z[p.nodes[s-1]]};
                if (kind[s]<=K_LEDGE) { mk.add(at3); mkd.add((byte)M_GATE); mg.add(gate[s]); }                  // a door crossed
                else if ((kind[s]==K_LIFT || kind[s]==K_LADDER) && !opened) { mk.add(at3); mkd.add((byte)M_LEVER); mg.add(gate[s]); } // lever to operate first
            }
            last=q; ++at;
        }
        r.length=r.cum[cnt-1];
        r.marks=mk.size(); r.mx=new float[r.marks]; r.my=new float[r.marks]; r.mz=new float[r.marks]; r.mkind=new byte[r.marks]; r.mgate=new int[r.marks];
        for (int q=0;q<r.marks;++q) { float[] f=mk.get(q); r.mx[q]=f[0]; r.my[q]=f[1]; r.mz[q]=f[2]; r.mkind[q]=mkd.get(q); r.mgate[q]=mg.get(q); }
        int d=p.nodes[m-1]; r.destX=x[d]; r.destY=y[d]; r.destZ=z[d];
        return r;
    }

    /** A straight line from grid node a to grid node b along which every step between neighbouring cells has a plain walk
     *  arc (a grid line, Bresenham); virtual nodes never see. */
    boolean lineOfSight(int a,int b) {
        if (a>=gridCount || b>=gridCount) return false;
        int ca=key[a]/h, ja=key[a]%h, cb=key[b]/h, jb=key[b]%h;
        int dx=Math.abs(cb-ca), dz=Math.abs(jb-ja), sx=ca<cb?1:-1, sz=ja<jb?1:-1, err=dx-dz;
        int ci=ca, cj=ja, cur=a, steps=0;
        float ya=y[a], yb=y[b];
        int total=Math.max(dx,dz);
        while (ci!=cb || cj!=jb) {
            int e2=2*err, ni=ci, nj=cj;
            if (e2>-dz) { err-=dz; ni+=sx; }
            if (e2<dx) { err+=dx; nj+=sz; }
            ++steps;
            int want=ni*h+nj; float ty=ya+(yb-ya)*steps/Math.max(1,total);
            int best=-1; float bd=Float.MAX_VALUE;
            for (int k=off[cur];k<off[cur+1];++k) {
                int code=adj[k];
                if (((code>>>NODE_BITS)&3)!=WALK) continue;
                int v=code&NODE_MASK;
                if (v>=gridCount || key[v]!=want) continue;
                float d=Math.abs(y[v]-ty);
                if (d<bd) { bd=d; best=v; }
            }
            if (best<0 || (ni!=ci && nj!=cj && !diagonalOk(cur,best,ci,cj,ni,nj))) return false;
            cur=best; ci=ni; cj=nj;
            if (steps>400) return false;
        }
        return cur==b || (key[cur]==key[b] && Math.abs(y[cur]-y[b])<0.6f);
    }
    /** Corner cutting: the neighbour cells of a diagonal step must be walkable too, at about the same height. */
    private boolean diagonalOk(int cur,int next,int ci,int cj,int ni,int nj) {
        return Math.abs(y[cur]-y[next])<=1.0f;
    }
}
