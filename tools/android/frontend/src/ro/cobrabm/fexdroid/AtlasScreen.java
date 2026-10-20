package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ActivityOptions;
import android.app.Presentation;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Build;
import android.os.Looper;
import android.view.Display;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;

/** Hunter's Atlas (https://github.com/MikeTat1988/HuntersAtlas by MikeTat1988, GPL-2.0-or-later; this map is a port
 *  of its renderer): owns the map pack's data, the live state read from the game (AtlasState) and the views that show
 *  the map (AtlasView), 0 to 2 of them: the full map in a Presentation on the second display and/or the floating mini
 *  map, a view in the game's own layout (made by createMini, placed and layered by MainActivity). The map is on with
 *  the atlas_enabled setting; atlas_mode says where: "screen2", "float" or "both" (unset: the second screen when
 *  there is one, else floating; with no second display the effective mode is always floating, and the saved choice
 *  applies again when a display appears). The main screen is the display the game's activity is on
 *  (activity.getDisplay()), a second screen is any other valid display (also the device's default display when the
 *  game runs elsewhere); atlas_display ("name|id") picks one of several, else an external (presentation) display
 *  added last, else the first. The second-screen map is shown while the pack is installed, the game's activity is
 *  up (onResume to onStop) and a second display exists; it follows displays that come and go. The presentation's
 *  window is not focusable: the gamepad and keys stay with the game's activity on the main display, touch works on the
 *  map. A display without FLAG_PRESENTATION (the built-in screen when the game runs on the other one) refuses a
 *  Presentation (InvalidDisplayException), so the map is an AtlasActivity launched on that display instead (not
 *  focusable either; it gives the game's task the top back as soon as it is up, and MainActivity does not pause the game
 *  for that short loss, see ownWindowHasTop). When neither can show the map the effective mode is the floating map. The floating map is visible while the game's activity is up, the game is not paused, its position has been
 *  valid since the game started and the layout editor is not open. One poll loop reads the game's state for both
 *  views (the file is read once per tick). */
final class AtlasScreen implements DisplayManager.DisplayListener {
    static final String PREF_ENABLED="atlas_enabled", PREF_PICKUPS="atlas_pickups", PREF_NPC="atlas_npc", PREF_FOG="atlas_fog",
        PREF_MODE="atlas_mode", PREF_ALPHA="atlas_float_alpha", PREF_DISPLAY="atlas_display";
    static final String MODE_SCREEN="screen2", MODE_FLOAT="float", MODE_BOTH="both";
    static final int ALPHA_MIN=20, ALPHA_MAX=100, ALPHA_STEP=5, ALPHA_DEFAULT=50;
    /** The floating map is redrawn at most about 15 times a second, and only when the player or the flags changed. */
    private static final long MINI_INTERVAL_MS=66;

    private final Activity activity;
    private final File base;
    private final SharedPreferences prefs;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final ExecutorService jobs=Executors.newSingleThreadExecutor(r -> { Thread t=new Thread(r,"Atlas-jobs"); t.setDaemon(true); return t; });
    private final AtlasState state;
    private final DisplayManager displays;

    private AtlasModel model; private boolean modelLoading;
    private Bitmap background, parchment, icons; private Typeface bodyFace, titleFace; private boolean artLoaded, artLoading;
    private AtlasPresentation presentation; private AtlasView view;   // the second display's map
    private AtlasActivity atlasActivity;                              // ... or this activity (displays without FLAG_PRESENTATION)
    /** An AtlasActivity launch in flight (the display it was asked for, the display the second screen was when asked,
     *  when) and the loop guards. */
    private int pendingDisplay=-1, pendingWant=-1; private long pendingSince, lastLaunch; private boolean retryPosted;
    /** The second screen cannot show the map (reason) on this main/second display pair: the effective mode is floating. */
    private String failureKey, failureReason;
    private long launchUntil, lastFocusRequest; private boolean fallbackTried, windowCheckPosted;
    private BooleanSupplier mainHasTop; private Runnable topChanged;
    private int lastMainId=-2; private long lastMainCheck;
    private static final long LAUNCH_TIMEOUT_MS=3500, LAUNCH_GUARD_MS=1000, TOP_WINDOW_MS=4000;
    private AtlasView mini;                                           // the floating mini map
    private boolean resumed, destroyed, listening, launchedWithAtlas, ticking;
    private boolean gamePaused, suppressed, everValid, miniFed, miniDirty;
    private long lastPrefs, lastMini;
    private Runnable configListener;
    private boolean sMode, fMode; // the effective mode (see mode()), refreshed when displays or settings change
    private final Map<Integer,Integer> addedOrder=new HashMap<>(); private int addedCounter;
    // what the floating map last drew
    private boolean sMapped, sValid; private float sx, sy, sz, sYaw; private int sBlock, sFlags;

