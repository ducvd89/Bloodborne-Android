package ro.cobrabm.fexdroid;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewOutlineProvider;
import android.widget.OverScroller;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Executor;

/** The live map of Hunter's Atlas on the second screen: the area and floor the player is on, the player with the
 *  direction he faces, lamps, pickups, conversations, ladders and gates, the boss fog. A port of the renderer of
 *  bbport_map.cpp, bbport_map_features.cpp and bbport_atlas_style.h of Hunter's Atlas by MikeTat1988
 *  (https://github.com/MikeTat1988/HuntersAtlas, GPL-2.0-or-later) from ImGui to android.graphics.Canvas: floors are
 *  vertex meshes (colour by height, textured with the parchment) cached in flat arrays, outlines and walls cached
 *  Paths, all drawn under the canvas matrix of the map. The decorative cut hatching and interior fills of the
 *  original are not ported.
 *
 *  Touch: one finger pans (with fling) and stops following, pinch zooms around the fingers, a double tap zooms in at
 *  the tap (or resumes following when already close), a tap on a marker shows a label, the title opens the area list,
 *  the side buttons choose the floor (Auto follows the player), the Recenter button resumes everything. The view
 *  never takes focus. It is drawn when the game's tick changes, on touch and while a fling runs.
 *
 *  Mini mode (the floating map over the game): a small square that always follows the player (area and floor chosen
 *  by the game's state, one fixed zoom), with a title line at the top, the player, lamps, pickups and markers, a
 *  rounded clip and a thin border. It has no buttons or pickers, does not react to touch (onTouchEvent returns false
 *  and the view is not clickable: the touch controller and the camera under it keep working), keeps its last frame
 *  while the game loads, and draws only what falls inside its own small viewport. */
final class AtlasView extends View {
    /** What the screen controller does for the view. */
    interface Host { void changed(); }

    // ---- inputs ----
    private AtlasModel model;
    private AtlasState state;
    private Host host;
    private final Executor jobs;
    private final boolean mini;
    /** The zoom of the mini map in the map's zoom units (the full map's default is 9, its closest 25). */
    private static final float MINI_ZOOM=15f;
    private boolean optPickups=true, optFog=false; private String optNpc="all";
    private final Set<String> categories=new HashSet<>();
    private String modelError, notice;

    // ---- art ----
    private Bitmap background, parchment, icons;
    private BitmapShader parchmentShader, backgroundShader;
    private Typeface titleFace, bodyFace;
    private boolean artSet;

    // ---- view state ----
    private int selected=-1, current=-1, manualFloor=-1, autoActive=-1;
    private boolean preview, follow=true;
    private float cx, cz, zoom=9;
    private boolean lvalid, haveState; private float lx, ly, lz, lyaw; private int lblock;
    private String title="", subtitle="";
    private float autoX=Float.NaN, autoY, autoZ, titleX=Float.NaN, titleY, titleZ;
    private int seenFlagsVersion=-1, seenOptions=-1; private int pickupArea=-2;
    private List<AtlasFeatures.PickupView> pickups=new java.util.ArrayList<>();
    private final Set<AtlasModel.Area> queuedLoads=new HashSet<>();
    private final java.util.Map<AtlasModel.Area,String> queuedMasks=new java.util.HashMap<>();
    private int lastTick=-1;

    // ---- layout ----
    private float dens, scale, ms, ls, headerH, footerH;
    private final RectF canvasRect=new RectF();
    private float ccx, ccy;
    private final RectF btnUp=new RectF(), btnDown=new RectF(), btnAuto=new RectF(), btnRecenter=new RectF(), titleHit=new RectF();
    private float[] fadeVerts, fadeTexs; private int[] fadeColors;

    // ---- paints (made once) ----
    private final Paint bgPaint=new Paint(Paint.FILTER_BITMAP_FLAG), floorPaint=new Paint(), fadePaint=new Paint(), strokePaint=new Paint(Paint.ANTI_ALIAS_FLAG),
        fillPaint=new Paint(Paint.ANTI_ALIAS_FLAG), textPaint=new Paint(Paint.ANTI_ALIAS_FLAG), iconPaint=new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final PorterDuffColorFilter dimLamp=new PorterDuffColorFilter(Color.argb(125,100,100,100),PorterDuff.Mode.MULTIPLY),
        playerTint=new PorterDuffColorFilter(Color.argb(230,178,206,200),PorterDuff.Mode.MULTIPLY),
        compassTint=new PorterDuffColorFilter(Color.argb(170,174,166,143),PorterDuff.Mode.MULTIPLY);
    private final Rect src=new Rect(); private final RectF dst=new RectF(), tmpRect=new RectF();
    private static final int PAPER=Color.rgb(223,214,187), MUTED=Color.rgb(185,180,161), GOLD=Color.rgb(198,173,118), RULE=Color.argb(125,152,146,128);

    // ---- touch ----
    private final GestureDetector gestures;
    private final ScaleGestureDetector scaler;
    private final OverScroller scroller;
    private int flingX, flingY;
    private RectF pressed; private float pressX, pressY;
    private boolean pickerOpen, pickerDragging; private float pickerScroll, pickerLastY, pickerDownY;
    private final RectF pickerPanel=new RectF();
    private final Path scratch=new Path();

    // ---- tap targets of the last frame ----
    private static final int MAX_TARGETS=1024;
    private final float[] tx=new float[MAX_TARGETS], ty=new float[MAX_TARGETS], twx=new float[MAX_TARGETS], twz=new float[MAX_TARGETS];
    private final String[] tTitle=new String[MAX_TARGETS], tSub=new String[MAX_TARGETS];
    private int targets;
    private String cardTitle, cardSub; private float cardX, cardZ; private long cardUntil;

    AtlasView(Context context,Executor jobs,Typeface titleFace,Typeface bodyFace) { this(context,jobs,titleFace,bodyFace,false); }

