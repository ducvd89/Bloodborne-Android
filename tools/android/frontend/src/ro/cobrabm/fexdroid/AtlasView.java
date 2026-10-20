package ro.cobrabm.fexdroid;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Outline;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffColorFilter;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Build;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.view.GestureDetector;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.WindowInsets;
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
 *  Route finding (pack version 2, AtlasNav): a tap on a lamp, pickup or conversation marker opens its card with a Navigate
 *  button; a tap on an empty spot offers "Set destination here" (snapped to the nearest walkable spot of the floor shown,
 *  within 6 m); a chip at the top shows the destination, the remaining distance and a cross to clear it. The route is drawn
 *  as footprints like the Marauder's Map (alternating left and right shoe prints one stride apart, a walker that prints them
 *  one after another from the player toward the destination while the older prints fade, the rest of the route as faint
 *  prints, parts on other floors fainter) with marks where it climbs, rides a lift, drops or needs a lever; doors are small
 *  bars (green open, red closed, amber crossed from one side only, a key for key doors), a tap on one opens a card that
 *  says whether it can be opened from where the player is. The mini map draws the route too and an arrow at its edge toward
 *  the part of the route that is outside it.
 *
 *  A view made with setCloseHandler (the full-screen map over the game) is the same map with a Close button at the top
 *  right and its controls kept inside the display cutout's safe area.
 *
 *  Mini mode (the floating map over the game): a small square that always follows the player (area and floor chosen
 *  by the game's state, one fixed zoom), with a title line at the top, the player, lamps, pickups and markers, a
 *  rounded clip and a thin border. It has no buttons or pickers and no navigation: it is not clickable, and the game
 *  gets the touches under it; MainActivity.dispatchTouchEvent sees a touch that lands in it and runs the 2 s hold that opens
 *  the full map (a ring fills on it after 300 ms, see holdBegin). It keeps
 *  its last frame while the game loads, and draws only what falls inside its own small viewport. */
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
    private String banner;

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
    private final float[] tx=new float[MAX_TARGETS], ty=new float[MAX_TARGETS], twx=new float[MAX_TARGETS], twy=new float[MAX_TARGETS], twz=new float[MAX_TARGETS];
    private final String[] tTitle=new String[MAX_TARGETS], tSub=new String[MAX_TARGETS];
    private final int[] tKind=new int[MAX_TARGETS]; // T_LABEL, T_NAV (can be navigated to), T_GATE (tGate = gate index)
    private final int[] tGate=new int[MAX_TARGETS];
    private static final int T_LABEL=0, T_NAV=1, T_GATE=2;
    private int targets;
    private String cardTitle, cardSub; private float cardX, cardZ; private long cardUntil;
    // the card's Navigate button: where it leads (a marker, or the snapped point of a tap), shown while cardNav
    private boolean cardNav; private float cardNavX, cardNavY, cardNavZ; private String cardName=""; private boolean cardMarked; private String cardArea="";
    private final RectF cardRect=new RectF(), cardBtn=new RectF(), chipRect=new RectF(), chipClear=new RectF();
    private int cardSeq;

    // ---- route finding ----
    private AtlasNav nav;
    private NavData.Route pathRoute; private int pathLayer=-2; private float pathLo, pathHi;
    // footprints: the prints are placed by arc length along the route polyline for the zoom bucket shown (made when the route,
    // the floor shown or the zoom bucket changes); the exact polyline is drawn under them as a dotted line
    private static final float STRIDE=.75f;          // metres between two prints (at least; more when zoomed out, see FOOT_GAP)
    private static final float FOOT_RATE=2.5f;       // steps per second of the walker
    private static final float FOOT_TRAIL=16f;       // prints that stay bright behind the walker
    private static final float FOOT_BASE=.5f, FOOT_STATIC=.75f, FOOT_OFF=.55f; // the prints not being walked, those of a paused map, of other floors (times)
    private static final float FOOT_LEN_MINI=9f, FOOT_LEN_FULL=12f;    // dp: the print's length on screen (medium size)
    private static final float FOOT_UP=1.4f;         // the most the prints grow when zoomed in (the bitmaps are drawn at that size)
    private static final float FOOT_GAP=1.6f;        // the least on-screen distance between two prints, in print lengths
    private static final float FOOT_LAT=.18f;        // a print's offset left / right of the path, in print lengths
    private static final float ZOOM_BUCKET=1.15f;    // factor between the zoom buckets the placement is made for
    private static final float FOOT_SHAPE=.96f;      // the print shape's length in its own units (see makeFoot)
    private static final int FOOT_INK=Color.rgb(43,29,16), FOOT_HALO=Color.argb(217,243,230,200);
    private float footMul=1f;                        // Footprint size: small .75, medium 1, large 1.35
    private int footKeyLen, footKeyHalo;             // what the cached bitmaps were drawn for (px)
    private float stepStride;                        // the world stride the placement was made for
    private float[] stepX=new float[256], stepZ=new float[256], stepAng=new float[256], stepSin=new float[256], stepCos=new float[256], stepCum=new float[256];
    private byte[] stepOn=new byte[256]; private int stepCount;
    private float[] dashOn=new float[256], dashOff=new float[256]; private int dashOnN, dashOffN; // the dotted path line's dashes (x0,y0,x1,y1)
    private final Paint dotPaint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private Bitmap footL, footR; private final Matrix footMatrix=new Matrix();
    private final Paint footPaint=new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private boolean footAnimate=true; private long animDelay;
    private final Runnable animRun=new Runnable() { @Override public void run() { invalidate(); } };
    private static final long MINI_ANIM_MS=83, ANIM_MS=33;
    // the hold that opens the full map (mini mode): MainActivity detects it, this view only draws the ring
    private static final long HOLD_MS=2000, RING_AFTER_MS=300;
    private boolean ringOn; private long ringStart;
    // the full-screen map's Close button and the display cutout's safe insets
    private Runnable closeHandler; private final RectF btnClose=new RectF(); private int insL, insT, insR, insB;

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
    void setNav(AtlasNav nav) { this.nav=nav; }
    /** The Close button (a full-screen map over the game): null for the second screen's and the mini map. */
    void setCloseHandler(Runnable r) { closeHandler=r; if (getWidth()>0) { relayout(getWidth(),getHeight()); } invalidate(); }
    /** Footprint size (0 small, 1 medium, 2 large): the prints' size on screen, applied at once. */
    void setPrintSize(int size) {
        float m=size<=0 ? .75f : size>=2 ? 1.35f : 1f;
        if (m!=footMul) { footMul=m; invalidate(); }
    }
    /** The footprints' walker moves (it stands still, with the whole route as plain prints, when off: the game is paused). */
    void setFootAnimation(boolean on) { if (footAnimate!=on) { footAnimate=on; invalidate(); } }
    /** The hold on the mini map has begun (MainActivity saw the touch land in its rect): the ring fills from 300 ms. */
    void holdBegin() { ringOn=true; ringStart=SystemClock.uptimeMillis(); invalidate(); }
    /** The hold ended (finished, moved or lifted). */
    void holdEnd() { ringOn=false; invalidate(); }
    /** Follow the player again at the default zoom (the full map when it opens). */
    void resetView() { pickerOpen=false; cardTitle=null; cardNav=false; recenter(); }
    private boolean navOn() { return nav!=null && nav.available(); }
    boolean isMini() { return mini; }
    boolean artSet() { return artSet; }
    /** A small line at the bottom of the second-screen map (the map pack's update; null: none). */
    void setBanner(String text) {
        if (mini) return;
        if (text==null ? banner==null : text.equals(banner)) return;
        banner=text; invalidate();
    }
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
    @Override protected void onAttachedToWindow() { super.onAttachedToWindow(); readInsets(); }
    @Override protected void onDetachedFromWindow() { removeCallbacks(animRun); ringOn=false; super.onDetachedFromWindow(); }
    @Override public WindowInsets onApplyWindowInsets(WindowInsets insets) { readInsets(insets); return super.onApplyWindowInsets(insets); }
    private void readInsets() { readInsets(getRootWindowInsets()); }
    /** The display cutout's safe insets (only a view with a Close button, that is on the game's own screen, keeps off them). */
    private void readInsets(WindowInsets wi) {
        if (closeHandler==null || mini) return;
        int l=0, t=0, r=0, b=0;
        if (wi!=null && Build.VERSION.SDK_INT>=28 && wi.getDisplayCutout()!=null) {
            android.view.DisplayCutout dc=wi.getDisplayCutout();
            l=dc.getSafeInsetLeft(); t=dc.getSafeInsetTop(); r=dc.getSafeInsetRight(); b=dc.getSafeInsetBottom();
        }
        if (l==insL && t==insT && r==insR && b==insB) return;
        insL=l; insT=t; insR=r; insB=b;
        if (getWidth()>0 && getHeight()>0) { relayout(getWidth(),getHeight()); invalidate(); }
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
        headerH=Math.max(114*scale,56*dens); footerH=Math.max(Math.max(46*scale,28*dens),insB);
        float side=Math.max(18*scale,6*dens);
        if (closeHandler!=null) { // the full-screen map: Close at the top right, inside the safe area
            float bh=Math.max(48*dens,34*scale), bw=Math.max(116*dens,100*scale);
            btnClose.set(w-insR-10*dens-bw,insT+8*dens,w-insR-10*dens,insT+8*dens+bh);
            headerH=Math.max(headerH,btnClose.bottom+6*dens);
        } else btnClose.setEmpty();
        canvasRect.set(Math.max(side,insL),headerH,w-Math.max(side,insR),h-footerH);
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

    /** A single confirmed tap (a short touch with no movement; a double tap zooms instead): the card's button, a marker, a
     *  gate, or an empty spot of the map (Set destination here). */
    private void tap(float x,float y) {
        if (!canvasRect.contains(x,y)) return;
        boolean cardUp=cardTitle!=null && SystemClock.uptimeMillis()<=cardUntil;
        if (cardUp) {
            if (cardNav && !cardBtn.isEmpty() && cardBtn.contains(x,y)) { navigateCard(); return; }
            if (cardRect.contains(x,y)) return;                  // a tap on the card itself keeps it
        }
        if (chipRect.contains(x,y)) return;
        float radius=40*dens, best=radius*radius; int hit=-1;
        for (int i=0;i<targets;++i) { float dx=tx[i]-x, dy=ty[i]-y, d=dx*dx+dy*dy; if (d<=best) { best=d; hit=i; } }
        if (hit<0) {
            if (navOn() && mapAvailable()) { pointTap(x,y); return; }
            cardTitle=null; cardNav=false; invalidate(); return;
        }
        ++cardSeq;
        cardTitle=tTitle[hit]; cardSub=tSub[hit]; cardX=twx[hit]; cardZ=twz[hit]; cardUntil=SystemClock.uptimeMillis()+8000;
        AtlasModel.Area a=area();
        if (tKind[hit]==T_GATE && nav!=null && a!=null) {
            NavGate[] gates=nav.gates(a.id);
            if (gates!=null && tGate[hit]>=0 && tGate[hit]<gates.length) { String[] c=AtlasNav.gateCard(gates[tGate[hit]],state,lvalid && current==selected,lx,ly,lz); cardTitle=c[0]; cardSub=c[1]; }
        }
        cardNav=tKind[hit]==T_NAV && navOn() && a!=null && a.navData;
        if (cardNav) { cardNavX=twx[hit]; cardNavY=twy[hit]; cardNavZ=twz[hit]; cardName=tTitle[hit]; cardMarked=false; cardArea=a.id; }
        postInvalidateDelayed(8100); invalidate();
    }

    /** A tap on an empty spot: the nearest walkable spot of the floor shown within 6 m becomes a possible destination. */
    private void pointTap(float x,float y) {
        AtlasModel.Area a=area();
        if (a==null || !a.navData) { cardTitle=null; cardNav=false; invalidate(); return; }
        final float wx=cx+(x-ccx)/zoom, wz=cz-(y-ccy)/zoom;
        int active=activeLayer(a);
        float[] band=floorBand(a,active);
        final int seq=++cardSeq;
        cardTitle="Set destination here"; cardSub="Finding the floor…"; cardX=wx; cardZ=wz; cardNav=false; cardUntil=SystemClock.uptimeMillis()+8000;
        final String areaId=a.id;
        nav.snapPoint(areaId,wx,wz,band[0],band[1],(snap,problem) -> {
            if (seq!=cardSeq || cardTitle==null) return;
            if (snap!=null) {
                cardX=snap.x; cardZ=snap.z; cardNav=true; cardNavX=snap.x; cardNavY=snap.y; cardNavZ=snap.z; cardName="Marked point"; cardMarked=true; cardArea=areaId;
                float d=lvalid && current==selected ? (float)Math.sqrt((snap.x-lx)*(snap.x-lx)+(snap.y-ly)*(snap.y-ly)+(snap.z-lz)*(snap.z-lz)) : -1;
                cardSub=d>=0 ? String.format(java.util.Locale.ROOT,"%d m away (straight line)",Math.round(d)) : "";
            } else { cardNav=false; cardSub=problem!=null ? problem : "No walkable floor here"; }
            cardUntil=SystemClock.uptimeMillis()+8000;
            postInvalidateDelayed(8100); invalidate();
        });
        postInvalidateDelayed(8100); invalidate();
    }
    private void navigateCard() {
        if (nav!=null && cardNav) nav.setDest(cardArea,cardNavX,cardNavY,cardNavZ,cardName,cardMarked);
        cardTitle=null; cardNav=false; invalidate();
    }

    /** The heights [lo, hi] of a floor of the area (the player's when there is none): what counts as being on it. */
    private float[] floorBand(AtlasModel.Area a,int active) {
        if (active>=0 && active<a.layers.length) {
            AtlasModel.Layer l=a.layers[active];
            if (l.hasVertexHeights) return new float[]{l.minY-1f,l.maxY+1f};
            return new float[]{l.anchor-l.span*.5f-1f,l.anchor+l.span*.5f+1f};
        }
        return new float[]{ly-3f,ly+3f};
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
        if (mini) return false;
        int action=e.getActionMasked();
        if (pickerOpen) return pickerTouch(e);
        if (action==MotionEvent.ACTION_DOWN) {
            pressed=null;
            for (RectF r:new RectF[]{btnClose,chipClear,btnUp,btnDown,btnAuto,btnRecenter,titleHit}) {
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
        if (r==btnClose) return closeHandler!=null && !btnClose.isEmpty();
        if (r==titleHit) return model!=null && !model.areas.isEmpty();
        if (r==chipClear) return nav!=null && nav.hasDestination() && !chipClear.isEmpty();
        if (r==btnRecenter) return !follow || preview;
        return a!=null && a.state==AtlasModel.READY && a.layers.length>1; // the floor buttons
    }
    private void press(RectF r) {
        if (r==btnClose) { if (closeHandler!=null) closeHandler.run(); }
        else if (r==chipClear) { nav.clear(); chipClear.setEmpty(); }
        else if (r==btnRecenter) recenter();
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
        targets=0; animDelay=Long.MAX_VALUE;
        if (mini) drawMini(c,w,h); else drawFull(c,w,h);
        if (animDelay!=Long.MAX_VALUE) { removeCallbacks(animRun); postDelayed(animRun,animDelay); }
    }

    private void drawFull(Canvas c,int w,int h) {
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
        if (available) drawChip(c); else { chipRect.setEmpty(); chipClear.setEmpty(); }
        if (a!=null && available && cardTitle!=null) drawCard(c);
        if (pickerOpen) drawPicker(c);
        if (banner!=null) drawBanner(c,w,h);
    }

    /** The status line (map data update) over the bottom of the map, centered; it takes no touches. */
    private void drawBanner(Canvas c,int w,int h) {
        float size=14*dens, padX=12*dens, hh=size*2f;
        textPaint.setTypeface(bodyFace); textPaint.setTextSize(size);
        float tw=Math.min(textPaint.measureText(banner),w*.9f-2*padX), bw=tw+2*padX;
        float left=canvasRect.centerX()-bw*.5f, bottom=canvasRect.bottom-8*dens;
        tmpRect.set(left,bottom-hh,left+bw,bottom);
        fillPaint.setColor(Color.argb(228,27,29,27)); c.drawRoundRect(tmpRect,hh/2,hh/2,fillPaint);
        strokePaint.setColor(GOLD); strokePaint.setStrokeWidth(Math.max(1,dens*.9f)); c.drawRoundRect(tmpRect,hh/2,hh/2,strokePaint);
        text(c,banner,canvasRect.centerX(),tmpRect.centerY()+size*.35f,fit(banner,bodyFace,size,9*dens,tw),PAPER,bodyFace,Paint.Align.CENTER);
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
        drawMiniDest(c,w,h);
        if (ringOn) drawHoldRing(c,w,h);
        float bw=Math.max(1,1.2f*dens), r=miniRadius(getContext());
        strokePaint.setColor(Color.argb(210,198,173,118)); strokePaint.setStrokeWidth(bw);
        tmpRect.set(bw/2,bw/2,w-bw/2,h-bw/2); c.drawRoundRect(tmpRect,r,r,strokePaint);
    }

    /** The hold's progress: a ring that fills clockwise over the middle of the mini map, after 300 ms. */
    private void drawHoldRing(Canvas c,int w,int h) {
        animDelay=Math.min(animDelay,45);
        long el=SystemClock.uptimeMillis()-ringStart;
        if (el<RING_AFTER_MS) return;
        float frac=Math.min(1f,(el-RING_AFTER_MS)/(float)(HOLD_MS-RING_AFTER_MS));
        float r=Math.min(w,h)*.2f, sw=Math.max(3f,3.2f*dens);
        tmpRect.set(w*.5f-r,h*.5f-r,w*.5f+r,h*.5f+r);
        fillPaint.setColor(Color.argb(95,12,12,12)); c.drawCircle(w*.5f,h*.5f,r+sw,fillPaint);
        strokePaint.setStrokeCap(Paint.Cap.ROUND);
        strokePaint.setColor(Color.argb(110,223,214,187)); strokePaint.setStrokeWidth(sw); c.drawCircle(w*.5f,h*.5f,r,strokePaint);
        strokePaint.setColor(Color.rgb(255,226,140)); strokePaint.setStrokeWidth(sw*1.15f); c.drawArc(tmpRect,-90,360*frac,false,strokePaint);
        strokePaint.setStrokeCap(Paint.Cap.BUTT);
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
        drawGates(c,a,active);
        drawFootprints(c,a,active);

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
            target(px,py,d.x,d.y,d.z,d.name,d.available ? "Lamp · lit" : "Lamp · locked or unavailable",T_NAV,-1);
        }
        for (AtlasModel.Marker m:a.markers) {
            float px=sx(m.x), py=sy(m.z);
            if (px<canvasRect.left-margin || py<canvasRect.top-margin || px>canvasRect.right+margin || py>canvasRect.bottom+margin) continue;
            if (haveAnchor && Math.abs(m.y-anchor)>Math.max(6,marker*1.4f)) continue;
            if (!f.revealed(a.id,m.x,m.y,m.z,fog,state)) continue;
            if (m.kind.equals("npc") && !f.npcVisible(a.id,m.entity,optNpc,state)) continue;
            int idx=m.kind.equals("ladder") ? 1 : m.kind.equals("gate") ? 3 : 2;
            icon(c,idx,px,py,32*ms,0,null);
            target(px,py,m.x,m.y,m.z,m.name.isEmpty() ? m.kind : m.name,m.kind.equals("npc") ? "Conversation" : m.kind.equals("gate") ? "Gate · may need unlocking" : "Ladder",
                m.kind.equals("npc") ? T_NAV : T_LABEL,-1);
        }
        if (optPickups) for (AtlasFeatures.PickupView p:pickups) {
            if (haveAnchor && Math.abs(p.y-anchor)>Math.max(3,a.layers[active].span)) continue;
            float px=sx(p.x), py=sy(p.z);
            if (!canvasRect.contains(px,py)) continue;
            int color=p.category.equals("healing") ? Color.rgb(230,81,76) : p.category.equals("insight") ? Color.rgb(189,121,234)
                : p.category.equals("equipment") ? Color.rgb(93,172,242) : Color.rgb(235,193,79);
            fillPaint.setColor(Color.rgb(20,22,24)); c.drawCircle(px,py,4.1f*ms,fillPaint);
            fillPaint.setColor(color); c.drawCircle(px,py,3f*ms,fillPaint);
            target(px,py,p.x,p.y,p.z,categoryName(p.category),p.label,T_NAV,-1);
        }
        drawRouteMarks(c,a,active);
        if (lvalid && current==selected) {
            float px=sx(lx), py=sy(lz);
            float dx=-(float)Math.sin(lyaw), dy=(float)Math.cos(lyaw);   // FacingOnMap (bbport_map_direction.h)
            float angle=(float)Math.atan2(dx,-dy);
            fillPaint.setColor(Color.argb(27,96,139,138)); c.drawCircle(px,py,18.5f*ms,fillPaint);
            fillPaint.setColor(Color.argb(210,20,29,29)); c.drawCircle(px,py,16f*ms,fillPaint);
            strokePaint.setColor(Color.argb(120,125,160,155)); strokePaint.setStrokeWidth(Math.max(1,.8f*ms)); c.drawCircle(px,py,17f*ms,strokePaint);
            icon(c,4,px,py,35*ms,(float)Math.toDegrees(angle),playerTint);
        }
        if (mini) drawEdgeArrow(c,a);
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

    // ---- route finding: drawing ----

    private static final int ROUTE_COLOR=Color.rgb(255,214,84), GATE_OPEN=Color.rgb(96,204,116), GATE_BLOCKED=Color.rgb(226,84,72),
        GATE_ONEWAY=Color.rgb(240,176,56), GATE_FREE=Color.rgb(196,194,176);

    /** Places the prints by arc length along the route polyline: every strideW metres (world), each at the exact interpolated
     *  point with the direction of the polyline segment it is on (never an averaged direction). The stepping restarts (first
     *  print half a stride on) after each ladder, lift or drop and where the route changes floor shown, so no print lies across
     *  a transition. Made again only when the route, the floor shown or strideW (the zoom bucket) changes. */
    private void ensurePrints(NavData.Route r,AtlasModel.Area a,int active,float strideW) {
        float[] band=floorBand(a,active);
        if (r==pathRoute && active==pathLayer && band[0]==pathLo && band[1]==pathHi && strideW==stepStride) return;
        pathRoute=r; pathLayer=active; pathLo=band[0]; pathHi=band[1]; stepStride=strideW;
        stepCount=0;
        float rem=strideW*.5f; int prevOn=-1;
        for (int i=1;i<r.n;++i) {
            byte k=r.kind[i];
            if (k==NavData.K_LADDER || k==NavData.K_LIFT || k==NavData.K_DROP) { rem=strideW*.5f; prevOn=-1; continue; }
            float x0=r.x[i-1], z0=r.z[i-1], dx=r.x[i]-x0, dz=r.z[i]-z0, len=(float)Math.hypot(dx,dz);
            if (len<0.01f) continue;
            float ym=(r.y[i-1]+r.y[i])*.5f;
            int on=ym>=pathLo && ym<=pathHi ? 1 : 0;
            if (prevOn>=0 && on!=prevOn) rem=strideW*.5f; // the floor shown changes here
            prevOn=on;
            float ang=(float)Math.toDegrees(Math.atan2(-dz,dx)), rad=(float)Math.toRadians(ang), sn=(float)Math.sin(rad), cs=(float)Math.cos(rad);
            float c0=r.cum!=null ? r.cum[i-1] : 0, cl=r.cum!=null ? r.cum[i]-r.cum[i-1] : 0;
            float at=rem;
            while (at<=len) {
                if (stepCount==stepX.length) growSteps();
                int n=stepCount++;
                stepX[n]=x0+dx*at/len; stepZ[n]=z0+dz*at/len; stepAng[n]=ang; stepSin[n]=sn; stepCos[n]=cs; stepOn[n]=(byte)on; stepCum[n]=c0+cl*at/len;
                at+=strideW;
            }
            rem=at-len;
        }
    }
    private void growSteps() {
        int n=stepX.length*2;
        stepX=java.util.Arrays.copyOf(stepX,n); stepZ=java.util.Arrays.copyOf(stepZ,n); stepAng=java.util.Arrays.copyOf(stepAng,n);
        stepSin=java.util.Arrays.copyOf(stepSin,n); stepCos=java.util.Arrays.copyOf(stepCos,n); stepCum=java.util.Arrays.copyOf(stepCum,n);
        stepOn=java.util.Arrays.copyOf(stepOn,n);
    }

    /** One shoe print (a sole and a heel) pointing right, drawn with its light halo (haloPx wide outside the shoe) at length
     *  lenPx; the right foot is the left one mirrored. Made again only when the sizes change (density, size, mini or not). */
    private static Bitmap makeFoot(int side,int lenPx,float haloPx) {
        float S=lenPx/FOOT_SHAPE;
        int m=(int)Math.ceil(haloPx)+2, w=lenPx+2*m, h=(int)(S*.56f)+2*m;
        Bitmap b=Bitmap.createBitmap(w,h,Bitmap.Config.ARGB_8888);
        Canvas c=new Canvas(b);
        c.translate(w/2f,h/2f); c.scale(S,S*side);
        Path p=new Path();
        Path sole=new Path(); sole.addOval(new RectF(-.01f,-.19f,.51f,.19f),Path.Direction.CW);
        Matrix rot=new Matrix(); rot.setRotate(-8f,.25f,0f); sole.transform(rot);
        p.addPath(sole);
        p.addOval(new RectF(-.45f,-.11f,-.19f,.11f),Path.Direction.CW);
        Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setStyle(Paint.Style.FILL_AND_STROKE); paint.setStrokeJoin(Paint.Join.ROUND);
        paint.setStrokeWidth(2*haloPx/S); paint.setColor(FOOT_HALO); c.drawPath(p,paint);
        paint.setStyle(Paint.Style.FILL); paint.setColor(FOOT_INK); c.drawPath(p,paint);
        return b;
    }

    /** The first step whose length along the route is at least len. */
    private int firstStep(float len) {
        int lo=0, hi=stepCount;
        while (lo<hi) { int mid=(lo+hi)>>>1; if (stepCum[mid]<len) lo=mid+1; else hi=mid; }
        return lo;
    }

    /** The print's length on screen (px) at zoom z: the size choice (FOOT_LEN_*, in dp) however far out the map is, growing with
     *  the world (to FOOT_UP times) once the world stride is wider than FOOT_GAP print lengths. */
    private float footLen(float base,float z) { return base*Math.max(1f,Math.min(FOOT_UP,STRIDE*z/(FOOT_GAP*base))); }

    /** Adds the dashes of the segment (x0,y0)-(x1,y1) (screen) to the dash list; phase is the length along the route so far. */
    private void addDashes(boolean on,float x0,float y0,float x1,float y1,float phase,float dash,float period) {
        float dx=x1-x0, dy=y1-y0, len=(float)Math.hypot(dx,dy);
        if (len<1e-3f) return;
        // the part of the segment inside the (widened) canvas, by parameter
        float t0=0, t1=1, m=period;
        for (int ax=0;ax<2;++ax) {
            float p0=ax==0 ? x0 : y0, d=ax==0 ? dx : dy;
            float lo=(ax==0 ? canvasRect.left : canvasRect.top)-m, hi=(ax==0 ? canvasRect.right : canvasRect.bottom)+m;
            if (Math.abs(d)<1e-6f) { if (p0<lo || p0>hi) return; continue; }
            float ta=(lo-p0)/d, tb=(hi-p0)/d;
            if (ta>tb) { float t=ta; ta=tb; tb=t; }
            t0=Math.max(t0,ta); t1=Math.min(t1,tb);
            if (t0>=t1) return;
        }
        float ux=dx/len, uy=dy/len;
        float s=t0*len, e=t1*len;
        float q=(float)Math.floor((phase+s)/period)*period-phase; // start of the dash period that holds s
        for (;q<e;q+=period) {
            float a0=Math.max(s,q), a1=Math.min(e,q+dash);
            if (a1<=a0) continue;
            float[] buf; int n;
            if (on) { if (dashOnN+4>dashOn.length) dashOn=java.util.Arrays.copyOf(dashOn,dashOn.length*2); buf=dashOn; n=dashOnN; dashOnN+=4; }
            else { if (dashOffN+4>dashOff.length) dashOff=java.util.Arrays.copyOf(dashOff,dashOff.length*2); buf=dashOff; n=dashOffN; dashOffN+=4; }
            buf[n]=x0+ux*a0; buf[n+1]=y0+uy*a0; buf[n+2]=x0+ux*a1; buf[n+3]=y0+uy*a1;
        }
    }

    /** The exact route as a thin dotted ink line along the polyline (ahead of the player; ladder and lift hops are the marks),
     *  with a light halo, parts on other floors fainter. Screen space; no allocation. */
    private void drawRouteLine(Canvas c,NavData.Route r,AtlasModel.Area a,int active,float boost) {
        float dash=4*dens, gap=3*dens, period=dash+gap;
        float from=nav.progressLength();
        dashOnN=0; dashOffN=0;
        float phase=0, px=sx(r.x[0]), py=sy(r.z[0]);
        for (int i=1;i<r.n;++i) {
            float qx=sx(r.x[i]), qy=sy(r.z[i]);
            byte k=r.kind[i];
            float seg=(float)Math.hypot(qx-px,qy-py);
            if (k!=NavData.K_LADDER && k!=NavData.K_LIFT && r.cum[i]>from) {
                float ax=px, ay=py;
                float cl=r.cum[i]-r.cum[i-1];
                float ph=phase;
                if (from>r.cum[i-1] && cl>1e-6f) { float t=(from-r.cum[i-1])/cl; ax=px+(qx-px)*t; ay=py+(qy-py)*t; ph+=seg*t; }
                float ym=(r.y[i-1]+r.y[i])*.5f;
                boolean on=ym>=pathLo && ym<=pathHi && k!=NavData.K_DROP;
                addDashes(on,ax,ay,qx,qy,ph,dash,period);
            }
            phase+=seg; px=qx; py=qy;
        }
        float w=Math.max(1.5f,1.5f*dens), halo=Math.max(1f,1f*dens);
        dotPaint.setStyle(Paint.Style.STROKE); dotPaint.setStrokeCap(Paint.Cap.BUTT);
        for (int pass=0;pass<2;++pass) {
            float[] buf=pass==0 ? dashOn : dashOff; int n=pass==0 ? dashOnN : dashOffN;
            if (n==0) continue;
            float f=pass==0 ? 1f : FOOT_OFF;
            dotPaint.setStrokeWidth(w+2*halo); dotPaint.setColor(Color.argb((int)(Math.min(1f,.6f*f*boost)*217),243,230,200));
            c.drawLines(buf,0,n,dotPaint);
            dotPaint.setStrokeWidth(w); dotPaint.setColor(Color.argb((int)(Math.min(1f,.6f*f*boost)*255),43,29,16));
            c.drawLines(buf,0,n,dotPaint);
        }
    }

    /** The route as the Marauder's Map draws a walk: the exact path as a thin dotted line, with footprints on it alternating left
     *  and right (a little), placed by arc length (see ensurePrints) at least FOOT_GAP print lengths apart on screen, in dark ink
     *  with a light halo, sized in screen space (see FOOT_LEN_*). The route behind the player is not drawn; the prints ahead are
     *  faint, and a walker prints them one after another (FOOT_RATE a second) from the player toward the destination, each print
     *  then fading over FOOT_TRAIL steps, looped. Parts of other floors are fainter. Screen space. */
    private void drawFootprints(Canvas c,AtlasModel.Area a,int active) {
        NavData.Route r=nav!=null ? nav.route() : null;
        if (r==null || !r.area.equals(a.id) || r.n<2) return;
        // the mini map is shown at the user's opacity (a view alpha): the route is drawn stronger to stay legible
        float boost=mini ? Math.max(1f,Math.min(1.6f,.65f/Math.max(.2f,getAlpha()))) : 1f;
        // the placement is made for the zoom bucket below the zoom; the stride is wide enough for the largest print of the bucket
        float base=(mini ? FOOT_LEN_MINI : FOOT_LEN_FULL)*dens*footMul;
        float zb=(float)Math.pow(ZOOM_BUCKET,Math.floor(Math.log(Math.max(1e-6f,zoom))/Math.log(ZOOM_BUCKET)));
        float strideW=Math.max(STRIDE,FOOT_GAP*footLen(base,zb*ZOOM_BUCKET)/zb);
        ensurePrints(r,a,active,strideW);
        drawRouteLine(c,r,a,active,boost);
        int n=stepCount;
        if (n==0) return;
        int lenPx=Math.max(8,Math.round(base*FOOT_UP));
        float haloPx=(mini ? 1.6f : 1.2f)*dens*FOOT_UP;
        int haloKey=Math.round(haloPx*4);
        if (footL==null || lenPx!=footKeyLen || haloKey!=footKeyHalo) {
            footKeyLen=lenPx; footKeyHalo=haloKey; footL=makeFoot(1,lenPx,haloPx); footR=makeFoot(-1,lenPx,haloPx);
        }
        float len=footLen(base,zoom);
        float lat=FOOT_LAT*len, sc=len/lenPx;
        int m0=firstStep(nav.progressLength()-STRIDE*.6f), mEnd=n-1;
        if (m0>mEnd) return; // the player is past the last print
        boolean anim=footAnimate;
        float pw=0;
        if (anim) {
            float cycle=(mEnd-m0+1)+FOOT_TRAIL+5;
            double t=SystemClock.uptimeMillis()*(FOOT_RATE/1000.0);
            pw=m0+(float)(t-Math.floor(t/cycle)*cycle);
            animDelay=Math.min(animDelay,mini ? MINI_ANIM_MS : ANIM_MS);
        }
        float left=canvasRect.left-len*2, right=canvasRect.right+len*2, top=canvasRect.top-len*2, bottom=canvasRect.bottom+len*2;
        float hw=footL.getWidth()*.5f, hh=footL.getHeight()*.5f;
        for (int m=m0;m<=mEnd;++m) {
            float px=sx(stepX[m]), py=sy(stepZ[m]);
            if (px<left || px>right || py<top || py>bottom) continue;
            float alpha;
            if (!anim) alpha=FOOT_STATIC;
            else {
                float d=pw-m;
                if (d<0 || d>=FOOT_TRAIL) alpha=FOOT_BASE;
                else alpha=FOOT_BASE+(1-FOOT_BASE)*(1-d/FOOT_TRAIL)*Math.min(1f,d/.35f);
            }
            if (stepOn[m]==0) alpha*=FOOT_OFF;
            alpha=Math.min(1f,alpha*boost);
            int side=(m&1)==0 ? -1 : 1;
            px-=stepSin[m]*lat*side; py+=stepCos[m]*lat*side;
            footMatrix.setTranslate(-hw,-hh); footMatrix.postScale(sc,sc); footMatrix.postRotate(stepAng[m]); footMatrix.postTranslate(px,py);
            footPaint.setAlpha((int)(alpha*255));
            c.drawBitmap(side<0 ? footL : footR,footMatrix,footPaint);
        }
    }

    /** The marks where the route climbs, rides, drops or needs a lever, and the destination pin (screen space). */
    private void drawRouteMarks(Canvas c,AtlasModel.Area a,int active) {
        if (nav==null) return;
        NavData.Route r=nav.route();
        if (r!=null && r.area.equals(a.id) && r.n>=2) {
            float[] band=floorBand(a,active);
            for (int k=0;k<r.marks;++k) {
                float px=sx(r.mx[k]), py=sy(r.mz[k]);
                if (!canvasRect.contains(px,py)) continue;
                boolean on=r.my[k]>=band[0] && r.my[k]<=band[1];
                int kind=r.mkind[k];
                if (kind==NavData.K_LADDER) icon(c,1,px,py,26*ms,0,on ? null : dimLamp);
                else if (kind==NavData.K_DROP) glyph(c,px,py,ms,0,on);
                else if (kind==NavData.K_LIFT) glyph(c,px,py,ms,1,on);
                else if (kind==NavData.M_LEVER) glyph(c,px,py,ms,2,on);
            }
        }
        AtlasNav.Dest d=nav.dest();
        if (d!=null && d.area.equals(a.id)) {
            float px=sx(d.x), py=sy(d.z);
            float[] band=floorBand(a,active);
            boolean on=d.y>=band[0]-0.5f && d.y<=band[1]+0.5f;
            if (canvasRect.contains(px,py-10*ms)) pin(c,px,py,ms,on);
        }
    }

    /** Small vector marks: 0 drop (down arrow), 1 lift (box with up / down arrows), 2 lever (handle). */
    private void glyph(Canvas c,float px,float py,float u,int type,boolean on) {
        int alpha=on ? 255 : 130;
        float r=7.5f*u;
        fillPaint.setColor(Color.argb(alpha*7/10,12,14,14)); c.drawCircle(px,py,r,fillPaint);
        strokePaint.setColor(Color.argb(alpha,255,244,200)); strokePaint.setStrokeWidth(Math.max(1.2f,1.3f*u));
        Path p=scratch; p.reset();
        if (type==0) { p.moveTo(px,py-r*.55f); p.lineTo(px,py+r*.55f); p.moveTo(px-r*.4f,py+r*.15f); p.lineTo(px,py+r*.55f); p.lineTo(px+r*.4f,py+r*.15f); }
        else if (type==1) {
            p.moveTo(px-r*.55f,py-r*.1f); p.lineTo(px,py-r*.6f); p.lineTo(px+r*.55f,py-r*.1f);
            p.moveTo(px-r*.55f,py+r*.1f); p.lineTo(px,py+r*.6f); p.lineTo(px+r*.55f,py+r*.1f);
        } else { p.moveTo(px-r*.45f,py+r*.55f); p.lineTo(px+r*.2f,py-r*.35f); p.addCircle(px+r*.3f,py-r*.5f,r*.2f,Path.Direction.CW); }
        c.drawPath(p,strokePaint);
    }

    private void pin(Canvas c,float px,float py,float u,boolean on) {
        int alpha=on ? 255 : 140;
        float r=6.5f*u, h=15*u;
        Path p=scratch; p.reset();
        p.moveTo(px,py); p.lineTo(px-r*.8f,py-h+r*.35f); p.lineTo(px+r*.8f,py-h+r*.35f); p.close();
        fillPaint.setColor(Color.argb(alpha,16,16,16)); c.drawCircle(px,py-h,r*1.28f,fillPaint); c.drawPath(p,fillPaint);
        fillPaint.setColor(Color.argb(alpha,232,76,62)); c.drawCircle(px,py-h,r,fillPaint); c.drawPath(p,fillPaint);
        fillPaint.setColor(Color.argb(alpha,255,244,230)); c.drawCircle(px,py-h,r*.38f,fillPaint);
    }

    /** Doors and mechanisms of the area as small bars / marks, coloured by their state from the live flags. */
    private void drawGates(Canvas c,AtlasModel.Area a,int active) {
        if (nav==null || state==null || a.gates==null) return;
        NavGate[] gates=a.gates;
        byte[] st=nav.drawStates(a.id,state);
        if (st==null) return;
        float[] band=floorBand(a,active);
        float margin=30*ms;
        for (int i=0;i<gates.length;++i) {
            NavGate g=gates[i];
            if (g.type.equals(NavGate.FOG)) continue; // boss fog walls: passable, not drawn
            int color=st[i]==AtlasNav.S_OPEN ? GATE_OPEN : st[i]==AtlasNav.S_BLOCKED ? GATE_BLOCKED : st[i]==AtlasNav.S_ONEWAY ? GATE_ONEWAY : GATE_FREE;
            if (g.seg!=null) {
                if (g.yHi<band[0]-0.5f || g.yLo>band[1]+0.5f) continue;
                float x0=sx(g.seg[0]), y0=sy(g.seg[1]), x1=sx(g.seg[2]), y1=sy(g.seg[3]);
                float mx=(x0+x1)*.5f, my=(y0+y1)*.5f;
                if (mx<canvasRect.left-margin || my<canvasRect.top-margin || mx>canvasRect.right+margin || my>canvasRect.bottom+margin) continue;
                strokePaint.setStrokeCap(Paint.Cap.BUTT);
                strokePaint.setColor(Color.argb(210,12,14,14)); strokePaint.setStrokeWidth(Math.max(3f,4.6f*ms)); c.drawLine(x0,y0,x1,y1,strokePaint);
                strokePaint.setColor(color); strokePaint.setStrokeWidth(Math.max(2f,3.0f*ms)); c.drawLine(x0,y0,x1,y1,strokePaint);
                // a dot on the side the door opens from
                if (st[i]!=AtlasNav.S_OPEN && (g.side==NavGate.FRONT || g.side==NavGate.BACK) && g.hasFrame) {
                    float sgn=g.side==NavGate.FRONT ? 1 : -1;
                    float dx=sx(g.cx+g.nx*sgn*1.1f), dy=sy(g.cz+g.nz*sgn*1.1f);
                    fillPaint.setColor(Color.argb(235,12,14,14)); c.drawCircle(dx,dy,3.0f*ms,fillPaint);
                    fillPaint.setColor(color); c.drawCircle(dx,dy,2.0f*ms,fillPaint);
                }
                if (g.type.equals(NavGate.KEY) && st[i]!=AtlasNav.S_OPEN) keyGlyph(c,mx,my-6.5f*ms,ms,color);
                target(mx,my,g.cx!=0||g.cz!=0 ? g.cx : g.x,g.y,g.cx!=0||g.cz!=0 ? g.cz : g.z,"Gate","",T_GATE,i);
            } else if (g.stops.length>=3 || g.type.equals(NavGate.LADDER)) {
                int n=g.type.equals(NavGate.LADDER) ? 1 : g.stops.length/3;
                for (int k=0;k<n;++k) {
                    float gx=g.type.equals(NavGate.LADDER) ? g.x : g.stops[k*3], gy=g.type.equals(NavGate.LADDER) ? g.y : g.stops[k*3+1], gz=g.type.equals(NavGate.LADDER) ? g.z : g.stops[k*3+2];
                    if (gy<band[0]-0.5f || gy>band[1]+0.5f) continue;
                    float px=sx(gx), py=sy(gz);
                    if (!canvasRect.contains(px,py)) continue;
                    fillPaint.setColor(Color.argb(215,12,14,14)); c.drawCircle(px,py,6.2f*ms,fillPaint);
                    strokePaint.setColor(color); strokePaint.setStrokeWidth(Math.max(1.5f,1.7f*ms)); c.drawCircle(px,py,5f*ms,strokePaint);
                    glyph(c,px,py,ms*.75f,g.type.equals(NavGate.LIFT) ? 1 : 2,true);
                    target(px,py,gx,gy,gz,"Gate","",T_GATE,i);
                }
            }
        }
    }

    private void keyGlyph(Canvas c,float px,float py,float u,int color) {
        strokePaint.setColor(Color.argb(230,12,14,14)); strokePaint.setStrokeWidth(Math.max(3f,3.3f*u));
        Path p=scratch; p.reset();
        p.addCircle(px-3.2f*u,py,2.3f*u,Path.Direction.CW); p.moveTo(px-.9f*u,py); p.lineTo(px+4.2f*u,py); p.moveTo(px+2.6f*u,py); p.lineTo(px+2.6f*u,py+2.2f*u); p.moveTo(px+4.2f*u,py); p.lineTo(px+4.2f*u,py+2.2f*u);
        c.drawPath(p,strokePaint);
        strokePaint.setColor(color); strokePaint.setStrokeWidth(Math.max(1.4f,1.5f*u)); c.drawPath(p,strokePaint);
    }

    /** The mini map's edge arrow: points toward where the route leaves the map (or the destination), with the distance. */
    private void drawEdgeArrow(Canvas c,AtlasModel.Area a) {
        if (nav==null || !nav.hasDestination()) return;
        AtlasNav.Dest d=nav.dest();
        if (!d.area.equals(a.id)) return;
        float inset=13*ms;
        tmpRect.set(canvasRect.left+inset,canvasRect.top+inset+18*dens,canvasRect.right-inset,canvasRect.bottom-inset-(nav.hasDestination() ? 18*dens : 0));
        if (tmpRect.contains(sx(d.x),sy(d.z))) return;
        float tx=sx(d.x), ty=sy(d.z);
        NavData.Route r=nav.route();
        if (r!=null && r.area.equals(a.id)) {
            float from=nav.progressLength();
            for (int i=0;i<r.n;++i) {
                if (r.cum[i]<from) continue;
                float qx=sx(r.x[i]), qy=sy(r.z[i]);
                if (!tmpRect.contains(qx,qy)) { tx=qx; ty=qy; break; }
            }
        }
        float dx=tx-ccx, dy=ty-ccy, len=(float)Math.hypot(dx,dy);
        if (len<1) return;
        dx/=len; dy/=len;
        float hw=tmpRect.width()*.5f, hh=tmpRect.height()*.5f;
        float t=Math.min(dx!=0 ? hw/Math.abs(dx) : 1e9f,dy!=0 ? hh/Math.abs(dy) : 1e9f);
        float ax=tmpRect.centerX()+dx*t, ay=tmpRect.centerY()+dy*t;
        float s=8*ms;
        Path p=scratch; p.reset(); p.moveTo(s,0); p.lineTo(-s*.7f,-s*.75f); p.lineTo(-s*.3f,0); p.lineTo(-s*.7f,s*.75f); p.close();
        c.save(); c.translate(ax,ay); c.rotate((float)Math.toDegrees(Math.atan2(dy,dx)));
        fillPaint.setColor(Color.argb(235,12,14,14)); c.scale(1.25f,1.25f); c.drawPath(p,fillPaint); c.scale(.8f,.8f);
        fillPaint.setColor(ROUTE_COLOR); c.drawPath(p,fillPaint);
        c.restore();
        String dist=distanceText(nav.remaining()>=0 ? nav.remaining() : nav.straightLine());
        if (!dist.isEmpty()) {
            float size=fit(dist,bodyFace,10.5f*dens,7*dens,canvasRect.width()*.4f);
            textPaint.setTypeface(bodyFace); textPaint.setTextSize(size);
            float tw=textPaint.measureText(dist);
            float lx2=Math.max(canvasRect.left+3*dens,Math.min(canvasRect.right-tw-3*dens,ax-tw*.5f-dx*(s*1.9f+tw*.5f)));
            float ly2=Math.max(canvasRect.top+18*dens+size,Math.min(canvasRect.bottom-20*dens,ay-dy*s*2.2f+size*.35f));
            fillPaint.setColor(Color.argb(170,12,14,14)); c.drawRoundRect(lx2-3*dens,ly2-size,lx2+tw+3*dens,ly2+size*.3f,3*dens,3*dens,fillPaint);
            text(c,dist,lx2,ly2,size,PAPER,bodyFace,Paint.Align.LEFT);
        }
    }

    private static String distanceText(float meters) {
        if (meters<0) return "";
        return meters>=1000 ? String.format(java.util.Locale.ROOT,"%.1f km",meters/1000) : String.format(java.util.Locale.ROOT,"%d m",Math.round(meters));
    }

    /** The destination chip at the top of the map: the target, the remaining distance and a cross that clears it. */
    private void drawChip(Canvas c) {
        chipRect.setEmpty(); chipClear.setEmpty();
        if (nav==null) return;
        boolean arrived=nav.arrived();
        AtlasNav.Dest d=nav.dest();
        if (d==null && !arrived) return;
        String name=arrived ? "Arrived" : d.marked ? "Marked point" : d.name.isEmpty() ? "Destination" : d.name;
        String detail=arrived ? "" : nav.chipDetail();
        float size=15*dens, pad=10*dens, clearW=arrived ? 0 : 38*dens, h=38*dens;
        float maxW=canvasRect.width()*.92f-clearW-2*pad-12*dens;
        textPaint.setTypeface(bodyFace); textPaint.setTextSize(size);
        float dw=detail.isEmpty() ? 0 : textPaint.measureText(detail)+14*dens;
        float nameMax=Math.max(60*dens,maxW-dw);
        String shownName=name; 
        while (shownName.length()>3 && textPaint.measureText(shownName)>nameMax) shownName=shownName.substring(0,shownName.length()-2);
        if (!shownName.equals(name)) shownName=shownName.trim()+"…";
        float nw=textPaint.measureText(shownName);
        float w=pad*2+nw+dw+clearW;
        float left=canvasRect.centerX()-w/2, top=canvasRect.top+8*dens;
        chipRect.set(left,top,left+w,top+h);
        fillPaint.setColor(Color.argb(228,27,29,27)); c.drawRoundRect(chipRect,h/2,h/2,fillPaint);
        strokePaint.setColor(arrived ? GATE_OPEN : GOLD); strokePaint.setStrokeWidth(Math.max(1,dens*.9f)); c.drawRoundRect(chipRect,h/2,h/2,strokePaint);
        float base=chipRect.centerY()+size*.35f;
        text(c,shownName,left+pad,base,size,arrived ? GATE_OPEN : PAPER,bodyFace,Paint.Align.LEFT);
        if (!detail.isEmpty()) text(c,detail,left+pad+nw+14*dens,base,size,detail.startsWith("No route") || detail.startsWith("Go to") ? MUTED : GOLD,bodyFace,Paint.Align.LEFT);
        if (!arrived) {
            chipClear.set(chipRect.right-clearW,chipRect.top,chipRect.right,chipRect.bottom);
            float cxp=chipClear.centerX()-2*dens, cyp=chipClear.centerY(), r=5*dens;
            strokePaint.setColor(pressed==chipClear ? GOLD : PAPER); strokePaint.setStrokeWidth(Math.max(1.5f,1.6f*dens));
            c.drawLine(cxp-r,cyp-r,cxp+r,cyp+r,strokePaint); c.drawLine(cxp-r,cyp+r,cxp+r,cyp-r,strokePaint);
        }
        if (arrived) postInvalidateDelayed(3200);
    }

    /** The mini map's destination line (bottom strip): "→ name · distance", or Arrived. */
    private void drawMiniDest(Canvas c,int w,int h) {
        if (nav==null) return;
        boolean arrived=nav.arrived();
        if (!nav.hasDestination() && !arrived) return;
        String name=arrived ? "Arrived" : nav.dest().marked ? "Marked point" : nav.dest().name;
        String detail=arrived ? "" : nav.chipDetail();
        String t=detail.isEmpty() ? name : name+" · "+detail;
        float strip=18*dens, size=fit(t,bodyFace,11*dens,7.5f*dens,w-10*dens);
        fillPaint.setColor(Color.argb(165,12,12,12)); c.drawRect(0,h-strip,w,h,fillPaint);
        text(c,t,w*.5f,h-strip*.5f+size*.34f,size,arrived ? GATE_OPEN : ROUTE_COLOR,bodyFace,Paint.Align.CENTER);
        if (arrived) postInvalidateDelayed(3200);
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
    private void target(float px,float py,float wx,float wz,String title,String sub) { target(px,py,wx,0,wz,title,sub,T_LABEL,-1); }
    private void target(float px,float py,float wx,float wy,float wz,String title,String sub,int kind,int gate) {
        if (mini || targets>=MAX_TARGETS) return;
        tx[targets]=px; ty[targets]=py; twx[targets]=wx; twy[targets]=wy; twz[targets]=wz; tTitle[targets]=title; tSub[targets]=sub; tKind[targets]=kind; tGate[targets]=gate; ++targets;
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
        if (buttonShown(btnClose)) {
            button(c,btnClose,true,pressed==btnClose);
            float cr=Math.min(btnClose.height()*.16f,7*dens), cxp=btnClose.left+btnClose.height()*.5f, cyp=btnClose.centerY();
            strokePaint.setStrokeCap(Paint.Cap.ROUND); strokePaint.setColor(PAPER); strokePaint.setStrokeWidth(Math.max(2f,2f*dens));
            c.drawLine(cxp-cr,cyp-cr,cxp+cr,cyp+cr,strokePaint); c.drawLine(cxp-cr,cyp+cr,cxp+cr,cyp-cr,strokePaint);
            strokePaint.setStrokeCap(Paint.Cap.BUTT);
            float cs=Math.min(17*dens,btnClose.height()*.36f);
            text(c,"Close",(cxp+cr+btnClose.right-4*dens)*.5f,btnClose.centerY()+cs*.35f,cs,PAPER,bodyFace,Paint.Align.CENTER);
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
        if (SystemClock.uptimeMillis()>cardUntil) { cardTitle=null; cardNav=false; return; }
        float px=sx(cardX), py=sy(cardZ);
        if (!canvasRect.contains(px,py)) { cardRect.setEmpty(); cardBtn.setEmpty(); return; }
        float ts=15*dens, ss=12*dens, pad=9*dens;
        String[] lines=cardSub==null ? new String[0] : cardSub.split("\n");
        int shown=Math.min(lines.length,6);
        textPaint.setTypeface(bodyFace);
        textPaint.setTextSize(ts); float wid=textPaint.measureText(cardTitle);
        textPaint.setTextSize(ss);
        for (int i=0;i<shown;++i) wid=Math.max(wid,textPaint.measureText(lines[i]));
        float buttonH=cardNav ? 34*dens : 0, buttonGap=cardNav ? 6*dens : 0;
        if (cardNav) { textPaint.setTextSize(ts); wid=Math.max(wid,textPaint.measureText("Navigate")+36*dens); }
        float w=Math.min(canvasRect.width()*.8f,wid+2*pad), h=pad*2+ts*1.2f+shown*ss*1.3f+buttonGap+buttonH;
        float left=Math.max(canvasRect.left+4*dens,Math.min(canvasRect.right-w-4*dens,px-w/2));
        float top=py-24*ms-h; if (top<canvasRect.top+4*dens) top=py+24*ms;
        tmpRect.set(left,top,left+w,top+h); cardRect.set(tmpRect);
        fillPaint.setColor(Color.argb(235,27,29,27)); c.drawRoundRect(tmpRect,6*dens,6*dens,fillPaint);
        strokePaint.setColor(GOLD); strokePaint.setStrokeWidth(Math.max(1,dens*.8f)); c.drawRoundRect(tmpRect,6*dens,6*dens,strokePaint);
        text(c,cardTitle,left+pad,top+pad+ts,ts,PAPER,bodyFace,Paint.Align.LEFT);
        for (int i=0;i<shown;++i) text(c,lines[i],left+pad,top+pad+ts*1.2f+(i+1)*ss*1.3f-ss*.2f,ss,MUTED,bodyFace,Paint.Align.LEFT);
        if (cardNav) {
            float by=top+h-pad-buttonH;
            cardBtn.set(left+pad,by,left+w-pad,by+buttonH);
            fillPaint.setColor(Color.argb(235,103,91,63)); c.drawRoundRect(cardBtn,6*dens,6*dens,fillPaint);
            strokePaint.setColor(GOLD); c.drawRoundRect(cardBtn,6*dens,6*dens,strokePaint);
            text(c,"Navigate",cardBtn.centerX(),cardBtn.centerY()+ts*.35f,ts,PAPER,bodyFace,Paint.Align.CENTER);
        } else cardBtn.setEmpty();
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