    AtlasScreen(Activity activity,File base) {
        this.activity=activity; this.base=base;
        prefs=activity.getSharedPreferences("bloodborne",Context.MODE_PRIVATE);
        state=new AtlasState(new File(base,"atlas.shm"));
        displays=(DisplayManager)activity.getSystemService(Context.DISPLAY_SERVICE);
        try { titleFace=activity.getResources().getFont(activity.getResources().getIdentifier("im_fell_english","font",activity.getPackageName())); }
        catch (Exception e) { titleFace=Typeface.SERIF; }
        updateModes();
    }

    // ---- settings ----

    boolean enabled() { return prefs.getBoolean(PREF_ENABLED,false); }
    boolean installed() { return AtlasPack.installed(base); }
    private boolean wanted() { return enabled() && installed(); }
    void setEnabled(boolean on) { prefs.edit().putBoolean(PREF_ENABLED,on).apply(); notifyConfig(); }
    /** Where the map is shown: MODE_SCREEN, MODE_FLOAT or MODE_BOTH. With no second display always MODE_FLOAT (the saved
     *  choice is kept and applies again once a display appears); not chosen yet: the second screen when there is one. */
    String mode() {
        if (secondDisplay()==null) return MODE_FLOAT;
        String saved=savedMode();
        if (!MODE_FLOAT.equals(saved) && screenFailed()) return MODE_FLOAT; // the other screen cannot show the map
        return saved;
    }
    private String pairKey() {
        Display d=secondDisplay();
        return d==null ? null : mainDisplayId()+"/"+displayKey(d)+"/0x"+Integer.toHexString(d.getFlags());
    }
    /** The second screen failed to show the map for this pair of displays (the failure is forgotten when the displays
     *  change or the user picks the screen again, so it is tried again then). */
    private boolean screenFailed() { return failureKey!=null && failureKey.equals(pairKey()); }
    private void failScreen(String reason) {
        failureKey=pairKey(); failureReason=reason;
        android.util.Log.w("Bloodborne","atlas second screen: "+reason);
        main.post(this::displaysChanged);
    }
    private void clearFailure() { failureKey=null; failureReason=null; }
    /** The saved choice (a default when none) whether or not there is a second display. */
    String savedMode() {
        String v=prefs.getString(PREF_MODE,null);
        if (MODE_SCREEN.equals(v) || MODE_FLOAT.equals(v) || MODE_BOTH.equals(v)) return v;
        return secondDisplay()!=null ? MODE_SCREEN : MODE_FLOAT;
    }
    void setMode(String mode) { prefs.edit().putString(PREF_MODE,mode).apply(); clearFailure(); displaysChanged(); }
    private void updateModes() { String m=mode(); sMode=m.equals(MODE_SCREEN) || m.equals(MODE_BOTH); fMode=m.equals(MODE_FLOAT) || m.equals(MODE_BOTH); }
    private boolean screenMode() { return sMode; }
    private boolean floatMode() { return fMode; }
    /** The map is on and shown floating (the layout editor offers the mini map then). */
    boolean floatOn() { return enabled() && floatMode(); }
    /** The opacity of the floating map in percent: 20 to 100 in steps of 5, 50 by default. */
    int floatAlpha() {
        int v=prefs.getInt(PREF_ALPHA,ALPHA_DEFAULT);
        v=Math.max(ALPHA_MIN,Math.min(ALPHA_MAX,v));
        return ALPHA_MIN+Math.round((v-ALPHA_MIN)/(float)ALPHA_STEP)*ALPHA_STEP;
    }
    void setFloatAlpha(int percent) { prefs.edit().putInt(PREF_ALPHA,Math.max(ALPHA_MIN,Math.min(ALPHA_MAX,percent))).apply(); applyFloat(); }
    private void applyFloat() { if (mini!=null) mini.setAlpha(floatAlpha()/100f); }
    /** Called (main thread) when the map is turned on or off or its mode changes. */
    void setConfigListener(Runnable r) { configListener=r; }
    private void notifyConfig() { if (configListener!=null) configListener.run(); }
    /** The game is paused by the player: the floating map is hidden. */
    void setGamePaused(boolean paused) { if (gamePaused!=paused) { gamePaused=paused; updateMini(); } }
    /** The layout editor is open: the editor draws the mini map's place, the view itself is hidden. */
    void setSuppressed(boolean on) { if (suppressed!=on) { suppressed=on; updateMini(); } }
    boolean pickups() { return prefs.getBoolean(PREF_PICKUPS,true); }
    String npcMode() { String v=prefs.getString(PREF_NPC,"all"); return v.equals("new") || v.equals("hidden") ? v : "all"; }
    boolean fog() { return prefs.getBoolean(PREF_FOG,false); }
    void setOption(String key,boolean value) { prefs.edit().putBoolean(key,value).apply(); applyOptions(); }
    void setNpcMode(String mode) { prefs.edit().putString(PREF_NPC,mode).apply(); applyOptions(); }
    private void applyOptions() {
        if (view!=null) view.setOptions(pickups(),npcMode(),fog());
        if (mini!=null) mini.setOptions(pickups(),npcMode(),fog());
    }
    File base() { return base; }