    AtlasView(Context context,Executor jobs,Typeface titleFace,Typeface bodyFace,boolean mini) {
        super(context);
        this.jobs=jobs; this.titleFace=titleFace; this.bodyFace=bodyFace; this.mini=mini;
        setFocusable(false); setFocusableInTouchMode(false); setClickable(!mini);
        if (mini) {
            final float radius=miniRadius(context);
            setOutlineProvider(new ViewOutlineProvider() {
                @Override public void getOutline(View v,Outline o) { o.setRoundRect(0,0,v.getWidth(),v.getHeight(),radius); }
            });
            setClipToOutline(true);
        }
        categories.add("healing"); categories.add("insight"); categories.add("equipment"); categories.add("echoes");
        DisplayMetrics m=context.getResources().getDisplayMetrics(); dens=m.density;
        floorPaint.setFilterBitmap(true); floorPaint.setAntiAlias(false);
        strokePaint.setStyle(Paint.Style.STROKE); strokePaint.setStrokeCap(Paint.Cap.BUTT); strokePaint.setStrokeJoin(Paint.Join.ROUND);
        fillPaint.setStyle(Paint.Style.FILL);
        scroller=new OverScroller(context);
        scaler=new ScaleGestureDetector(context,new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override public boolean onScale(ScaleGestureDetector d) {
                if (!mapAvailable()) return true;
                zoomAt(d.getScaleFactor(),d.getFocusX(),d.getFocusY()); return true;
            }
        });
        scaler.setQuickScaleEnabled(false);
        gestures=new GestureDetector(context,new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent e) { scroller.forceFinished(true); return true; }
            @Override public boolean onScroll(MotionEvent e1,MotionEvent e2,float dx,float dy) {
                if (scaler.isInProgress() || !mapAvailable()) return true;
                follow=false; cardTitle=null;
                cx+=dx/zoom; cz-=dy/zoom; clampCenter(); changed(); invalidate(); return true;
            }
            @Override public boolean onFling(MotionEvent e1,MotionEvent e2,float vx,float vy) {
                if (scaler.isInProgress() || !mapAvailable()) return true;
                flingX=flingY=0;
                scroller.fling(0,0,(int)vx,(int)vy,-1000000,1000000,-1000000,1000000);
                postInvalidateOnAnimation(); return true;
            }
            @Override public boolean onDoubleTap(MotionEvent e) { doubleTap(e.getX(),e.getY()); return true; }
            @Override public boolean onSingleTapConfirmed(MotionEvent e) { tap(e.getX(),e.getY()); return true; }
        });
    }

    // ---- wiring ----

    void setArt(Bitmap background,Bitmap parchment,Bitmap icons) {
        this.background=background; this.parchment=parchment; this.icons=icons; artSet=true;
        parchmentShader=parchment!=null ? new BitmapShader(parchment,Shader.TileMode.CLAMP,Shader.TileMode.CLAMP) : null;
        backgroundShader=background!=null ? new BitmapShader(background,Shader.TileMode.CLAMP,Shader.TileMode.CLAMP) : null;
        floorPaint.setShader(parchmentShader);
        fadePaint.setShader(backgroundShader);
        buildFade(); invalidate();
    }
    void setModel(AtlasModel model,String error) { this.model=model; this.modelError=error; seenFlagsVersion=-1; selected=-1; current=-1; onState(); invalidate(); }
    void setState(AtlasState state) { this.state=state; }
    boolean isMini() { return mini; }
    boolean artSet() { return artSet; }
    void setBodyFace(Typeface face) { if (face!=null && face!=bodyFace) { bodyFace=face; invalidate(); } }
    AtlasModel model() { return model; }
    private static float miniRadius(Context context) { return 10*context.getResources().getDisplayMetrics().density; }
    void setHost(Host host) { this.host=host; }
    /** pickups: the markers; npc: "all", "new" or "hidden"; fog: boss fog. */
    void setOptions(boolean pickups,String npc,boolean fog) {
        if (pickups==optPickups && npc.equals(optNpc) && fog==optFog) return;
        optPickups=pickups; optNpc=npc; optFog=fog; seenFlagsVersion=-1; pickupArea=-2;
        scheduleMasks(); invalidate();
    }
    void setNotice(String text) { notice=text; invalidate(); }
    private void changed() { if (host!=null) host.changed(); }

    // ---- the game's state ----

    /** The game's state changed (the screen controller polled it): follow the player. Main thread. */
    void onState() {
        if (state==null) return;
        if (mini && lvalid && state.mapped && !state.valid) return; // loading: the mini map keeps its last frame
        haveState=state.mapped; lvalid=state.mapped && state.valid;
        lx=state.x; ly=state.y; lz=state.z; lyaw=state.yaw; lblock=state.block;
        current=model!=null && lvalid ? model.areaForBlock(lblock) : -1;
        if (!preview && current!=selected) { selected=current; follow=true; manualFloor=-1; zoom=defaultZoom(); cardTitle=null; autoX=Float.NaN; titleX=Float.NaN; pickupArea=-2; }
        AtlasModel.Area a=area();
        if (a!=null) {
            a.used=SystemClock.uptimeMillis();
            if (a.state==AtlasModel.UNLOADED) queueLoad(a);
            if (!preview && current==selected && follow) { cx=lx; cz=lz; }
            if (a.state==AtlasModel.READY) {
                scheduleMasks();
                if (!preview && lvalid && (lx!=autoX || ly!=autoY || lz!=autoZ)) { autoActive=AtlasModel.autoLayer(a,lx,ly,lz); autoX=lx; autoY=ly; autoZ=lz; }
                if (lvalid && (lx!=titleX || ly!=titleY || lz!=titleZ)) { title=AtlasModel.title(a,lx,ly,lz); subtitle=AtlasModel.subtitle(a,lx,ly,lz); titleX=lx; titleY=ly; titleZ=lz; }
            }
        }
        refreshFeatures();
        changed();
        invalidate();
    }

    private AtlasModel.Area area() {
        return model!=null && selected>=0 && selected<model.areas.size() ? model.areas.get(selected) : null;
    }
    private boolean mapAvailable() {
        AtlasModel.Area a=area();
        return a!=null && a.state==AtlasModel.READY && (preview || (lvalid && current==selected));
    }
    private int activeLayer(AtlasModel.Area a) {
        if (a.state!=AtlasModel.READY || a.layers.length==0) return -1;
        if (manualFloor>=0 && manualFloor<a.layers.length) return manualFloor;
        if (!preview && lvalid && current==selected) return autoActive>=0 && autoActive<a.layers.length ? autoActive : 0;
        return AtlasModel.primaryLayer(a);
    }

    private void queueLoad(final AtlasModel.Area a) {
        final AtlasModel m=model;
        if (m==null || !queuedLoads.add(a)) return;
        jobs.execute(() -> {
            m.loadGeometry(a);
            post(() -> {
                queuedLoads.remove(a);
                if (m!=model) return;
                AtlasModel.Area now=area();
                if (now!=null) now.used=SystemClock.uptimeMillis();
                m.evict(3,now,current>=0 && current<m.areas.size() ? m.areas.get(current) : null); // at most about three areas in memory
                for (AtlasModel.Area x:m.areas) if (x.state==AtlasModel.UNLOADED) queuedMasks.remove(x);
                onState();
            });
        });
    }

    /** The fog's masks and paths for the area on screen, when the flags changed (off the UI thread). */
    private void scheduleMasks() {
        final AtlasModel m=model; final AtlasModel.Area a=area();
        if (m==null || a==null || a.state!=AtlasModel.READY || state==null) return;
        final boolean fog=optFog && m.features.present;
        final String key=fog ? "fog"+state.flagsVersion : "plain";
        boolean stale=false;
        for (AtlasModel.Layer l:a.layers) { AtlasModel.Cache c=l.cache; if (c==null || !key.equals(c.key)) stale=true; }
        if (!stale || key.equals(queuedMasks.get(a))) return;
        queuedMasks.put(a,key);
        final AtlasFeatures.Flags flags=fog ? state.snapshot() : null;
        jobs.execute(() -> {
            for (AtlasModel.Layer l:a.layers) l.cache=m.buildCache(a,l,fog,flags,key);
            post(() -> { if (key.equals(queuedMasks.get(a))) queuedMasks.remove(a); scheduleMasks(); invalidate(); });
        });
    }

    private void refreshFeatures() {
        AtlasModel.Area a=area();
        if (a==null || model==null || state==null) { pickups.clear(); return; }
        int options=(optPickups?1:0)|(optFog?2:0)|(optNpc.hashCode()<<2);
        if (seenFlagsVersion==state.flagsVersion && seenOptions==options && pickupArea==selected) return;
        seenFlagsVersion=state.flagsVersion; seenOptions=options; pickupArea=selected;
        AtlasFeatures f=model.features; AtlasFeatures.Area fa=f.areas.get(a.id);
        if (fa!=null) {
            f.refreshLamps(fa,lvalid,state);
            pickups=optPickups ? f.pickups(fa,lvalid,optFog,state,categories) : new java.util.ArrayList<AtlasFeatures.PickupView>();
        } else pickups=new java.util.ArrayList<>();
        if (!optFog) { /* masks stay as they are */ } else scheduleMasks();
    }

    // ---- layout ----

    @Override protected void onSizeChanged(int w,int h,int ow,int oh) {
        super.onSizeChanged(w,h,ow,oh);
        relayout(w,h);
    }
    /** The display's metrics or configuration changed (another mode, a density change): laid out again at once. */
    @Override protected void onConfigurationChanged(android.content.res.Configuration config) {
        super.onConfigurationChanged(config);
        if (getWidth()>0 && getHeight()>0) { relayout(getWidth(),getHeight()); invalidate(); }
    }

    /** Sizes everything from the view's own pixel size and its context's density (for the second screen: the
     *  presentation's display, never the main one). The UI scale is the width over 1280, kept within the height over 600
     *  (a wide short display) and never below a quarter of the density (a small dense panel); the buttons are capped
     *  relative to the shorter side, so a 3.9" panel and a 27" monitor, portrait or landscape, both look right. */
    private void relayout(int w,int h) {
        float oldScale=scale;
        dens=getResources().getDisplayMetrics().density;
        if (mini) { // the whole view is the map; markers and lines sized to the small view, the zoom fixed
            scale=w/1280f;
            ms=Math.max(scale*4.5f,.55f*dens); ls=Math.max(scale*4f,.5f*dens);
            headerH=footerH=0; canvasRect.set(0,0,w,h); ccx=w*.5f; ccy=h*.5f;
            zoom=defaultZoom(); return;
        }
        float shorter=Math.min(w,h);
        scale=Math.max(Math.min(w/1280f,h/600f),.25f*dens);
        ms=scale*1.4f; ls=scale*1.3f;
        headerH=Math.max(114*scale,56*dens); footerH=Math.max(46*scale,28*dens);
        float side=Math.max(18*scale,6*dens);
        canvasRect.set(side,headerH,w-side,h-footerH);
        ccx=canvasRect.centerX(); ccy=canvasRect.centerY();
        float b=Math.min(Math.max(52*dens,44*scale),.16f*shorter), gap=6*dens;
        float right=canvasRect.right-10*dens, midY=canvasRect.centerY();
        btnAuto.set(right-b,midY-b/2,right,midY+b/2);
        btnUp.set(right-b,btnAuto.top-gap-b,right,btnAuto.top-gap);
        btnDown.set(right-b,btnAuto.bottom+gap,right,btnAuto.bottom+gap+b);
        float rw=Math.min(Math.max(128*dens,150*scale),.32f*w), rh=Math.min(Math.max(48*dens,40*scale),.13f*shorter);
        btnRecenter.set(canvasRect.right-10*dens-rw,canvasRect.bottom-10*dens-rh,canvasRect.right-10*dens,canvasRect.bottom-10*dens);
        titleHit.set(w*.08f,0,w*.92f,headerH);
        if (zoom==9) zoom=defaultZoom();
        else if (oldScale>0 && oldScale!=scale) zoom=Math.max(minZoom(),Math.min(maxZoom(),zoom*scale/oldScale)); // the same part of the map
        buildFade();
    }

    /** The background beneath the map's edge, as four trapezoids (BbMapArt::DrawBackgroundFade). */
    private void buildFade() {
        if (background==null || canvasRect.isEmpty() || getWidth()==0) return;
        float w=Math.min(34*scale,Math.min(canvasRect.width(),canvasRect.height())*.5f);
        float[][] outer={{canvasRect.left,canvasRect.top},{canvasRect.right,canvasRect.top},{canvasRect.right,canvasRect.bottom},{canvasRect.left,canvasRect.bottom}};
        float[][] inner={{canvasRect.left+w,canvasRect.top+w},{canvasRect.right-w,canvasRect.top+w},{canvasRect.right-w,canvasRect.bottom-w},{canvasRect.left+w,canvasRect.bottom-w}};
        fadeVerts=new float[4*6*2]; fadeTexs=new float[4*6*2]; fadeColors=new int[4*6];
        int n=0; int[] idx={0,1,2,0,2,3};
        for (int side=0;side<4;++side) {
            int next=(side+1)%4;
            float[][] pos={outer[side],outer[next],inner[next],inner[side]};
            for (int i:idx) {
                fadeVerts[n*2]=pos[i][0]; fadeVerts[n*2+1]=pos[i][1];
                fadeTexs[n*2]=pos[i][0]/getWidth()*background.getWidth(); fadeTexs[n*2+1]=pos[i][1]/getHeight()*background.getHeight();
                fadeColors[n]=i<2 ? 0xffffffff : 0x00ffffff; ++n;
            }
        }
    }

    // ---- coordinates ----

    private float sx(float x) { return ccx+(x-cx)*zoom; }
    private float sy(float z) { return ccy-(z-cz)*zoom; }
    private boolean laidOut;
    private float defaultZoom() { return (mini ? MINI_ZOOM : 9)*(scale>0 ? scale : 1); }
    private float minZoom() { return .15f*scale; }
    private float maxZoom() { return 25*scale; }

    private void zoomAt(float factor,float fx,float fy) {
        float nz=Math.max(minZoom(),Math.min(maxZoom(),zoom*factor));
        if (!follow) { // keep the world point under the fingers where it is
            float wx=cx+(fx-ccx)/zoom, wz=cz-(fy-ccy)/zoom;
            cx=wx-(fx-ccx)/nz; cz=wz+(fy-ccy)/nz;
        }
        zoom=nz; clampCenter(); changed(); invalidate();
    }

    /** The centre stays within the area's bounds and a margin: the map cannot be thrown off the screen. */
    private void clampCenter() {
        AtlasModel.Area a=area();
        if (a==null) return;
        float mx=(a.bounds[2]-a.bounds[0])*.25f, mz=(a.bounds[3]-a.bounds[1])*.25f;
        cx=Math.max(a.bounds[0]-mx,Math.min(a.bounds[2]+mx,cx)); cz=Math.max(a.bounds[1]-mz,Math.min(a.bounds[3]+mz,cz));
    }

    private void recenter() {
        preview=false; manualFloor=-1; follow=true; zoom=defaultZoom(); cardTitle=null;
        selected=current; autoX=Float.NaN; titleX=Float.NaN; pickupArea=-2;
        if (selected>=0) { cx=lx; cz=lz; }
        scroller.forceFinished(true);
        onState();
    }

    private void doubleTap(float x,float y) {
        if (!mapAvailable() || !canvasRect.contains(x,y)) return;
        float closeZoom=defaultZoom()*1.9f;
        if (!follow && zoom>=closeZoom*.9f) { recenter(); return; }          // already zoomed in: back to the player
        if (follow) { zoom=zoom>=closeZoom*.9f ? defaultZoom() : Math.min(maxZoom(),zoom*1.9f); changed(); invalidate(); return; }
        zoomAt(1.9f,x,y);
    }

    private void tap(float x,float y) {
        if (!canvasRect.contains(x,y)) return;
        float radius=40*dens, best=radius*radius; int hit=-1;
        for (int i=0;i<targets;++i) { float dx=tx[i]-x, dy=ty[i]-y, d=dx*dx+dy*dy; if (d<=best) { best=d; hit=i; } }
        if (hit<0) { cardTitle=null; invalidate(); return; }
        cardTitle=tTitle[hit]; cardSub=tSub[hit]; cardX=twx[hit]; cardZ=twz[hit]; cardUntil=SystemClock.uptimeMillis()+8000;
        postInvalidateDelayed(8100); invalidate();
    }

    /** Previews an area (the list): its floors, centred and fitted; the map stops following. */
    private void pickArea(int index) {
        if (model==null || index<0 || index>=model.areas.size()) return;
        AtlasModel.Area a=model.areas.get(index);
        selected=index; preview=true; follow=false; manualFloor=-1; cardTitle=null; pickupArea=-2; title=a.name; subtitle="";
        cx=(a.bounds[0]+a.bounds[2])*.5f; cz=(a.bounds[1]+a.bounds[3])*.5f;
        zoom=Math.max(minZoom(),Math.min(maxZoom(),Math.min(canvasRect.width()/(a.bounds[2]-a.bounds[0]),canvasRect.height()/(a.bounds[3]-a.bounds[1]))));
        scroller.forceFinished(true);
        if (a.state==AtlasModel.UNLOADED) queueLoad(a);
        titleX=Float.NaN; scheduleMasks(); changed(); invalidate();
    }

    private void floor(int step) {
        AtlasModel.Area a=area();
        if (a==null || a.state!=AtlasModel.READY || a.layers.length<2) return;
        int active=activeLayer(a), pos=0;
        for (int i=0;i<a.floorOrder.length;++i) if (a.floorOrder[i]==active) pos=i;
        pos=Math.max(0,Math.min(a.floorOrder.length-1,pos+step));
        manualFloor=a.floorOrder[pos]; changed(); invalidate();
    }

    // ---- touch ----

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (mini) return false; // never consumes a touch: the controller and the camera underneath get it
        int action=e.getActionMasked();
        if (pickerOpen) return pickerTouch(e);
        if (action==MotionEvent.ACTION_DOWN) {
            pressed=null;
            for (RectF r:new RectF[]{btnUp,btnDown,btnAuto,btnRecenter,titleHit}) {
                if (r.contains(e.getX(),e.getY()) && buttonShown(r)) { pressed=r; pressX=e.getX(); pressY=e.getY(); break; }
            }
            if (pressed!=null) { scroller.forceFinished(true); return true; }
        }
        if (pressed!=null) {
            if (action==MotionEvent.ACTION_UP) {
                RectF r=pressed; pressed=null;
                if (r.contains(e.getX(),e.getY())) press(r);
            } else if (action==MotionEvent.ACTION_CANCEL) pressed=null;
            else if (action==MotionEvent.ACTION_POINTER_DOWN) pressed=null;
            invalidate();
            return true;
        }
        scaler.onTouchEvent(e);
        gestures.onTouchEvent(e);
        return true;
    }

    private boolean buttonShown(RectF r) {
        AtlasModel.Area a=area();
        if (r==titleHit) return model!=null && !model.areas.isEmpty();
        if (r==btnRecenter) return !follow || preview;
        return a!=null && a.state==AtlasModel.READY && a.layers.length>1; // the floor buttons
    }
    private void press(RectF r) {
        if (r==btnRecenter) recenter();
        else if (r==btnUp) floor(1);
        else if (r==btnDown) floor(-1);
        else if (r==btnAuto) { manualFloor=-1; changed(); invalidate(); }
        else if (r==titleHit) { pickerOpen=true; pickerScroll=0; invalidate(); }
    }

    private float rowHeight() { return Math.max(52*dens,44*scale); }
    private boolean pickerTouch(MotionEvent e) {
        int action=e.getActionMasked();
        float y=e.getY();
        if (action==MotionEvent.ACTION_DOWN) { pickerDragging=false; pickerLastY=y; pickerDownY=y; return true; }
        if (action==MotionEvent.ACTION_MOVE) {
            if (Math.abs(y-pickerDownY)>8*dens) pickerDragging=true;
            if (pickerDragging) { pickerScroll-=y-pickerLastY; clampPicker(); invalidate(); }
            pickerLastY=y; return true;
        }
        if (action==MotionEvent.ACTION_UP) {
            if (!pickerDragging) {
                if (!pickerPanel.contains(e.getX(),e.getY())) pickerOpen=false;
                else {
                    float list=pickerPanel.top+rowHeight(); // the first row is the heading
                    int row=(int)Math.floor((y-list+pickerScroll)/rowHeight());
                    if (y>=list && model!=null && row>=0 && row<model.areas.size()) { pickerOpen=false; pickArea(row); }
                }
            }
            invalidate();
        }
        return true;
    }
    private void clampPicker() {
        float content=model==null ? 0 : model.areas.size()*rowHeight(), view=pickerPanel.height()-rowHeight();
        pickerScroll=Math.max(0,Math.min(Math.max(0,content-view),pickerScroll));
    }

    @Override public void computeScroll() {
        if (scroller.computeScrollOffset()) {
            int x=scroller.getCurrX(), y=scroller.getCurrY();
            cx-=(x-flingX)/zoom; cz+=(y-flingY)/zoom; flingX=x; flingY=y;
            clampCenter();
            postInvalidateOnAnimation();
        }
    }

    // ---- drawing ----

    private static int tint(float y,float hmin,float hmax,boolean shading,int alpha) {
        float t=shading ? Math.max(0,Math.min(1,(y-hmin)/Math.max(.01f,hmax-hmin))) : .72f;
        int shade=(int)(118+120*t);
        return Color.argb(alpha,shade+4,shade+2,shade);
    }

    private int[] colorsFor(AtlasModel.Layer l,int slot,int alpha,float hmin,float hmax,byte[] mask) {
        float[] key=l.colorKey[slot];
        if (l.colors[slot]!=null && key[0]==hmin && key[1]==hmax && l.colorMask[slot]==mask) return l.colors[slot];
        int[] c=l.colors[slot];
        if (c==null) c=l.colors[slot]=new int[l.triCount*3];
        for (int i=0;i<l.triCount;++i) {
            boolean shown=mask==null || mask[i]!=0;
            for (int k=0;k<3;++k) c[i*3+k]=shown ? tint(l.hasVertexHeights ? l.triY[i*3+k] : l.anchor,hmin,hmax,true,alpha) : 0;
        }
        key[0]=hmin; key[1]=hmax; l.colorMask[slot]=mask;
        return c;
    }
    private void ensureTexs(AtlasModel.Layer l,AtlasModel.Area a) {
        if (l.texs!=null || parchment==null) return;
        float ox=(a.bounds[0]+a.bounds[2])*.5f, oz=(a.bounds[1]+a.bounds[3])*.5f;
        float span=Math.max(a.bounds[2]-a.bounds[0],a.bounds[3]-a.bounds[1]); if (span<=0) span=512;
        float pw=parchment.getWidth(), ph=parchment.getHeight();
        float[] t=new float[l.triCount*6];
        for (int i=0;i<l.triCount;++i) for (int k=0;k<3;++k) {
            t[i*6+k*2]=Math.max(0,Math.min(1,.5f+(l.tri[i*6+k*2]-ox)/span))*pw;
            t[i*6+k*2+1]=Math.max(0,Math.min(1,.5f+(l.tri[i*6+k*2+1]-oz)/span))*ph;
        }
        l.texs=t;
    }

    @Override protected void onDraw(Canvas c) {
        int w=getWidth(), h=getHeight();
        if (w==0 || h==0) return;
        targets=0;
        if (mini) { drawMini(c,w,h); return; }
        if (background!=null) { dst.set(0,0,w,h); c.drawBitmap(background,null,dst,bgPaint); } else c.drawColor(Color.rgb(24,22,19));
        // frame and header rule
        strokePaint.setColor(Color.argb(95,156,148,126)); strokePaint.setStrokeWidth(Math.max(1,.8f*scale));
        c.drawRect(10*scale,8*scale,w-10*scale,h-8*scale,strokePaint);

        AtlasModel.Area a=area();
        boolean available=mapAvailable();
        if (available) drawMap(c,a);
        if (backgroundShader!=null && fadeVerts!=null) c.drawVertices(Canvas.VertexMode.TRIANGLES,fadeVerts.length,fadeVerts,0,fadeTexs,0,fadeColors,0,null,0,0,fadePaint);
        drawHeader(c,a,available);
        if (!available) drawMessage(c,a);
        drawControls(c,a);
        if (a!=null && available && cardTitle!=null) drawCard(c);
        if (pickerOpen) drawPicker(c);
    }

    /** The mini map: background, the map around the player, a title line, a thin border (the rounded clip is the outline). */
    private void drawMini(Canvas c,int w,int h) {
        if (background!=null) { dst.set(0,0,w,h); c.drawBitmap(background,null,dst,bgPaint); } else c.drawColor(Color.rgb(24,22,19));
        AtlasModel.Area a=area();
        boolean available=mapAvailable();
        if (available) drawMap(c,a);
        else {
            String msg;
            if (modelError!=null || (a!=null && a.state==AtlasModel.FAILED)) msg="Map unavailable";
            else if (model==null || !lvalid || (a!=null && a.state!=AtlasModel.READY)) msg="Loading the map…";
            else msg="No map here";
            float size=fit(msg,bodyFace,12*dens,8*dens,w*.9f);
            text(c,msg,ccx,ccy,size,MUTED,bodyFace,Paint.Align.CENTER);
        }
        String t=available && !title.isEmpty() ? title : a!=null ? a.name : "";
        if (!t.isEmpty()) {
            float strip=18*dens, size=fit(t,titleFace,11.5f*dens,8*dens,w-10*dens);
            fillPaint.setColor(Color.argb(150,12,12,12)); c.drawRect(0,0,w,strip,fillPaint);
            text(c,t,w*.5f,strip*.5f+size*.34f,size,PAPER,titleFace,Paint.Align.CENTER);
        }
        float bw=Math.max(1,1.2f*dens), r=miniRadius(getContext());
        strokePaint.setColor(Color.argb(210,198,173,118)); strokePaint.setStrokeWidth(bw);
        tmpRect.set(bw/2,bw/2,w-bw/2,h-bw/2); c.drawRoundRect(tmpRect,r,r,strokePaint);
    }

    private void drawMap(Canvas c,AtlasModel.Area a) {
        int active=activeLayer(a);
        boolean fog=optFog && model.features.present;
        float halfW=canvasRect.width()*.5f/zoom, halfH=canvasRect.height()*.5f/zoom;
        float vx0=cx-halfW, vx1=cx+halfW, vz0=cz-halfH, vz1=cz+halfH;
        float hmin, hmax;
        if (active>=0) {
            AtlasModel.Layer act=a.layers[active]; hmin=hmax=act.anchor;
            for (int li=0;li<a.layers.length;++li) if (li==active || Math.abs(a.layers[li].anchor-act.anchor)<=6) {
                AtlasModel.Layer l=a.layers[li];
                hmin=Math.min(hmin,l.hasVertexHeights ? l.minY : l.anchor); hmax=Math.max(hmax,l.hasVertexHeights ? l.maxY : l.anchor);
            }
        } else { hmin=hmax=ly-3; }

        c.save();
        c.clipRect(canvasRect);
        c.save();
        c.translate(ccx,ccy); c.scale(zoom,-zoom); c.translate(-cx,-cz);
        // other floors as faint context, then the floor itself
        for (int li=0;!mini && active>=0 && li<a.layers.length;++li) { // the mini map draws the floor itself only
            if (li==active) continue;
            AtlasModel.Layer l=a.layers[li];
            boolean nearby=Math.abs(l.anchor-a.layers[active].anchor)<=6;
            drawLayer(c,a,l,nearby ? 1 : 2,nearby ? 38 : 27,hmin,hmax,fog,vx0,vz0,vx1,vz1,false,nearby ? Color.argb(62,213,203,177) : Color.argb(49,213,203,177));
        }
        if (active>=0) drawLayer(c,a,a.layers[active],0,255,hmin,hmax,fog,vx0,vz0,vx1,vz1,true,Color.argb(220,204,193,163));
        c.restore();

        // markers, in screen space
        float marker=active>=0 ? a.layers[active].span : 6;
        float anchor=active>=0 ? a.layers[active].anchor : ly;
        boolean haveAnchor=active>=0;
        AtlasFeatures f=model.features; AtlasFeatures.Area fa=f.areas.get(a.id);
        List<AtlasFeatures.Lamp> lamps=f.present ? (fa!=null ? fa.lamps : null) : a.mapLamps;
        float margin=22*ms;
        if (lamps!=null) for (AtlasFeatures.Lamp d:lamps) {
            if (!d.discovered) continue; // not lit yet: not shown
            float px=sx(d.x), py=sy(d.z);
            if (!canvasRect.contains(px,py)) continue;
            icon(c,0,px,py,32*ms,0,d.available ? null : dimLamp);
            target(px,py,d.x,d.z,d.name,d.available ? "Lamp · lit" : "Lamp · locked or unavailable");
        }
        for (AtlasModel.Marker m:a.markers) {
            float px=sx(m.x), py=sy(m.z);
            if (px<canvasRect.left-margin || py<canvasRect.top-margin || px>canvasRect.right+margin || py>canvasRect.bottom+margin) continue;
            if (haveAnchor && Math.abs(m.y-anchor)>Math.max(6,marker*1.4f)) continue;
            if (!f.revealed(a.id,m.x,m.y,m.z,fog,state)) continue;
            if (m.kind.equals("npc") && !f.npcVisible(a.id,m.entity,optNpc,state)) continue;
            int idx=m.kind.equals("ladder") ? 1 : m.kind.equals("gate") ? 3 : 2;
            icon(c,idx,px,py,32*ms,0,null);
            target(px,py,m.x,m.z,m.name.isEmpty() ? m.kind : m.name,m.kind.equals("npc") ? "Conversation" : m.kind.equals("gate") ? "Gate · may need unlocking" : "Ladder");
        }
        if (optPickups) for (AtlasFeatures.PickupView p:pickups) {
            if (haveAnchor && Math.abs(p.y-anchor)>Math.max(3,a.layers[active].span)) continue;
            float px=sx(p.x), py=sy(p.z);
            if (!canvasRect.contains(px,py)) continue;
            int color=p.category.equals("healing") ? Color.rgb(230,81,76) : p.category.equals("insight") ? Color.rgb(189,121,234)
                : p.category.equals("equipment") ? Color.rgb(93,172,242) : Color.rgb(235,193,79);
            fillPaint.setColor(Color.rgb(20,22,24)); c.drawCircle(px,py,4.1f*ms,fillPaint);
            fillPaint.setColor(color); c.drawCircle(px,py,3f*ms,fillPaint);
            target(px,py,p.x,p.z,categoryName(p.category),p.label);
        }
        if (lvalid && current==selected) {
            float px=sx(lx), py=sy(lz);
            float dx=-(float)Math.sin(lyaw), dy=(float)Math.cos(lyaw);   // FacingOnMap (bbport_map_direction.h)
            float angle=(float)Math.atan2(dx,-dy);
            fillPaint.setColor(Color.argb(27,96,139,138)); c.drawCircle(px,py,18.5f*ms,fillPaint);
            fillPaint.setColor(Color.argb(210,20,29,29)); c.drawCircle(px,py,16f*ms,fillPaint);
            strokePaint.setColor(Color.argb(120,125,160,155)); strokePaint.setStrokeWidth(Math.max(1,.8f*ms)); c.drawCircle(px,py,17f*ms,strokePaint);
            icon(c,4,px,py,35*ms,(float)Math.toDegrees(angle),playerTint);
        }
        c.restore();
    }

    private void drawLayer(Canvas c,AtlasModel.Area a,AtlasModel.Layer l,int slot,int alpha,float hmin,float hmax,boolean fog,
                           float vx0,float vz0,float vx1,float vz1,boolean active,int outlineColor) {
        AtlasModel.Cache cache=l.cache;
        if (fog && (cache==null || cache.key==null || !cache.key.toString().startsWith("fog"))) return; // concealed until the fog is computed
        byte[] mask=fog && cache!=null ? cache.mask : null;
        if (l.triCount>0) {
            int[] cols=colorsFor(l,slot,alpha,hmin,hmax,mask);
            ensureTexs(l,a);
            for (int k=0;k*AtlasModel.CHUNK_TRIANGLES<l.triCount;++k) {
                float[] b=l.chunkBox;
                if (b[k*4+2]<vx0 || b[k*4]>vx1 || b[k*4+3]<vz0 || b[k*4+1]>vz1) continue;
                int first=k*AtlasModel.CHUNK_TRIANGLES, n=Math.min(AtlasModel.CHUNK_TRIANGLES,l.triCount-first);
                c.drawVertices(Canvas.VertexMode.TRIANGLES,n*6,l.tri,first*6,l.texs,l.texs!=null ? first*6 : 0,cols,first*3,null,0,0,floorPaint);
            }
        }
        for (int r=0;r<l.rects.length/4;++r) {
            float x=l.rects[r*4], z=l.rects[r*4+1], rw=l.rects[r*4+2], rh=l.rects[r*4+3];
            if (x+rw<vx0 || x>vx1 || z+rh<vz0 || z>vz1) continue;
            fillPaint.setColor(tint(l.anchor,hmin,hmax,true,alpha)); c.drawRect(x,z,x+rw,z+rh,fillPaint);
        }
        if (cache==null) return;
        strokePaint.setColor(outlineColor); strokePaint.setStrokeWidth((active ? .85f : .7f)*ls/zoom);
        drawChunks(c,cache.outline,vx0,vz0,vx1,vz1);
        if (active) {
            strokePaint.setColor(Color.rgb(15,18,19));
            strokePaint.setStrokeWidth(Math.max(2.5f*ls,Math.min(5*ls,zoom*.13f))/zoom);
            drawChunks(c,cache.walls,vx0,vz0,vx1,vz1);
        }
    }
    private void drawChunks(Canvas c,AtlasModel.Chunks ch,float vx0,float vz0,float vx1,float vz1) {
        if (ch==null) return;
        for (int i=0;i<ch.paths.length;++i) {
            float[] b=ch.box;
            if (b[i*4+2]<vx0 || b[i*4]>vx1 || b[i*4+3]<vz0 || b[i*4+1]>vz1) continue;
            c.drawPath(ch.paths[i],strokePaint);
        }
    }

    private void icon(Canvas c,int index,float px,float py,float size,float degrees,PorterDuffColorFilter filter) {
        if (icons==null) return;
        int col=index%4, row=index/4;
        src.set(col*384+3,row*384+3,(col+1)*384-3,(row+1)*384-3);
        dst.set(px-size/2,py-size/2,px+size/2,py+size/2);
        iconPaint.setColorFilter(filter);
        if (degrees!=0) { c.save(); c.rotate(degrees,px,py); c.drawBitmap(icons,src,dst,iconPaint); c.restore(); }
        else c.drawBitmap(icons,src,dst,iconPaint);
    }
    private void target(float px,float py,float wx,float wz,String title,String sub) {
        if (mini || targets>=MAX_TARGETS) return;
        tx[targets]=px; ty[targets]=py; twx[targets]=wx; twz[targets]=wz; tTitle[targets]=title; tSub[targets]=sub; ++targets;
    }
    private static String categoryName(String c) {
        return c.equals("healing") ? "Blood vials" : c.equals("insight") ? "Insight" : c.equals("equipment") ? "Equipment" : "Coldblood";
    }

    private void text(Canvas c,String s,float x,float y,float size,int color,Typeface face,Paint.Align align) {
        textPaint.setTypeface(face); textPaint.setTextSize(size); textPaint.setColor(color); textPaint.setTextAlign(align);
        c.drawText(s,x,y,textPaint);
    }
    /** The text at its size, shrunk (to at least min) to fit the width. */
    private float fit(String s,Typeface face,float size,float min,float width) {
        textPaint.setTypeface(face); textPaint.setTextSize(size);
        float w=textPaint.measureText(s);
        return w>width ? Math.max(min,size*width/w) : size;
    }
    private void ornament(Canvas c,float x,float y,float r,int color) {
        Path p=scratch; p.reset();
        p.moveTo(x-r,y); p.lineTo(x,y-r*.55f); p.lineTo(x+r,y); p.lineTo(x,y+r*.55f); p.close();
        strokePaint.setColor(color); strokePaint.setStrokeWidth(Math.max(1,.7f*scale)); c.drawPath(p,strokePaint);
    }

    private void drawHeader(Canvas c,AtlasModel.Area a,boolean available) {
        int w=getWidth();
        String t, sub="";
        if (!haveState) { t="Waiting for the game…"; }
        else if (available) { t=preview ? a.name : title; sub=preview ? "Browsing · the player is elsewhere" : subtitle; if (preview && current==selected) sub=""; }
        else if (a!=null && a.state!=AtlasModel.READY && (preview || (lvalid && current==selected))) t=a.name;
        else t=lvalid ? "Location unavailable" : "Awaiting your location";
        float titleSize=fit(t,titleFace,51*scale*1.05f,27*scale,780*scale);
        float baseline=14*scale+titleSize*.86f;
        text(c,t,w*.5f,baseline,titleSize,PAPER,titleFace,Paint.Align.CENTER);
        // a small chevron: the title opens the list of areas
        if (model!=null && !model.areas.isEmpty()) {
            textPaint.setTypeface(titleFace); textPaint.setTextSize(titleSize);
            float half=textPaint.measureText(t)*.5f, cxp=w*.5f+half+16*scale, cyp=baseline-titleSize*.3f, r=7*scale;
            Path p=scratch; p.reset(); p.moveTo(cxp-r,cyp-r*.5f); p.lineTo(cxp+r,cyp-r*.5f); p.lineTo(cxp,cyp+r*.7f); p.close();
            fillPaint.setColor(RULE); c.drawPath(p,fillPaint);
        }
        float ruleY=Math.max(67*scale,baseline+titleSize*.22f);
        strokePaint.setColor(RULE); strokePaint.setStrokeWidth(Math.max(1,.8f*scale));
        c.drawLine(w*.25f,ruleY,w*.75f,ruleY,strokePaint);
        for (float f:new float[]{.2625f,.5f,.7375f}) ornament(c,w*f,ruleY,f==.5f ? 5*scale : 3*scale,RULE);
        if (!sub.isEmpty()) {
            float s=fit(sub,titleFace,29*scale*1.05f,18*scale,760*scale);
            text(c,sub,w*.5f,ruleY+10*scale+s*.8f,s,MUTED,titleFace,Paint.Align.CENTER);
        }
        icon(c,5,66*scale,60*scale,73*scale,0,compassTint);
        // credit and the pickup key in the footer
        float fy=getHeight()-footerH*.5f;
        if (available && optPickups) {
            String[][] keys={{"Blood vials","healing"},{"Insight","insight"},{"Equipment","equipment"},{"Coldblood","echoes"}};
            float size=13*scale*1.25f; textPaint.setTypeface(bodyFace); textPaint.setTextSize(size);
            float total=0; for (String[] k:keys) total+=textPaint.measureText(k[0])+34*scale;
            float x=w*.5f-total*.5f;
            for (String[] k:keys) {
                int color=k[1].equals("healing") ? Color.rgb(230,81,76) : k[1].equals("insight") ? Color.rgb(189,121,234)
                    : k[1].equals("equipment") ? Color.rgb(93,172,242) : Color.rgb(235,193,79);
                fillPaint.setColor(color); c.drawCircle(x+6*scale,fy,4f*ms,fillPaint);
                text(c,k[0],x+16*scale,fy+size*.35f,size,PAPER,bodyFace,Paint.Align.LEFT);
                x+=textPaint.measureText(k[0])+34*scale;
            }
        }
    }

    private void drawMessage(Canvas c,AtlasModel.Area a) {
        String msg;
        if (modelError!=null) msg="The map data could not be read: "+modelError;
        else if (model==null) msg="Loading the map…";
        else if (!haveState) msg="Waiting for the game…";
        else if (a!=null && a.state==AtlasModel.FAILED) msg="This area's map could not be read: "+a.error;
        else if (a!=null && a.state!=AtlasModel.READY) msg="Loading the map…";
        else if (!lvalid) msg="Position unavailable — loading, or outside a supported area";
        else msg="Map data is not available for this location";
        if (notice!=null && !haveState) msg=notice;
        float size=fit(msg,bodyFace,20*scale*1.25f,12*scale,canvasRect.width()*.9f);
        text(c,msg,ccx,ccy,size,MUTED,bodyFace,Paint.Align.CENTER);
    }

    private void button(Canvas c,RectF r,boolean on,boolean focus) {
        fillPaint.setColor(focus ? Color.argb(235,103,91,63) : Color.argb(215,27,29,27));
        float round=6*dens; c.drawRoundRect(r,round,round,fillPaint);
        strokePaint.setColor(on ? GOLD : Color.argb(120,152,146,128)); strokePaint.setStrokeWidth(Math.max(1,1.2f*dens*.7f));
        c.drawRoundRect(r,round,round,strokePaint);
    }
    private void drawControls(Canvas c,AtlasModel.Area a) {
        float size=15*dens;
        if (buttonShown(btnUp)) {
            int active=activeLayer(a), pos=0, total=a.floorOrder.length;
            for (int i=0;i<total;++i) if (a.floorOrder[i]==active) pos=i;
            boolean auto=manualFloor<0;
            button(c,btnUp,pos<total-1,pressed==btnUp); button(c,btnDown,pos>0,pressed==btnDown); button(c,btnAuto,auto,pressed==btnAuto);
            drawArrow(c,btnUp,true,pos<total-1); drawArrow(c,btnDown,false,pos>0);
            text(c,(pos+1)+"/"+total,btnAuto.centerX(),btnAuto.centerY()-size*.15f,size*1.05f,PAPER,bodyFace,Paint.Align.CENTER);
            text(c,auto ? "AUTO" : "auto",btnAuto.centerX(),btnAuto.centerY()+size*1.0f,size*.62f,auto ? GOLD : MUTED,bodyFace,Paint.Align.CENTER);
        }
        if (buttonShown(btnRecenter)) {
            button(c,btnRecenter,true,pressed==btnRecenter);
            text(c,"Recenter",btnRecenter.centerX(),btnRecenter.centerY()+size*.36f,size,PAPER,bodyFace,Paint.Align.CENTER);
        }
    }
    private void drawArrow(Canvas c,RectF r,boolean up,boolean on) {
        float cxp=r.centerX(), cyp=r.centerY(), h=r.width()*.16f, wd=r.width()*.24f;
        Path p=scratch; p.reset();
        if (up) { p.moveTo(cxp-wd,cyp+h); p.lineTo(cxp+wd,cyp+h); p.lineTo(cxp,cyp-h); }
        else { p.moveTo(cxp-wd,cyp-h); p.lineTo(cxp+wd,cyp-h); p.lineTo(cxp,cyp+h); }
        p.close();
        fillPaint.setColor(on ? PAPER : Color.argb(90,223,214,187)); c.drawPath(p,fillPaint);
    }

    private void drawCard(Canvas c) {
        if (SystemClock.uptimeMillis()>cardUntil) { cardTitle=null; return; }
        float px=sx(cardX), py=sy(cardZ);
        if (!canvasRect.contains(px,py)) return;
        float ts=15*dens, ss=12*dens, pad=9*dens;
        String[] lines=cardSub==null ? new String[0] : cardSub.split("\n");
        int shown=Math.min(lines.length,5);
        textPaint.setTypeface(bodyFace);
        textPaint.setTextSize(ts); float wid=textPaint.measureText(cardTitle);
        textPaint.setTextSize(ss);
        for (int i=0;i<shown;++i) wid=Math.max(wid,textPaint.measureText(lines[i]));
        float w=Math.min(canvasRect.width()*.8f,wid+2*pad), h=pad*2+ts*1.2f+shown*ss*1.3f;
        float left=Math.max(canvasRect.left+4*dens,Math.min(canvasRect.right-w-4*dens,px-w/2));
        float top=py-24*ms-h; if (top<canvasRect.top+4*dens) top=py+24*ms;
        tmpRect.set(left,top,left+w,top+h);
        fillPaint.setColor(Color.argb(235,27,29,27)); c.drawRoundRect(tmpRect,6*dens,6*dens,fillPaint);
        strokePaint.setColor(GOLD); strokePaint.setStrokeWidth(Math.max(1,dens*.8f)); c.drawRoundRect(tmpRect,6*dens,6*dens,strokePaint);
        text(c,cardTitle,left+pad,top+pad+ts,ts,PAPER,bodyFace,Paint.Align.LEFT);
        for (int i=0;i<shown;++i) text(c,lines[i],left+pad,top+pad+ts*1.2f+(i+1)*ss*1.3f-ss*.2f,ss,MUTED,bodyFace,Paint.Align.LEFT);
    }

    private void drawPicker(Canvas c) {
        int w=getWidth(), h=getHeight();
        fillPaint.setColor(Color.argb(170,0,0,0)); c.drawRect(0,0,w,h,fillPaint);
        float rh=rowHeight();
        float pw=Math.min(w*.84f,520*dens);
        float top=8*dens, bottom=h-8*dens;
        pickerPanel.set((w-pw)/2,top,(w+pw)/2,bottom);
        float contentH=(model==null ? 0 : model.areas.size())*rh+rh;
        if (contentH+16*dens<pickerPanel.height()) pickerPanel.bottom=pickerPanel.top+contentH;
        clampPicker();
        fillPaint.setColor(Color.argb(245,27,29,27)); c.drawRoundRect(pickerPanel,8*dens,8*dens,fillPaint);
        strokePaint.setColor(GOLD); strokePaint.setStrokeWidth(Math.max(1,dens)); c.drawRoundRect(pickerPanel,8*dens,8*dens,strokePaint);
        float size=17*dens;
        text(c,"Choose an area",pickerPanel.centerX(),pickerPanel.top+rh*.5f+size*.35f,size,GOLD,bodyFace,Paint.Align.CENTER);
        c.save();
        c.clipRect(pickerPanel.left,pickerPanel.top+rh,pickerPanel.right,pickerPanel.bottom);
        if (model!=null) for (int i=0;i<model.areas.size();++i) {
            float y=pickerPanel.top+rh+i*rh-pickerScroll;
            if (y+rh<pickerPanel.top+rh || y>pickerPanel.bottom) continue;
            if (i==selected) { fillPaint.setColor(Color.argb(70,198,173,118)); c.drawRect(pickerPanel.left,y,pickerPanel.right,y+rh,fillPaint); }
            String name=model.areas.get(i).name+(i==current ? "  • you are here" : "");
            text(c,name,pickerPanel.left+18*dens,y+rh*.5f+size*.35f,size,i==selected ? PAPER : MUTED,bodyFace,Paint.Align.LEFT);
            strokePaint.setColor(Color.argb(50,152,146,128)); strokePaint.setStrokeWidth(1);
            c.drawLine(pickerPanel.left+12*dens,y+rh,pickerPanel.right-12*dens,y+rh,strokePaint);
        }
        c.restore();
    }
}
