package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.os.Build;
import android.os.Bundle;
import android.view.Display;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/** Hunter's Atlas on a second display that takes no Presentation window (a display without FLAG_PRESENTATION, such as
 *  the built-in screen of a dual-screen handheld when the game runs on the other one): the map as an activity launched
 *  on that display by AtlasScreen. Its window is not focusable and touch-modal-free, so the gamepad and keys stay with
 *  the game's activity; Android makes this activity the top-resumed one when it starts, so it hands the top back to
 *  the game's task at once (AtlasScreen.atlasGotTop) and MainActivity does not pause the game for that
 *  (AtlasScreen.ownWindowHasTop). AtlasScreen opens and closes it; it never finishes itself except when it is not
 *  wanted where it landed. Nothing in it takes focus. */
public final class AtlasActivity extends Activity {
    /** The owner that launched this activity (same process; null after a process restart: the activity closes). */
    static volatile AtlasScreen owner;
    /** This activity is the system's top-resumed one right now. */
    static volatile boolean topResumed;

    private AtlasScreen screen;

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        Window w=getWindow();
        w.addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
            | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED);
        w.setBackgroundDrawable(new ColorDrawable(Color.rgb(24,22,19)));
        if (Build.VERSION.SDK_INT>=28) {
            WindowManager.LayoutParams lp=w.getAttributes();
            lp.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            w.setAttributes(lp);
        }
        immersive();
        screen=owner;
        if (screen==null || !screen.attach(this)) { screen=null; finishAndRemoveTask(); return; }
        setContentView(screen.makeView(this));
        w.setLayout(WindowManager.LayoutParams.MATCH_PARENT,WindowManager.LayoutParams.MATCH_PARENT);
        refreshMode();
    }

    /** Hides the system bars of this display (best effort: the window never has focus). */
    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN |
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
            View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        if (Build.VERSION.SDK_INT>=30) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController c=getWindow().getInsetsController();
            if (c!=null) {
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                c.hide(WindowInsets.Type.systemBars());
            }
        }
    }

    /** Asks for the display's native mode, as the presentation does. */
    void refreshMode() {
        Display d;
        try { d=getWindowManager().getDefaultDisplay(); } catch (RuntimeException e) { return; }
        AtlasScreen.AtlasPresentation.applyNativeMode(d,getWindow());
    }

    @Override protected void onResume() {
        super.onResume();
        // Android 10+ reports the top in onTopResumedActivityChanged; before it, being resumed is all there is.
        if (Build.VERSION.SDK_INT<29 && screen!=null) screen.atlasGotTop();
    }
    @Override public void onTopResumedActivityChanged(boolean top) {
        super.onTopResumedActivityChanged(top);
        topResumed=top;
        AtlasScreen s=screen;
        if (s==null) return;
        if (top) s.atlasGotTop(); else s.atlasLostTop();
    }
    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        immersive(); refreshMode();
        if (screen!=null) screen.displaysChanged();
    }
    /** Closed only by AtlasScreen. */
    @Override public void onBackPressed() { }
    @Override protected void onDestroy() {
        topResumed=false;
        AtlasScreen s=screen; screen=null;
        if (s!=null) { s.detached(this); s.atlasLostTop(); }
        super.onDestroy();
    }
}