    // ---- displays ----

    /** The display the game's activity is on (the main screen). */
    private Display mainDisplay() {
        Display d=null;
        if (Build.VERSION.SDK_INT>=30) { try { d=activity.getDisplay(); } catch (RuntimeException ignored) {} }
        if (d==null) { try { d=activity.getWindowManager().getDefaultDisplay(); } catch (RuntimeException ignored) {} }
        return d;
    }
    private int mainDisplayId() { Display d=mainDisplay(); return d!=null ? d.getDisplayId() : Display.DEFAULT_DISPLAY; }
    static String displayKey(Display d) { return d.getName()+"|"+d.getDisplayId(); }

    /** Every display that can show the map besides the game's own: valid, not private (no presentation can open on it). */
    List<Display> secondDisplays() {
        List<Display> out=new ArrayList<>();
        if (displays==null) return out;
        int main=mainDisplayId();
        for (Display d:displays.getDisplays()) {
            if (d.getDisplayId()==main || !d.isValid() || (d.getFlags()&Display.FLAG_PRIVATE)!=0) continue;
            if (!addedOrder.containsKey(d.getDisplayId())) addedOrder.put(d.getDisplayId(),++addedCounter);
            out.add(d);
        }
        return out;
    }
    /** The second screen to use: the saved one (atlas_display: name and id, else the name), else an external
     *  (presentation) display, the one added last, else the first other display. null: none. */
    Display secondDisplay() {
        List<Display> list=secondDisplays();
        if (list.isEmpty()) return null;
        String saved=prefs.getString(PREF_DISPLAY,null);
        if (saved!=null) {
            for (Display d:list) if (saved.equals(displayKey(d))) return d;
            int bar=saved.lastIndexOf('|'); String name=bar>0 ? saved.substring(0,bar) : saved;
            for (Display d:list) if (name.equals(d.getName())) return d;
        }
        Display best=null; int order=-1;
        for (Display d:list) {
            Integer o=addedOrder.get(d.getDisplayId());
            if ((d.getFlags()&Display.FLAG_PRESENTATION)!=0 && o!=null && o>order) { best=d; order=o; }
        }
        return best!=null ? best : list.get(0);
    }
    /** Chooses the display for the second-screen map (the dialog's Display list). */
    void setDisplay(Display d) { prefs.edit().putString(PREF_DISPLAY,displayKey(d)).apply(); clearFailure(); displaysChanged(); }
    /** Displays came or went, the game's activity moved to another display, or a setting changed: the effective mode,
     *  the presentation (on the right display, not on the game's own) and the floating map are evaluated again. */
    void displaysChanged() { updateModes(); sync(); notifyConfig(); }

