package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Bloodborne-only Android player. Keeps the game surface alive beneath its sliding drawer. */
public final class MainActivity extends Activity {
    private static final String FILES="/data/data/ro.cobrabm.fexdroid/files";
    private static final String ROOT=FILES+"/rootfs", BASE=FILES+"/bbport";
    private final Handler main=new Handler(Looper.getMainLooper());
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private FrameLayout screen;
    private SurfaceView surface;
    private View scrim, handle;
    private LinearLayout drawer;
    private TextView status;
    private Button resume;
    private PadBridge pad;
    private volatile Process xvfb, game;
    private volatile int shmid=-1;
    private volatile boolean destroyed;
    private boolean drawerOpen, displayOnly, startingX, hadError, swipeConsumed;
    private float touchX, touchY;
    private boolean edgeGesture;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT>=28) {
            WindowManager.LayoutParams attrs=getWindow().getAttributes();
            attrs.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attrs);
        }
        displayOnly="x".equals(getIntent().getStringExtra("action"));
        pad=new PadBridge(new File(BASE,"android-pad.state"));
        buildScreen();
        immersive();
    }

    private int dp(float n) { return Math.round(n*getResources().getDisplayMetrics().density); }
    private TextView text(String value, int size, int color) {
        TextView t=new TextView(this); t.setText(value); t.setTextSize(size); t.setTextColor(color); return t;
    }
    private FrameLayout.LayoutParams frame(int width,int height,int gravity) {
        return new FrameLayout.LayoutParams(width,height,gravity);
    }

    private void buildScreen() {
        screen=new FrameLayout(this); screen.setBackgroundColor(Color.BLACK);
        surface=new SurfaceView(this);
        screen.addView(surface,frame(-1,-1,Gravity.CENTER));
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) {
                if (shmid>=0) attach(h);
                else if (!startingX) { startingX=true; worker.execute(() -> startX(h)); }
            }
            @Override public void surfaceChanged(SurfaceHolder h,int format,int w,int height) {}
            @Override public void surfaceDestroyed(SurfaceHolder h) { DisplayBridge.stop(); }
        });
        surface.setOnTouchListener((v,e) -> {
            if (shmid<0 || drawerOpen) return true;
            XInput.moveTo(Math.round(e.getX()*1280/Math.max(1,v.getWidth())),
                          Math.round(e.getY()*720/Math.max(1,v.getHeight())));
            if (e.getActionMasked()==MotionEvent.ACTION_DOWN) XInput.button(1,true);
            if (e.getActionMasked()==MotionEvent.ACTION_UP || e.getActionMasked()==MotionEvent.ACTION_CANCEL)
                XInput.button(1,false);
            return true;
        });
        status=text("Starting Bloodborne…",18,0xffc6ad76); status.setGravity(Gravity.CENTER);
        screen.addView(status,frame(-1,-1,Gravity.CENTER));
        status.setClickable(false);

        handle=new View(this);
        GradientDrawable grip=new GradientDrawable(); grip.setColor(0x80c6ad76); grip.setCornerRadius(dp(4));
        handle.setBackground(grip); handle.setContentDescription("Open game menu");
        screen.addView(handle,frame(dp(4),dp(44),Gravity.LEFT|Gravity.CENTER_VERTICAL));
        handle.setOnClickListener(v -> openDrawer());

        scrim=new View(this); scrim.setBackgroundColor(0x99000000);
        scrim.setVisibility(View.GONE); scrim.setOnClickListener(v -> closeDrawer());
        screen.addView(scrim,frame(-1,-1,Gravity.FILL));

        drawer=new LinearLayout(this); drawer.setOrientation(LinearLayout.VERTICAL);
        drawer.setPadding(dp(24),dp(18),dp(24),dp(16)); drawer.setBackgroundColor(0xff15171d);
        drawer.setElevation(dp(16)); drawer.setVisibility(View.GONE);
        TextView title=text("BLOODBORNE",25,0xffe8dfc9); title.setTypeface(Typeface.SERIF,Typeface.BOLD);
        drawer.addView(title,new LinearLayout.LayoutParams(-1,dp(44)));
        TextView hint=text("Swipe left or press A to return",12,0xffa6a7ac);
        drawer.addView(hint,new LinearLayout.LayoutParams(-1,dp(30)));
        resume=menuButton("Resume game",() -> closeDrawer());
        menuButton("Graphics settings",() -> {
            closeDrawer();
            if (shmid>=0) { XInput.key(118,true); main.postDelayed(() -> XInput.key(118,false),100); }
        });
        menuButton("Restart game",() -> new AlertDialog.Builder(this)
            .setTitle("Restart Bloodborne?").setMessage("Any unsaved progress will be lost.")
            .setNegativeButton("Cancel",null).setPositiveButton("Restart",(dialog,which) -> {
                closeDrawer(); status.setText("Restarting Bloodborne…"); status.setVisibility(View.VISIBLE);
                worker.execute(() -> { stopGame(); startGame(); });
            }).show());
        menuButton("Quit game",() -> new AlertDialog.Builder(this)
            .setTitle("Quit Bloodborne?").setMessage("Any unsaved progress will be lost.")
            .setNegativeButton("Cancel",null).setPositiveButton("Quit",(dialog,which) -> finish()).show());
        TextView controls=text("B  Confirm     A  Cancel\nY  Item             X  Heal",13,0xffc6ad76);
        controls.setPadding(0,dp(16),0,0); drawer.addView(controls);
        screen.addView(drawer,frame(dp(300),-1,Gravity.LEFT));
        setContentView(screen);
    }

    private Button menuButton(String title,Runnable action) {
        Button b=new Button(this); b.setText(title); b.setAllCaps(false);
        b.setTextSize(16); b.setTextColor(0xffe8dfc9); b.setGravity(Gravity.LEFT|Gravity.CENTER_VERTICAL);
        b.setPadding(dp(14),0,dp(14),0); b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,dp(46)); lp.bottomMargin=dp(4);
        drawer.addView(b,lp); return b;
    }

    private void openDrawer() {
        if (drawerOpen) return;
        drawerOpen=true; pad.reset(); XInput.button(1,false);
        handle.setVisibility(View.GONE);
        scrim.animate().cancel(); drawer.animate().cancel();
        scrim.setAlpha(0); scrim.setVisibility(View.VISIBLE); scrim.animate().alpha(1).setDuration(200).start();
        drawer.setTranslationX(-dp(300)); drawer.setVisibility(View.VISIBLE);
        drawer.animate().translationX(0).setDuration(220).start(); resume.requestFocus();
    }
    private void closeDrawer() {
        if (!drawerOpen) return;
        drawerOpen=false; pad.reset();
        scrim.animate().cancel(); drawer.animate().cancel();
        scrim.animate().alpha(0).setDuration(180).withEndAction(() -> scrim.setVisibility(View.GONE)).start();
        drawer.animate().translationX(-dp(300)).setDuration(200)
            .withEndAction(() -> { drawer.setVisibility(View.GONE); handle.setVisibility(View.VISIBLE); surface.requestFocus(); }).start();
        immersive();
    }

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
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) immersive(); else if (pad!=null) pad.reset();
    }
    @Override public void onBackPressed() { if (drawerOpen) closeDrawer(); else openDrawer(); }

    @Override public boolean dispatchTouchEvent(MotionEvent e) {
        if (e.getActionMasked()==MotionEvent.ACTION_DOWN) {
            swipeConsumed=false;
            touchX=e.getX(); touchY=e.getY();
            edgeGesture=!drawerOpen && touchX<dp(28);
        }
        if (swipeConsumed) return true;
        if (e.getActionMasked()==MotionEvent.ACTION_MOVE) {
            float dx=e.getX()-touchX, dy=e.getY()-touchY;
            if (edgeGesture && dx>dp(40) && Math.abs(dy)<dp(90)) {
                openDrawer(); edgeGesture=false; swipeConsumed=true; return true;
            }
            if (drawerOpen && dx< -dp(70) && Math.abs(dy)<dp(90)) {
                MotionEvent cancel=MotionEvent.obtain(e); cancel.setAction(MotionEvent.ACTION_CANCEL);
                super.dispatchTouchEvent(cancel); cancel.recycle();
                closeDrawer(); swipeConsumed=true; return true;
            }
        }
        if (edgeGesture) {
            if (e.getActionMasked()==MotionEvent.ACTION_UP) { edgeGesture=false; openDrawer(); }
            return true;
        }
        return super.dispatchTouchEvent(e);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int code=e.getKeyCode();
        if (code==KeyEvent.KEYCODE_BACK || code==KeyEvent.KEYCODE_BUTTON_MODE) {
            if (e.getAction()==KeyEvent.ACTION_UP) onBackPressed(); return true;
        }
        if (drawerOpen) {
            if (code==KeyEvent.KEYCODE_BUTTON_B || e.getScanCode()==305) {
                if (e.getAction()==KeyEvent.ACTION_UP) closeDrawer(); return true;
            }
            if (code==KeyEvent.KEYCODE_BUTTON_A || e.getScanCode()==304) {
                if (e.getAction()==KeyEvent.ACTION_UP && getCurrentFocus()!=null) getCurrentFocus().performClick();
                return true;
            }
            return super.dispatchKeyEvent(e);
        }
        if (pad.key(e)) return true;
        if (shmid>=0 && e.getScanCode()>0 && e.getScanCode()+8<=255) {
            XInput.key(e.getScanCode()+8,e.getAction()==KeyEvent.ACTION_DOWN); return true;
        }
        return super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (drawerOpen && e.isFromSource(android.view.InputDevice.SOURCE_JOYSTICK)) return true;
        return pad.motion(e) || super.dispatchGenericMotionEvent(e);
    }
    @Override protected void onPause() { if (pad!=null) pad.reset(); super.onPause(); }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        // Deterministic UI checks over ADB, using the same handlers as the actual controls.
        String action=intent.getStringExtra("ui_action");
        if ("open-menu".equals(action)) openDrawer();
        if ("close-menu".equals(action)) closeDrawer();
    }

    private Process process(String... args) throws java.io.IOException {
        ProcessBuilder b=new ProcessBuilder(args).redirectErrorStream(true);
        Map<String,String> env=b.environment(); env.clear();
        env.put("PATH",ROOT+"/usr/bin:"+ROOT+"/bin:/system/bin");
        env.put("HOME",FILES+"/home/bbport"); env.put("LANG","C.UTF-8");
        env.put("TMPDIR",ROOT+"/tmp"); env.put("XDG_RUNTIME_DIR",ROOT+"/tmp");
        env.put("FXD_FILES",FILES); env.put("FXD_ROOT",ROOT); env.put("BB_ANDROID_INPUT","1");
        if (args[0].endsWith("/Xvfb")) env.put("LD_PRELOAD",ROOT+"/usr/lib/fexdroid/libfxpath.so");
        env.put("LIBGL_DRIVERS_PATH",ROOT+"/usr/lib/aarch64-linux-gnu/dri");
        return b.start();
    }
    private void startX(SurfaceHolder holder) {
        try {
            if (!new File(ROOT,"usr/bin/Xvfb").canExecute() || !new File(BASE,"game/eboot.bin").isFile())
                throw new java.io.IOException("Bloodborne files are missing. Restore the installed game bundle.");
            new File(FILES,"home/bbport").mkdirs();
            try (PrintWriter passwd=new PrintWriter(new File(ROOT,"etc/passwd"));
                 PrintWriter group=new PrintWriter(new File(ROOT,"etc/group"))) {
                int uid=android.os.Process.myUid();
                passwd.println("root:x:0:0:root:/root:/bin/sh");
                passwd.println("user:x:"+uid+":"+uid+":Bloodborne:"+FILES+"/home/bbport:"+ROOT+"/bin/sh");
                group.println("root:x:0:"); group.println("user:x:"+uid+":");
            }
            new File(ROOT,"tmp/.X0-lock").delete(); new File(ROOT,"tmp/.X11-unix/X0").delete();
            xvfb=process(ROOT+"/usr/bin/Xvfb",":0","-screen","0","1280x720x24",
                "-shmem","-ac","-nolisten","tcp","-pn");
            Pattern ready=Pattern.compile("screen 0 shmid (\\d+)");
            BufferedReader r=new BufferedReader(new InputStreamReader(xvfb.getInputStream()));
            {
                String line;
                while ((line=r.readLine())!=null && !destroyed) {
                    android.util.Log.i("Bloodborne-X11",line);
                    Matcher m=ready.matcher(line);
                    if (m.find()) {
                        shmid=Integer.parseInt(m.group(1));
                        main.post(() -> attach(holder));
                        break;
                    }
                }
                if (shmid<0) throw new java.io.IOException("The game display could not start.");
                Thread drain=new Thread(() -> { try (BufferedReader reader=r) { while (reader.readLine()!=null) {} }
                    catch (Exception ignored) {} },"X11-log");
                drain.setDaemon(true); drain.start();
            }
        } catch (Exception ex) { error(ex.toString()); }
    }
    private void attach(SurfaceHolder holder) {
        if (destroyed || !holder.getSurface().isValid()) return;
        String result=DisplayBridge.start(holder.getSurface(),ROOT+"/tmp/.fxshm/sock",shmid,60,ROOT+"/tmp/fxpresent.sock");
        android.util.Log.i("Bloodborne",result);
        if (!result.startsWith("bridge running:")) { error(result); return; }
        XInput.connect(0);
        if (!displayOnly && game==null) worker.execute(() -> startGame());
        pollFrames();
    }
    private void pollFrames() {
        if (destroyed) return;
        if (!hadError && DisplayBridge.directFrames()>0) status.setVisibility(View.GONE);
        main.postDelayed(this::pollFrames,1000);
    }
    private void startGame() {
        if (destroyed) return;
        hadError=false;
        try {
            Process launched=process(ROOT+"/bin/sh",BASE+"/run-thor.sh","game"); game=launched;
            Thread log=new Thread(() -> {
                try (BufferedReader r=new BufferedReader(new InputStreamReader(launched.getInputStream()));
                     PrintWriter out=new PrintWriter(new File(BASE,"logs/android-game.log"))) {
                    String line; while ((line=r.readLine())!=null) { out.println(line); out.flush(); }
                    int code=launched.waitFor();
                    if (game==launched) { game=null; error("Game stopped ("+code+"). Open the menu to restart."); }
                } catch (Exception ex) { if (!destroyed) error(ex.toString()); }
            },"Bloodborne-log"); log.setDaemon(true); log.start();
        } catch (Exception ex) { error(ex.toString()); }
    }
    private void error(String message) {
        hadError=true;
        android.util.Log.e("Bloodborne",message);
        main.post(() -> { if (!destroyed) { status.setText(message); status.setVisibility(View.VISIBLE); } });
    }
    private void stopGame() {
        Process p=game; game=null;
        if (p!=null) { p.destroyForcibly(); try { p.waitFor(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } }
    }
    @Override protected void onDestroy() {
        destroyed=true; pad.reset(); main.removeCallbacksAndMessages(null);
        DisplayBridge.stop(); stopGame(); if (xvfb!=null) xvfb.destroyForcibly();
        worker.shutdownNow(); super.onDestroy();
    }
}