    /** For the dialog: what is known about the second screen and the map. */
    String describe() {
        StringBuilder s=new StringBuilder();
        if (screenMode()) {
            Display d=secondDisplay();
            s.append(d==null ? "No second display connected."
                : "Second display: "+d.getName()+" (id "+d.getDisplayId()+(presentation!=null || atlasActivity!=null ? ", showing the map" : "")+")");
        }
        if (floatMode()) s.append(s.length()>0 ? "\n" : "").append("Mini map on the game screen (shown once the game is in a mapped place).");
        if (screenFailed() && !MODE_FLOAT.equals(savedMode()))
            s.append(s.length()>0 ? "\n" : "").append("The other screen can't show the map: ").append(failureReason).append(". The mini map is shown instead.");
        return s.toString();
    }
    /** The game was started with the map's files (so atlas.shm is being written). */
    boolean launchedWithAtlas() { return launchedWithAtlas; }

    // ---- lifecycle of the activity ----

    void resume() {
        resumed=true;
        if (!listening && displays!=null) { displays.registerDisplayListener(this,main); listening=true; }
        displaysChanged();
    }
    void stop() {
        resumed=false;
        if (listening && displays!=null) { displays.unregisterDisplayListener(this); listening=false; }
        sync();
    }
    void destroy() {
        destroyed=true; sync(); state.close(); jobs.shutdownNow(); main.removeCallbacks(tick); ticking=false;
    }
    /** The setting or the pack changed. */
    void refresh() { sync(); notifyConfig(); }
    /** The pack was installed or deleted: the data is read again. */
    void packChanged() {
        model=null; modelLoading=false; artLoaded=false;
        if (presentation!=null) { presentation.dismiss(); }
        if (atlasActivity!=null) hide();
        if (mini!=null) mini.setModel(null,null);
        sync();
    }

    private void sync() {
        boolean should=!destroyed && resumed && wanted();
        if (should) {
            if (presentation!=null || atlasActivity!=null) { // on a display that is gone, is now the game's own, or is not the chosen one
                Display cur=presentation!=null ? presentation.getDisplay() : displayOf(atlasActivity), want=secondDisplay();
                if (!screenMode() || want==null || cur==null || !cur.isValid() || cur.getDisplayId()!=want.getDisplayId()) hide();
            }
            if (screenMode()) show(); else hide();
            if (mini!=null && floatMode()) ensureData(mini);
            ensureTick();
        } else hide();
        applyFloat(); updateMini();
    }

    @Override public void onDisplayAdded(int id) { main.post(() -> { addedOrder.put(id,++addedCounter); clearFailure(); displaysChanged(); }); }
    @Override public void onDisplayChanged(int id) {
        main.post(() -> {
            AtlasPresentation p=presentation;
            if (p!=null && p.getDisplay()!=null && p.getDisplay().getDisplayId()==id) p.refreshMode(); // another mode or size
            AtlasActivity a=atlasActivity; Display ad=displayOf(a);
            if (a!=null && ad!=null && ad.getDisplayId()==id) a.refreshMode();
            displaysChanged();
        });
    }
    @Override public void onDisplayRemoved(int id) {
        main.post(() -> {
            addedOrder.remove(id); clearFailure();
            Display ad=displayOf(atlasActivity);
            if ((presentation!=null && presentation.getDisplay()!=null && presentation.getDisplay().getDisplayId()==id)
                || (ad!=null && ad.getDisplayId()==id)) hide();
            displaysChanged();
        });
    }

    private void hide() {
        AtlasPresentation p=presentation; presentation=null; view=null;
        AtlasActivity a=atlasActivity; atlasActivity=null;
        if (p!=null) { try { p.dismiss(); } catch (Exception ignored) {} }
        if (a!=null) { try { a.finishAndRemoveTask(); } catch (RuntimeException ignored) {} }
    }
    /** The display an AtlasActivity is on (null: none or unknown). */
    private static Display displayOf(AtlasActivity a) {
        if (a==null) return null;
        try { return a.getWindowManager().getDefaultDisplay(); } catch (RuntimeException e) { return null; }
    }

    private void show() {
        if (presentation!=null || atlasActivity!=null) return;
        Display d=secondDisplay();
        if (d==null) return;
        String why=null;
        if ((d.getFlags()&Display.FLAG_PRESENTATION)!=0) { // only these displays take a Presentation window
            AtlasPresentation p=new AtlasPresentation(activity,d,this);
            try { p.show(); }
            catch (RuntimeException e) { // WindowManager.InvalidDisplayException, BadTokenException
                why="a presentation was refused ("+e.getMessage()+")";
                android.util.Log.w("Bloodborne","atlas presentation: "+e+" (display "+d.getName()+" flags 0x"+Integer.toHexString(d.getFlags())+")");
                p=null;
            }
            if (p!=null) {
                final AtlasPresentation pp=p;
                presentation=pp;
                pp.setOnDismissListener(x -> { if (presentation==pp) { presentation=null; view=null; } });
                ensureTick();
                return;
            }
        }
        showActivity(d,why);
    }

    /** The map in an activity on a display that does not take a Presentation. One launch at a time, at most one per
     *  second (a map the system or the user closed is not opened again until the next display, mode or resume trigger). */
    private void showActivity(Display d,String presentationFailure) {
        long now=android.os.SystemClock.uptimeMillis();
        if (pendingDisplay>=0 && now-pendingSince<LAUNCH_TIMEOUT_MS) return; // in flight
        if (now-lastLaunch<LAUNCH_GUARD_MS) {
            if (!retryPosted) { retryPosted=true; main.postDelayed(() -> { retryPosted=false; sync(); },LAUNCH_GUARD_MS-(now-lastLaunch)+20); }
            return;
        }
        lastLaunch=now; pendingDisplay=d.getDisplayId(); pendingWant=d.getDisplayId(); pendingSince=now;
        fallbackTried=false;
        AtlasActivity.owner=this;
        Intent i=new Intent(activity,AtlasActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_MULTIPLE_TASK | Intent.FLAG_ACTIVITY_NO_ANIMATION);
        ActivityOptions o=ActivityOptions.makeBasic(); o.setLaunchDisplayId(d.getDisplayId());
        launchUntil=now+TOP_WINDOW_MS; // the game's activity is about to lose the top to ours: not a reason to pause
        try { activity.startActivity(i,o.toBundle()); }
        catch (RuntimeException e) {
            pendingDisplay=-1; launchUntil=0;
            failScreen((presentationFailure!=null ? presentationFailure+"; " : "")+"the map window could not start on "+d.getName()+": "+e.getMessage());
            return;
        }
        main.postDelayed(() -> {
            if (destroyed || pendingDisplay<0 || pendingSince!=now) return;
            pendingDisplay=-1;
            if (atlasActivity==null) failScreen((presentationFailure!=null ? presentationFailure+"; " : "")+"Android did not open the map window on "+d.getName());
        },LAUNCH_TIMEOUT_MS+100);
    }

    // ---- AtlasActivity (the map on a display that takes no Presentation) ----

    /** AtlasActivity.onCreate: may it stay (it is where it was asked to be and wanted)? false: it finishes itself. */
    boolean attach(AtlasActivity a) {
        Display here=displayOf(a), want=secondDisplay();
        int asked=pendingWant; pendingDisplay=-1;
        if (destroyed || !resumed || !wanted() || !screenMode() || want==null || here==null || atlasActivity!=null) return false;
        if (here.getDisplayId()==mainDisplayId()) { failScreen("the system opened the map on the game's screen instead"); return false; }
        if (here.getDisplayId()!=want.getDisplayId()) {
            if (asked==want.getDisplayId()) failScreen("the system opened the map on "+here.getName()+" instead of "+want.getName());
            else main.post(this::sync); // an old launch for the screen the map used to be on: asked again for the right one
            return false;
        }
        atlasActivity=a;
        return true;
    }
    /** AtlasActivity.onDestroy: closed by the system or the user (home or back on that display) when it was still ours:
     *  nothing is opened again until the next trigger. */
    void detached(AtlasActivity a) {
        if (atlasActivity==a) { atlasActivity=null; view=null; }
    }
    /** AtlasActivity got the top (window focus is not taken: it is not focusable): the game's task takes it back. */
    void atlasGotTop() {
        launchUntil=0;
        long now=android.os.SystemClock.uptimeMillis();
        if (now-lastFocusRequest<400) return;
        lastFocusRequest=now;
        main.post(() -> {
            if (destroyed) return;
            try { ((ActivityManager)activity.getSystemService(Context.ACTIVITY_SERVICE)).moveTaskToFront(activity.getTaskId(),0); }
            catch (RuntimeException e) { android.util.Log.w("Bloodborne","atlas: moveTaskToFront: "+e); }
            main.postDelayed(() -> {
                if (destroyed || mainHasTop==null || mainHasTop.getAsBoolean() || fallbackTried || atlasActivity==null) return;
                fallbackTried=true; // still not back: ask for the game's activity on its display, brought to the front of its task
                try {
                    Intent i=new Intent(activity,activity.getClass()).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_SINGLE_TOP);
                    ActivityOptions o=ActivityOptions.makeBasic(); o.setLaunchDisplayId(mainDisplayId());
                    activity.startActivity(i,o.toBundle());
                } catch (RuntimeException e) { android.util.Log.w("Bloodborne","atlas: reorder to front: "+e); }
            },700);
        });
    }
    /** AtlasActivity lost the top: the game's activity may be the one that has it, or another app. */
    void atlasLostTop() { Runnable r=topChanged; if (r!=null) main.post(r); }
    /** Is the top-resumed activity ours (an AtlasActivity that has it, or one just launched)? MainActivity then does not
     *  pause the game for losing the top. */
    boolean ownWindowHasTop() {
        if (AtlasActivity.topResumed) return true;
        long left=launchUntil-android.os.SystemClock.uptimeMillis();
        if (left<=0) return false;
        if (!windowCheckPosted) { // when the window runs out the pause is decided again
            windowCheckPosted=true;
            main.postDelayed(() -> { windowCheckPosted=false; Runnable r=topChanged; if (r!=null) r.run(); },left+50);
        }
        return true;
    }
    /** MainActivity: whether the system has given it the top (mainHasTop) and how to decide about pausing again (topChanged). */
    void setTopHooks(BooleanSupplier mainHasTop,Runnable topChanged) { this.mainHasTop=mainHasTop; this.topChanged=topChanged; }

    /** The view of a new presentation (made with its context: the second display's metrics). */
    AtlasView makeView(Context context) {
        final AtlasView v=new AtlasView(context,jobs,titleFace,bodyFace!=null ? bodyFace : Typeface.SANS_SERIF);
        view=v;
        v.setState(state);
        v.setOptions(pickups(),npcMode(),fog());
        ensureData(v);
        return v;
    }

    /** The floating mini map: a view for the game's layout, made once (hidden: this class shows it when it should
     *  be). MainActivity adds it above the game's surface and below the touch controller and the menu, sizes and
     *  places it (the layout editor's rect) and never gives it a touch: it is not clickable. Its opacity is a
     *  view alpha on a hardware layer, so overlapping draws are not blended twice. */
    AtlasView createMini(Context context) {
        final AtlasView v=new AtlasView(context,jobs,titleFace,bodyFace!=null ? bodyFace : Typeface.SANS_SERIF,true);
        mini=v;
        v.setState(state);
        v.setOptions(pickups(),npcMode(),fog());
        v.setLayerType(View.LAYER_TYPE_HARDWARE,null);
        v.setAlpha(floatAlpha()/100f);
        v.setVisibility(View.GONE);
        return v;
    }

    /** Gives a view the pack's data and art (read when first needed, for every view that asks). */
    private void ensureData(AtlasView v) {
        if (model!=null) { if (v.model()!=model) v.setModel(model,model.error); } else loadModel();
        if (artLoaded) { if (!v.artSet()) giveArt(v); } else loadArt();
    }
    private void giveArt(AtlasView v) { v.setBodyFace(bodyFace); v.setArt(background,parchment,icons); }

    private void loadModel() {
        if (modelLoading) return;
        modelLoading=true;
        jobs.execute(() -> {
            final AtlasModel m=model(); // blocks
            main.post(() -> {
                modelLoading=false; if (destroyed) return;
                if (view!=null) view.setModel(m,m.error);
                if (mini!=null) mini.setModel(m,m.error);
            });
        });
    }
    /** The pack's data, read when first needed. Blocks; any thread. */
    synchronized AtlasModel model() {
        if (model==null) model=AtlasModel.load(AtlasPack.dir(base));
        return model;
    }

    private void loadArt() {
        if (artLoading) return;
        artLoading=true;
        jobs.execute(() -> {
            File art=new File(AtlasPack.dir(base),"art");
            Bitmap bg=decode(new File(art,"background.png")), pc=decode(new File(art,"parchment.png")), ic=decode(new File(art,"icons.png"));
            Typeface face=null;
            File ttf=new File(art,"fallback.ttf");
            if (ttf.isFile()) { try { face=Typeface.createFromFile(ttf); } catch (RuntimeException e) { android.util.Log.w("Bloodborne","atlas font: "+e); } }
            final Typeface f=face;
            main.post(() -> {
                artLoading=false; if (destroyed) return;
                background=bg; parchment=pc; icons=ic; if (f!=null) bodyFace=f; artLoaded=true;
                if (view!=null) giveArt(view);
                if (mini!=null) giveArt(mini);
            });
        });
    }
    private static Bitmap decode(File f) {
        try { return f.isFile() ? BitmapFactory.decodeFile(f.getPath()) : null; }
        catch (Throwable e) { android.util.Log.w("Bloodborne","atlas art "+f.getName()+": "+e); return null; }
    }

    private boolean needTick() {
        return !destroyed && (secondUp() || (mini!=null && resumed && wanted() && floatMode()));
    }
    private boolean secondUp() { return (presentation!=null || atlasActivity!=null) && view!=null; }
    private void ensureTick() { if (!ticking && needTick()) { ticking=true; main.post(tick); } }

    /** ~30 Hz while a view is up: reads the game's state once; the second screen's view is told at every change,
     *  the floating one by driveMini (at most ~15 Hz, only when the player or the flags changed). */
    private final Runnable tick=new Runnable() {
        @Override public void run() {
            if (!needTick()) { ticking=false; return; }
            long now=android.os.SystemClock.uptimeMillis();
            boolean polled=state.poll();
            if (polled && secondUp()) view.onState();
            if (now-lastMainCheck>500) { // the game's activity moved to another display without a configuration change
                lastMainCheck=now; int id=mainDisplayId();
                if (lastMainId!=-2 && id!=lastMainId) { lastMainId=id; displaysChanged(); } else lastMainId=id;
            }
            if (mini!=null && resumed && wanted() && floatMode()) driveMini(now);
            if (now-lastPrefs>1000) { lastPrefs=now; applyOptions(); applyFloat(); }
            main.postDelayed(this,33);
        }
    };

    private void driveMini(long now) {
        boolean validNow=state.mapped && state.valid;
        if (validNow) everValid=true;
        if (miniChanged()) miniDirty=true;
        if (miniDirty && now-lastMini>=MINI_INTERVAL_MS) {
            lastMini=now; miniDirty=false;
            sMapped=state.mapped; sValid=state.valid; sx=state.x; sy=state.y; sz=state.z; sYaw=state.yaw; sBlock=state.block; sFlags=state.flagsVersion;
            mini.onState();
            if (validNow) miniFed=true;
        }
        updateMini();
    }
    /** Something the mini map shows moved: validity, the block, the flags, the place (a tenth of a unit) or the facing. */
    private boolean miniChanged() {
        return state.mapped!=sMapped || state.valid!=sValid || state.block!=sBlock || state.flagsVersion!=sFlags
            || Math.abs(state.x-sx)>.1f || Math.abs(state.y-sy)>.1f || Math.abs(state.z-sz)>.1f || Math.abs(state.yaw-sYaw)>.01f;
    }

    /** The floating map is visible while the map is on in a floating mode, the activity is up, the game is not paused, the
     *  editor is closed and the game has reported a valid place since it started (a later loading keeps the last frame). */
    private void updateMini() {
        if (mini==null) return;
        boolean show=!destroyed && resumed && wanted() && floatMode() && !gamePaused && !suppressed && everValid && miniFed;
        int visibility=show ? View.VISIBLE : View.GONE;
        if (mini.getVisibility()!=visibility) mini.setVisibility(visibility);
    }

    // ---- the game's process ----

    /** Before the game starts (any thread): when the map is on, writes atlas.flags (every flag the pack's features
     *  need) and adds BB_ATLAS_FILE and BB_ATLAS_FLAGS_FILE to the game's environment. */
    void prepareLaunch(List<String> extra) {
        launchedWithAtlas=false;
        main.post(() -> { state.reset(); everValid=false; miniFed=false; miniDirty=false; sMapped=true; updateMini(); });
        if (!wanted()) return;
        try {
            AtlasModel m=model();
            final int gen=(int)(System.currentTimeMillis()/1000L);
            final List<Integer> ids=AtlasState.limited(m.features.flagIds);
            File shm=new File(base,"atlas.shm"), flags=new File(base,"atlas.flags");
            shm.delete();
            AtlasState.writeFlags(flags,gen,ids);
            extra.add("BB_ATLAS_FILE="+shm.getPath());
            extra.add("BB_ATLAS_FLAGS_FILE="+flags.getPath());
            launchedWithAtlas=true;
            main.post(() -> state.expect(gen,new ArrayList<>(ids)));
        } catch (Exception e) { android.util.Log.w("Bloodborne","atlas launch: "+e); }
    }

    // ---- the window ----

    /** The Presentation: a window on the second display that never takes focus and keeps the screen on. It asks for the
     *  display's largest listed mode (its native resolution; the refresh rate of the current mode, else up to 60 Hz),
     *  never one the display did not list, and its view is laid out from this presentation's own context (the
     *  display's metrics) and its real pixel size. */
    static final class AtlasPresentation extends Presentation {
        private final AtlasScreen owner;
        AtlasPresentation(Activity activity,Display display,AtlasScreen owner) { super(activity,display); this.owner=owner; }
        @Override protected void onCreate(Bundle saved) {
            super.onCreate(saved);
            Window w=getWindow();
            w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED);
            w.setBackgroundDrawable(new ColorDrawable(Color.rgb(24,22,19)));
            setCanceledOnTouchOutside(false);
            setContentView(owner.makeView(getContext()));
            w.setLayout(WindowManager.LayoutParams.MATCH_PARENT,WindowManager.LayoutParams.MATCH_PARENT);
            refreshMode();
        }
        /** Asks for the display's native mode (also when the display's modes changed). */
        void refreshMode() { applyNativeMode(getDisplay(),getWindow()); }
        /** The window asks for the display's native mode (a presentation's window or an AtlasActivity's). */
        static void applyNativeMode(Display d,Window w) {
            try {
                if (d==null || w==null) return;
                Display.Mode best=bestMode(d);
                if (best==null) return;
                WindowManager.LayoutParams lp=w.getAttributes();
                if (lp.preferredDisplayModeId==best.getModeId()) return;
                lp.preferredDisplayModeId=best.getModeId();
                w.setAttributes(lp);
            } catch (RuntimeException e) { android.util.Log.w("Bloodborne","atlas display mode: "+e); }
        }
        /** The listed mode with the most pixels; among those the current refresh rate, else the highest up to 60 Hz. */
        static Display.Mode bestMode(Display d) {
            Display.Mode[] modes=d.getSupportedModes(); Display.Mode current=d.getMode(), best=null;
            if (modes==null) return null;
            for (Display.Mode m:modes) {
                if (best==null) { best=m; continue; }
                long a=(long)m.getPhysicalWidth()*m.getPhysicalHeight(), b=(long)best.getPhysicalWidth()*best.getPhysicalHeight();
                if (a!=b) { if (a>b) best=m; continue; }
                int ra=rank(m,current), rb=rank(best,current);
                if (ra>rb || (ra==rb && m.getRefreshRate()>best.getRefreshRate())) best=m;
            }
            return best;
        }
        private static int rank(Display.Mode m,Display.Mode current) {
            if (current!=null && Math.abs(m.getRefreshRate()-current.getRefreshRate())<1f) return 2;
            return m.getRefreshRate()<=60.5f ? 1 : 0;
        }
    }
}
