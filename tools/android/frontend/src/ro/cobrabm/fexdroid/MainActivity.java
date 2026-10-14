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
import android.text.InputFilter;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
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
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Bloodborne-only Android player. Keeps the game surface alive beneath its sliding drawer. */
public final class MainActivity extends Activity {
    /** The app's files (by package: com.ducvd89.bloodborne), its Linux rootfs and the bbport runtime.
     *  The rootfs programs name its absolute path (their interpreter), set when it was installed. */
    private String FILES, ROOT, BASE;
    /** The game (e.g. CUSA03173, with eboot.bin) in shared storage, chosen by the player. */
    private String gameDir;
    private static final int REQUEST_FOLDER=1, REQUEST_STORAGE=2, REQUEST_DRIVER=3, REQUEST_SAVES=9; // 4-6: ModsDialog, 7-8: GameInstaller
    private SurfaceHolder pendingHolder;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final ExecutorService worker=Executors.newSingleThreadExecutor();
    private FrameLayout screen;
    private SurfaceView surface;
    private View scrim, handle;
    private LinearLayout drawer;
    private LinearLayout drawerItems;
    private TextView status;
    private View resume;
    private LinearLayout gameSettingsItem, cheatsItem, modsItem, graphicsItem, losslessItem, steamItem, driverItem, controllerItem, folderItem;
    private TouchControllerOverlay controllerOverlay;
    private PadBridge pad;
    private AudioBridge audio;
    private volatile Process xvfb, game;
    private volatile int shmid=-1;
    private volatile boolean destroyed;
    /** Vulkan drivers installed from zips (the GPU driver menu); whether the running game uses one. */
    private DriverManager drivers;
    private volatile boolean customDriver;
    private DriverDownloadDialog driverDownload;
    private ModsDialog mods;
    private GameInstaller installer;
    private AppUpdater updater;
    private LinearLayout updateItem;
    private volatile long gameStartedAt;
    private boolean drawerOpen, displayOnly, startingX, hadError, swipeConsumed;
    private float touchX, touchY;
    private boolean edgeGesture, imeOpen, imePolling;
    private final AtomicBoolean imeAnswered=new AtomicBoolean();

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        FILES="/data/data/"+getPackageName()+"/files";
        ROOT=FILES+"/rootfs"; BASE=FILES+"/bbport";
        drivers=new DriverManager(new File(FILES));
        GameInstaller.Host host=modsHost();
        mods=new ModsDialog(this,host); installer=new GameInstaller(this,host);
        updater=new AppUpdater(this,new AppUpdater.Host() {
            @Override public void stopGame() { MainActivity.this.stopGame(); }
            @Override public void restartGame() { if (!destroyed && shmid>=0) MainActivity.this.restartGame(); }
            @Override public void detail(String text) { setDetail(updateItem,text); }
            @Override public void closed() { if (!destroyed) immersive(); }
            @Override public boolean destroyed() { return destroyed; }
        });
        worker.execute(updater::cleanup); // downloads an earlier run left
        gameDir=getSharedPreferences("bloodborne",MODE_PRIVATE).getString("game_dir",null);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT>=28) {
            WindowManager.LayoutParams attrs=getWindow().getAttributes();
            attrs.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attrs);
        }
        displayOnly="x".equals(getIntent().getStringExtra("action"));
        pad=new PadBridge(new File(BASE,"android-pad.state"));
        audio=new AudioBridge(new File(BASE,"audio.fifo")); audio.start();
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
    private void fitSurface(int width,int height) {
        if (width<=0 || height<=0) return;
        int w=width, h=height;
        if ((long)w*9>(long)h*16) w=h*16/9; else h=w*9/16;
        FrameLayout.LayoutParams p=(FrameLayout.LayoutParams)surface.getLayoutParams();
        if (p.width==w && p.height==h) return;
        p.width=w; p.height=h;
        // Not during the layout pass that reported the size.
        surface.post(() -> surface.setLayoutParams(p));
    }

    private void buildScreen() {
        screen=new FrameLayout(this); screen.setBackgroundColor(Color.BLACK);
        surface=new SurfaceView(this);
        screen.addView(surface,frame(-1,-1,Gravity.CENTER));
        // The game is 16:9: the largest 16:9 area of the screen, black bars around it (a foldable's
        // near-square inner screen, a tall cover screen). Refitted when folding or rotating.
        screen.addOnLayoutChangeListener((v,l,t,r,b,ol,ot,or,ob) -> fitSurface(r-l,b-t));
        surface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) {
                if (shmid>=0) attach(h);
                else if (!hasGame()) { pendingHolder=h; if (!installer.active() && !folderDialogShowing()) askGameFolder(false); }
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

        controllerOverlay=new TouchControllerOverlay(this,pad);
        controllerOverlay.setVisibility(controllerEnabled() ? View.VISIBLE : View.GONE);
        screen.addView(controllerOverlay,frame(-1,-1,Gravity.CENTER));

        handle=new View(this);
        GradientDrawable grip=new GradientDrawable(); grip.setColor(0x80c6ad76); grip.setCornerRadius(dp(4));
        handle.setBackground(grip); handle.setContentDescription("Open game menu");
        screen.addView(handle,frame(dp(4),dp(44),Gravity.LEFT|Gravity.CENTER_VERTICAL));
        handle.setOnClickListener(v -> openDrawer());

        scrim=new View(this); scrim.setBackgroundColor(0x99000000);
        scrim.setVisibility(View.GONE); scrim.setOnClickListener(v -> closeDrawer());
        screen.addView(scrim,frame(-1,-1,Gravity.FILL));

        drawer=new LinearLayout(this); drawer.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable panel=new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
            new int[]{0xff1b1d24,0xff121318});
        drawer.setBackground(panel);
        drawer.setElevation(dp(16)); drawer.setVisibility(View.GONE);
        drawerItems=new LinearLayout(this); drawerItems.setOrientation(LinearLayout.VERTICAL);
        drawerItems.setPadding(dp(14),dp(20),dp(14),dp(16));
        android.widget.ScrollView drawerScroll=new android.widget.ScrollView(this);
        drawerScroll.setFillViewport(false);
        drawerScroll.addView(drawerItems);
        // A thin gold edge on the game side.
        LinearLayout body=new LinearLayout(this);
        body.addView(drawerScroll,new LinearLayout.LayoutParams(0,-1,1));
        View edge=new View(this); edge.setBackgroundColor(0x55c6ad76);
        body.addView(edge,new LinearLayout.LayoutParams(dp(1),-1));
        drawer.addView(body,new LinearLayout.LayoutParams(-1,0,1));

        TextView title=text("BLOODBORNE",24,0xffe8dfc9); title.setTypeface(Typeface.SERIF,Typeface.BOLD);
        title.setLetterSpacing(0.12f); title.setPadding(dp(10),0,0,0);
        drawerItems.addView(title,new LinearLayout.LayoutParams(-1,-2));
        TextView hint=text("Swipe left or press A to return",12,0xff8e9097);
        hint.setPadding(dp(10),dp(2),0,dp(6));
        drawerItems.addView(hint,new LinearLayout.LayoutParams(-1,-2));

        section("Game");
        resume=menuItem("ic_play","Resume game",null,() -> closeDrawer());
        gameSettingsItem=menuItem("ic_settings","Game settings","",() -> {
            closeDrawer();
            new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame).showGame();
        });
        cheatsItem=menuItem("ic_cheats","Cheats","",() -> {
            closeDrawer();
            new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame).showCheats();
        });
        modsItem=menuItem("ic_extension","Mods","",() -> { closeDrawer(); mods.show(); });
        menuItem("ic_refresh","Restart game","Unsaved progress is lost",() -> new AlertDialog.Builder(this)
            .setTitle("Restart Bloodborne?").setMessage("Any unsaved progress will be lost.")
            .setNegativeButton("Cancel",null).setPositiveButton("Restart",(dialog,which) -> restartGame()).show());

        section("Graphics");
        graphicsItem=menuItem("ic_tune","Graphics settings","",() -> {
            closeDrawer();
            new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame).show();
        });
        losslessItem=menuItem("ic_frames","Lossless Scaling","",() -> {
            closeDrawer();
            new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame)
                .showLossless(() -> { Process p=game; return p!=null && p.isAlive(); });
        });
        steamItem=menuItem("ic_download","Get Lossless.dll from Steam","",() -> {
            closeDrawer();
            new SteamDllDialog(this,new File(BASE,"Lossless.dll")).show();
        });
        driverItem=menuItem("ic_memory","GPU driver","",() -> { closeDrawer(); showDrivers(); });

        section("Controls");
        controllerItem=menuItem("ic_gamepad","On-screen controller","",() -> {
            boolean enabled=!controllerEnabled();
            getSharedPreferences("bloodborne",MODE_PRIVATE).edit().putBoolean("controller_overlay",enabled).apply();
            controllerOverlay.release();
            closeDrawer();
        });
        menuItem("ic_edit","Edit controller layout","Drag and resize the buttons",() -> {
            getSharedPreferences("bloodborne",MODE_PRIVATE).edit().putBoolean("controller_overlay",true).apply();
            closeDrawer();
            controllerOverlay.edit(null);
        });
        menuItem("ic_restore","Reset controller layout",null,() -> {
            controllerOverlay.resetLayout();
            closeDrawer();
        });

        section("Storage");
        folderItem=menuItem("ic_folder","Game folder","",() -> { closeDrawer(); askGameFolder(true); });
        menuItem("ic_download","Back up saves","Copy them to the Download folder",() -> { closeDrawer(); backupSaves(); });
        menuItem("ic_restore","Restore saves","From a backup folder",() -> { closeDrawer(); askRestoreSaves(); });
        menuItem("ic_exit","Quit game",null,() -> new AlertDialog.Builder(this)
            .setTitle("Quit Bloodborne?").setMessage("Any unsaved progress will be lost.")
            .setNegativeButton("Cancel",null).setPositiveButton("Quit",(dialog,which) -> finish()).show());

        section("App");
        updateItem=menuItem("ic_refresh","Check for updates","Version "+updater.version(),() -> updater.check());

        // The version, pinned under the scrolling menu.
        String version="";
        try { version=" "+getPackageManager().getPackageInfo(getPackageName(),0).versionName; }
        catch (Exception ignored) {}
        TextView about=text("Bloodborne for Android"+version,12,0xff8e9097);
        about.setPadding(dp(24),dp(8),dp(14),dp(14));
        drawer.addView(about,new LinearLayout.LayoutParams(-1,-2));
        screen.addView(drawer,frame(dp(320),-1,Gravity.LEFT));
        setContentView(screen);
    }

    private void restartGame() {
        closeDrawer(); status.setText("Restarting Bloodborne…"); status.setVisibility(View.VISIBLE);
        worker.execute(() -> { stopGame(); startGame(); });
    }

    /** A small gold heading above a group of menu rows. */
    private void section(String name) {
        TextView t=text(name.toUpperCase(java.util.Locale.ROOT),11,0xffc6ad76);
        t.setLetterSpacing(0.15f); t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setPadding(dp(10),dp(16),0,dp(6));
        drawerItems.addView(t,new LinearLayout.LayoutParams(-1,-2));
    }

    /** A menu row: icon, title and an optional second line (null: none; "" filled by refreshDrawer).
     *  Focusable for the gamepad, with a gold highlight when focused or pressed. */
    private LinearLayout menuItem(String icon,String title,String detail,Runnable action) {
        LinearLayout row=new LinearLayout(this); row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL); row.setPadding(dp(10),dp(8),dp(10),dp(8));
        row.setFocusable(true); row.setClickable(true); row.setOnClickListener(v -> action.run());
        android.graphics.drawable.StateListDrawable states=new android.graphics.drawable.StateListDrawable();
        GradientDrawable lit=new GradientDrawable(); lit.setColor(0x33c6ad76); lit.setCornerRadius(dp(10));
        lit.setStroke(dp(1),0x99c6ad76);
        GradientDrawable pressed=new GradientDrawable(); pressed.setColor(0x44c6ad76); pressed.setCornerRadius(dp(10));
        states.addState(new int[]{android.R.attr.state_pressed},pressed);
        states.addState(new int[]{android.R.attr.state_focused},lit);
        states.addState(new int[]{},new android.graphics.drawable.ColorDrawable(0));
        row.setBackground(states);

        android.widget.ImageView image=new android.widget.ImageView(this);
        int id=getResources().getIdentifier(icon,"drawable",getPackageName());
        if (id!=0) image.setImageResource(id);
        LinearLayout.LayoutParams ip=new LinearLayout.LayoutParams(dp(24),dp(24)); ip.rightMargin=dp(14);
        row.addView(image,ip);

        LinearLayout texts=new LinearLayout(this); texts.setOrientation(LinearLayout.VERTICAL);
        TextView name=text(title,15,0xffe8dfc9);
        texts.addView(name);
        if (detail!=null) {
            TextView second=text(detail,12,0xff8e9097); second.setTag("detail");
            texts.addView(second);
        }
        row.addView(texts,new LinearLayout.LayoutParams(0,-2,1));
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-1,-2); lp.bottomMargin=dp(2);
        row.setMinimumHeight(dp(48));
        drawerItems.addView(row,lp); return row;
    }
    private static void setDetail(LinearLayout row,String value) {
        TextView detail=row==null ? null : (TextView)row.findViewWithTag("detail");
        if (detail!=null) detail.setText(value);
    }
    /** Where run-thor.sh looks for Lossless.dll: next to the game folder, else the app's own copy. */
    private File losslessDll() {
        if (gameDir!=null) {
            File beside=new File(new File(gameDir).getParentFile(),"Lossless.dll");
            if (beside.isFile()) return beside;
        }
        File own=new File(BASE,"Lossless.dll");
        return own.isFile() ? own : null;
    }
    /** The rows' second lines: the current settings, read when the drawer opens. */
    private void refreshDrawer() {
        java.util.List<String> game=new java.util.ArrayList<>();
        if (!"0".equals(setting("skip_network_choice"))) game.add("Offline start");
        if ("1".equals(setting("skip_intro"))) game.add("no intros");
        if ("1".equals(setting("debug_camera"))) game.add("free camera");
        if ("1".equals(setting("debug_menu"))) game.add("debug menu");
        setDetail(gameSettingsItem,game.isEmpty() ? "Online/offline screen shown" : String.join(" · ",game));
        java.util.List<String> cheats=new java.util.ArrayList<>();
        if ("1".equals(setting("cheat_health"))) cheats.add("infinite health");
        if ("1".equals(setting("cheat_items"))) cheats.add("infinite items");
        setDetail(cheatsItem,cheats.isEmpty() ? "Off" : String.join(" · ",cheats));
        mods.summary(text -> setDetail(modsItem,text));
        String upscaler="off".equals(setting("upscaler")) ? "No upscaling" : "FSR 3.1";
        String limit=setting("fps_limit");
        setDetail(graphicsItem,upscaler+" · "+("0".equals(limit) ? "no FPS limit"
            : (limit==null || !limit.matches("40|45|60") ? "30" : limit)+" FPS limit"));
        String lsfg=setting("lsfg_multiplier"); boolean dll=losslessDll()!=null;
        boolean on="2".equals(lsfg) || "3".equals(lsfg) || "4".equals(lsfg);
        setDetail(losslessItem,!dll ? "Needs Lossless.dll" : on
            ? lsfg+"× frames · flow "+(setting("lsfg_flow_scale")==null ? "1.0" : setting("lsfg_flow_scale")) : "Off");
        File found=losslessDll();
        setDetail(steamItem,found==null ? "Sign in with your Steam account"
            : found.getParent().equals(BASE) ? "Downloaded from Steam" : "Using Lossless.dll next to the game folder");
        DriverManager.Driver driver=drivers.find(setting("gpu_driver"));
        setDetail(driverItem,driver==null ? "Built-in Turnip" : driver.title()+" · automatic choice off");
        setDetail(controllerItem,controllerEnabled() ? "On" : "Off");
        setDetail(folderItem,gameDir==null ? "Not chosen" : gameDir);
    }

    private void openDrawer() {
        if (drawerOpen) return;
        drawerOpen=true; controllerOverlay.release(); pad.reset(); XInput.button(1,false);
        refreshDrawer();
        controllerOverlay.setVisibility(View.GONE);
        handle.setVisibility(View.GONE);
        scrim.animate().cancel(); drawer.animate().cancel();
        scrim.setAlpha(0); scrim.setVisibility(View.VISIBLE); scrim.animate().alpha(1).setDuration(200).start();
        drawer.setTranslationX(-dp(320)); drawer.setVisibility(View.VISIBLE);
        drawer.animate().translationX(0).setDuration(220).start(); resume.requestFocus();
    }
    private void closeDrawer() {
        if (!drawerOpen) return;
        drawerOpen=false; pad.reset();
        controllerOverlay.setVisibility(controllerEnabled() ? View.VISIBLE : View.GONE);
        scrim.animate().cancel(); drawer.animate().cancel();
        scrim.animate().alpha(0).setDuration(180).withEndAction(() -> scrim.setVisibility(View.GONE)).start();
        drawer.animate().translationX(-dp(320)).setDuration(200)
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
    private boolean controllerEnabled() {
        return getSharedPreferences("bloodborne",MODE_PRIVATE).getBoolean("controller_overlay",false);
    }
    @Override protected void onPause() {
        if (controllerOverlay!=null) controllerOverlay.release();
        if (pad!=null) pad.reset();
        if (audio!=null) audio.pause();
        super.onPause();
    }
    @Override protected void onResume() {
        super.onResume(); if (audio!=null) audio.resume();
        if (updater!=null) updater.resumed(); // back from the "install unknown apps" setting
    }
    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        // Deterministic UI checks over ADB, using the same handlers as the actual controls.
        String action=intent.getStringExtra("ui_action");
        if ("open-menu".equals(action)) openDrawer();
        if ("close-menu".equals(action)) closeDrawer();
    }

    /** SIGTERM first: mods_android.py then stops the PKG tool it started and removes its staging
     *  folder (a SIGKILL would leave the tool extracting on its own); SIGKILL if it hangs. */
    private static void stopScript(Process p) {
        if (!p.isAlive()) return;
        p.destroy();
        try { if (!p.waitFor(5,java.util.concurrent.TimeUnit.SECONDS)) p.destroyForcibly(); }
        catch (InterruptedException e) { p.destroyForcibly(); Thread.currentThread().interrupt(); }
    }
    private Process process(String... args) throws java.io.IOException {
        ProcessBuilder b=new ProcessBuilder(args).redirectErrorStream(true);
        Map<String,String> env=b.environment(); env.clear();
        env.put("PATH",ROOT+"/usr/bin:"+ROOT+"/bin:/system/bin");
        env.put("HOME",FILES+"/home/bbport"); env.put("LANG","C.UTF-8");
        env.put("TMPDIR",ROOT+"/tmp"); env.put("XDG_RUNTIME_DIR",ROOT+"/tmp");
        env.put("FXD_FILES",FILES); env.put("FXD_ROOT",ROOT); env.put("BB_ANDROID_INPUT","1");
        // The rootfs's loader was built for the original fexdroid path: its libraries by name here.
        env.put("LD_LIBRARY_PATH",ROOT+"/usr/lib/aarch64-linux-gnu:"+ROOT+"/usr/lib");
        // Its libc emulates SysV shared memory through fxshmd (started below), whose built-in path
        // is also fexdroid's: the socket by name.
        env.put("FXD_SHM_SOCKET",ROOT+"/tmp/.fxshm/sock");
        if (gameDir!=null) env.put("BB_GAME_DIR",gameDir);
        if (args[0].endsWith("/Xvfb")) env.put("LD_PRELOAD",ROOT+"/usr/lib/fexdroid/libfxpath.so");
        env.put("LIBGL_DRIVERS_PATH",ROOT+"/usr/lib/aarch64-linux-gnu/dri");
        return b.start();
    }
    private void startX(SurfaceHolder holder) {
        try {
            RuntimeInstaller.ensure(this, new File(FILES), message -> main.post(() -> status.setText(message)));
            // The runtime brought bbport.default.ini: settings defaults apply before the game reads bbport.ini.
            try { SettingsDefaults.apply(new File(BASE)); main.post(() -> { if (drawerOpen) refreshDrawer(); }); }
            catch (Exception ex) { android.util.Log.e("Bloodborne","default settings: "+ex); }
            if (!new File(ROOT,"usr/bin/Xvfb").canExecute() || !new File(BASE,"run-thor.sh").isFile())
                throw new java.io.IOException("The Bloodborne runtime is missing. Install the runtime bundle.");
            new File(FILES,"home/bbport").mkdirs();
            try (PrintWriter passwd=new PrintWriter(new File(ROOT,"etc/passwd"));
                 PrintWriter group=new PrintWriter(new File(ROOT,"etc/group"))) {
                int uid=android.os.Process.myUid();
                passwd.println("root:x:0:0:root:/root:/bin/sh");
                passwd.println("user:x:"+uid+":"+uid+":Bloodborne:"+FILES+"/home/bbport:"+ROOT+"/bin/sh");
                group.println("root:x:0:"); group.println("user:x:"+uid+":");
            }
            new File(ROOT,"tmp/.X0-lock").delete(); new File(ROOT,"tmp/.X11-unix/X0").delete();
            // The shared memory daemon (Xvfb's frame, the display bridge): forks itself and stays.
            Process shmd=process(ROOT+"/usr/libexec/fxshmd","--dir",ROOT+"/tmp/.fxshm");
            shmd.waitFor();
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
        if (!imePolling) { imePolling=true; pollIme(); }
    }
    // ---- Game folder: the extracted game (any retail release with 1.09) in shared storage ----
    /** The retail releases' folder names (scripts/game_check.py SUPPORTED_TITLES): one 1.09 executable. */
    private static final String[] SERIALS={"CUSA03173","CUSA00900","CUSA00207","CUSA00208","CUSA00299",
        "CUSA01363","CUSA03179","CUSA03014","CUSA03023"};
    private static boolean isGame(File dir) { return dir!=null && new File(dir,"eboot.bin").isFile(); }
    private boolean hasGame() {
        if (Build.VERSION.SDK_INT>=23 && checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                !=android.content.pm.PackageManager.PERMISSION_GRANTED) return false;
        return gameDir!=null && isGame(new File(gameDir));
    }
    private AlertDialog folderDialog;
    private boolean folderDialogShowing() { return folderDialog!=null && folderDialog.isShowing(); }
    /** Explains, asks for storage access when needed, then opens the folder picker. */
    private void askGameFolder(boolean change) {
        if (Build.VERSION.SDK_INT>=23 && checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                !=android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{android.Manifest.permission.READ_EXTERNAL_STORAGE,
                android.Manifest.permission.WRITE_EXTERNAL_STORAGE},REQUEST_STORAGE);
            return;
        }
        status.setText(change ? "Choose the Bloodborne game folder." :
            "Choose your Bloodborne game folder (e.g. CUSA03173, the folder with eboot.bin).");
        status.setVisibility(View.VISIBLE);
        if (folderDialogShowing()) folderDialog.dismiss();
        folderDialog=new AlertDialog.Builder(this).setTitle("Bloodborne game folder")
            .setMessage("Select the extracted game folder (e.g. CUSA03173 or CUSA00900, which contains eboot.bin) or the folder "
                +"that holds it, in your phone's storage."+(gameDir!=null ? "\n\nCurrent: "+gameDir : ""))
            .setPositiveButton("Choose folder",(d,w) -> {
                Intent pick=new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                try { startActivityForResult(pick,REQUEST_FOLDER); }
                catch (Exception ex) { error("No folder picker: "+ex); }
            })
            .setNeutralButton("Install from PKG…",(d,w) -> installer.start())
            .setNegativeButton(change ? "Cancel" : "Quit",(d,w) -> { if (!change) finish(); })
            .setCancelable(change).show();
    }
    @Override public void onRequestPermissionsResult(int code,String[] permissions,int[] results) {
        super.onRequestPermissionsResult(code,permissions,results);
        if (code==ModsDialog.REQUEST_STORAGE) {
            boolean granted=results.length>0;
            for (int r:results) granted&=r==android.content.pm.PackageManager.PERMISSION_GRANTED;
            mods.storageResult(granted); return;
        }
        if (code!=REQUEST_STORAGE) return;
        if (results.length>0 && results[0]==android.content.pm.PackageManager.PERMISSION_GRANTED) {
            if (hasGame()) startPending(); else askGameFolder(false);
        } else error("Bloodborne needs storage access to read the game folder.");
    }
    @Override protected void onActivityResult(int code,int result,Intent data) {
        super.onActivityResult(code,result,data);
        if (code==REQUEST_DRIVER) {
            if (result==RESULT_OK && data!=null && data.getData()!=null) installDriver(data.getData());
            return;
        }
        if (code==ModsDialog.REQUEST_FOLDER || code==ModsDialog.REQUEST_ARCHIVE) { mods.onResult(code,result,data); return; }
        if (code==REQUEST_SAVES) {
            if (result==RESULT_OK && data!=null && data.getData()!=null) restoreSaves(folderOf(data.getData()));
            else immersive();
            return;
        }
        if (code==GameInstaller.REQUEST_PKG || code==GameInstaller.REQUEST_DEST) { installer.onResult(code,result,data); return; }
        if (code!=REQUEST_FOLDER) return;
        if (result!=RESULT_OK || data==null || data.getData()==null) { if (!hasGame()) askGameFolder(false); return; }
        File dir=folderOf(data.getData());
        if (dir!=null && !isGame(dir))
            for (String serial : SERIALS) if (isGame(new File(dir,serial))) { dir=new File(dir,serial); break; }
        if (!isGame(dir)) {
            new AlertDialog.Builder(this).setTitle("Not a Bloodborne folder")
                .setMessage((dir==null ? "That location" : dir.getPath())+" has no eboot.bin.")
                .setPositiveButton("Choose again",(d,w) -> askGameFolder(gameDir!=null)).show();
            return;
        }
        boolean changed=gameDir!=null && !gameDir.equals(dir.getPath());
        gameDir=dir.getPath();
        getSharedPreferences("bloodborne",MODE_PRIVATE).edit().putString("game_dir",gameDir).apply();
        status.setText("Starting Bloodborne…");
        if (changed && game!=null) restartGame(); else startPending();
    }
    // ---- Save data backup and restore (SaveBackup) ----
    private File savesDir() { return SaveBackup.saves(new File(BASE)); }
    private boolean gameRunning() { Process p=game; return p!=null && p.isAlive(); }
    private boolean saveStorage() {
        if (Build.VERSION.SDK_INT<23 || checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                ==android.content.pm.PackageManager.PERMISSION_GRANTED) return true;
        toast("Allow storage access first (it is asked when the game starts)."); return false;
    }
    private static final String STOP_NOTE="\n\nThe game is stopped for this and starts again afterwards; unsaved progress is lost.";
    private void backupSaves() {
        if (!saveStorage()) return;
        if (!SaveBackup.hasSaves(savesDir())) {
            new AlertDialog.Builder(this).setTitle("No saves yet").setMessage("There is no save data to back up. Play and save in the game first.")
                .setPositiveButton("OK",null).setOnDismissListener(d -> immersive()).show();
            return;
        }
        new AlertDialog.Builder(this).setTitle("Back up saves?")
            .setMessage("Copies your saves to a new Bloodborne-saves-… folder in your Download folder.")
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Back up",(d,w) -> saveJob("Backing up saves…","Backup failed",false,() -> {
                File dest=SaveBackup.backup(savesDir(),downloads(),new java.util.Date());
                return "Saves copied to\n"+dest.getPath();
            })).setOnDismissListener(d -> immersive()).show();
    }
    private File downloads() { return android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_DOWNLOADS); }
    private void askRestoreSaves() {
        if (!saveStorage()) return;
        new AlertDialog.Builder(this).setTitle("Restore saves")
            .setMessage("Choose a backup folder (Bloodborne-saves-… in Download) or a savedata folder. It replaces your current saves; "
                +"those are backed up to Download first.")
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Choose folder",(d,w) -> {
                try { startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE),REQUEST_SAVES); }
                catch (Exception ex) { toast("No folder picker: "+ex); }
            }).setOnDismissListener(d -> immersive()).show();
    }
    private void restoreSaves(File picked) {
        File source=SaveBackup.find(picked);
        if (source==null) {
            new AlertDialog.Builder(this).setTitle("Not a saves folder")
                .setMessage((picked==null ? "That location" : picked.getPath())+" has no save data (expected a savedata folder, "
                    +"or a folder with one, holding user id / title id folders such as 1000/CUSA03173).")
                .setPositiveButton("OK",null).setOnDismissListener(d -> immersive()).show();
            return;
        }
        try { if (source.getCanonicalFile().equals(savesDir().getCanonicalFile())) { toast("That is the current save data."); immersive(); return; } }
        catch (java.io.IOException ignored) {}
        new AlertDialog.Builder(this).setTitle("Replace your saves?")
            .setMessage("This replaces your current saves with those in\n"+source.getPath()+"\n\nYour current saves are backed up to the "
                +"Download folder first."+(gameRunning() ? STOP_NOTE : ""))
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Restore",(d,w) -> saveJob("Restoring saves…","Restore failed",true,() -> {
                String note="";
                if (SaveBackup.hasSaves(savesDir()))
                    note="\n\nYour previous saves are in\n"+SaveBackup.backup(savesDir(),downloads(),new java.util.Date()).getPath();
                SaveBackup.restore(savesDir(),source);
                return "Saves restored."+note;
            })).setOnDismissListener(d -> immersive()).show();
    }
    /** Runs a save data copy off the UI thread, then shows the result. With stop, the game (which writes the saves)
     *  is stopped for the copy and started again afterwards. */
    private void saveJob(String progress,String failure,boolean stop,java.util.concurrent.Callable<String> job) {
        boolean wasRunning=stop && gameRunning();
        AlertDialog busy=new AlertDialog.Builder(this).setTitle(progress).setMessage("Please wait.").setCancelable(false).show();
        worker.execute(() -> {
            if (wasRunning) stopGame();
            String message; boolean ok=true;
            try { message=job.call(); }
            catch (Exception ex) { ok=false; message=ex.getMessage()==null ? ex.toString() : ex.getMessage(); android.util.Log.e("Bloodborne","saves: "+ex); }
            String text=message; boolean success=ok;
            main.post(() -> {
                busy.dismiss();
                if (destroyed) return;
                new AlertDialog.Builder(this).setTitle(success ? "Done" : failure).setMessage(text)
                    .setPositiveButton("OK",null).setOnDismissListener(d -> immersive()).show();
                if (wasRunning && shmid>=0) restartGame();
            });
        });
    }
    /** The game was installed from PKGs: the same as choosing its folder, and the game starts (again). */
    private void useInstalledGame(File dir) {
        gameDir=dir.getPath();
        getSharedPreferences("bloodborne",MODE_PRIVATE).edit().putString("game_dir",gameDir).apply();
        status.setText("Starting Bloodborne…"); status.setVisibility(View.VISIBLE);
        if (shmid>=0) restartGame(); else startPending(); // the display runs already when the game was stopped for the install
    }
    /** A storage folder of the picker (tree document "primary:Path" or "XXXX-XXXX:Path") as a path. */
    static File folderOf(android.net.Uri tree) {
        try {
            String id=android.provider.DocumentsContract.getTreeDocumentId(tree);
            int colon=id.indexOf(':');
            String volume=colon<0 ? id : id.substring(0,colon), path=colon<0 ? "" : id.substring(colon+1);
            return DocPath.volume(android.os.Environment.getExternalStorageDirectory(),volume,path);
        } catch (Exception ex) { return null; }
    }
    /** The file path of a picked document ("primary:Download/x.pkg", "XXXX-XXXX:x", "raw:/storage/..."), or
     *  null for other providers (the caller copies the content instead). */
    private File pathOf(android.net.Uri document) {
        try {
            if ("file".equals(document.getScheme())) return new File(document.getPath());
            if (!android.provider.DocumentsContract.isDocumentUri(this,document)) return null;
            return DocPath.document(document.getAuthority(),android.provider.DocumentsContract.getDocumentId(document),
                android.os.Environment.getExternalStorageDirectory());
        } catch (Exception ex) { return null; }
    }
    private GameInstaller.Host modsHost() {
        return new GameInstaller.Host() {
            @Override public String script(String... args) throws java.io.IOException {
                unpackRuntime();
                if (!new File(BASE,ModsDialog.SCRIPT).isFile()) return null;
                Process p=process(scriptCommand(ModsDialog.SCRIPT,args));
                try {
                    StringBuilder out=new StringBuilder();
                    try (BufferedReader r=new BufferedReader(new InputStreamReader(p.getInputStream(),"UTF-8"))) {
                        for (String line; (line=r.readLine())!=null && out.length()<(4<<20);) out.append(line).append('\n');
                    }
                    p.waitFor(30,java.util.concurrent.TimeUnit.SECONDS);
                    return out.toString();
                } catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new java.io.IOException("Interrupted"); }
                finally { p.destroyForcibly(); }
            }
            @Override public boolean scriptStream(String script,java.util.function.Consumer<String> lines,AtomicBoolean cancel,String... args) throws java.io.IOException {
                unpackRuntime();
                if (!new File(BASE,script).isFile()) return false;
                Process p=process(scriptCommand(script,args));
                // Cancel (or the app closing) stops the process, which ends the read below.
                Thread watch=new Thread(() -> {
                    while (p.isAlive()) {
                        if (cancel.get() || destroyed) { stopScript(p); return; }
                        try { Thread.sleep(100); } catch (InterruptedException e) { return; }
                    }
                },"Mods-cancel");
                watch.setDaemon(true); watch.start();
                try (BufferedReader r=new BufferedReader(new InputStreamReader(p.getInputStream(),"UTF-8"))) {
                    for (String line; (line=r.readLine())!=null;) lines.accept(line);
                    p.waitFor();
                } catch (InterruptedException e) { Thread.currentThread().interrupt(); }
                finally { stopScript(p); watch.interrupt(); }
                return true;
            }
            @Override public File pathOf(android.net.Uri document) { return MainActivity.this.pathOf(document); }
            @Override public boolean hasStorage() {
                return Build.VERSION.SDK_INT<23 || (checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE)
                    ==android.content.pm.PackageManager.PERMISSION_GRANTED
                    && checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    ==android.content.pm.PackageManager.PERMISSION_GRANTED);
            }
            @Override public void requestStorage() {
                requestPermissions(new String[]{android.Manifest.permission.READ_EXTERNAL_STORAGE,
                    android.Manifest.permission.WRITE_EXTERNAL_STORAGE},ModsDialog.REQUEST_STORAGE);
            }
            @Override public void pick(Intent intent,int code) { startActivityForResult(intent,code); }
            @Override public File folderOf(android.net.Uri tree) { return MainActivity.folderOf(tree); }
            @Override public String setting(String key) { return MainActivity.this.setting(key); }
            @Override public boolean set(String key,String value) {
                return new SettingsDialog(MainActivity.this,new File(BASE,"bbport.ini"),MainActivity.this::restartGame).set(key,value);
            }
            @Override public File baseDir() { return new File(BASE); }
            @Override public String gameDir() { return gameDir; }
            @Override public boolean gameRunning() { Process p=game; return p!=null && p.isAlive(); }
            @Override public void stopGame() { MainActivity.this.stopGame(); }
            @Override public void useGame(File dir) { useInstalledGame(dir); }
            @Override public void installEnded(boolean stopped) {
                if (destroyed) return;
                if (!hasGame()) askGameFolder(false);
                else if (stopped && shmid>=0) restartGame(); // the install stopped the game and did not finish
            }
            @Override public void toast(String message) { MainActivity.this.toast(message); }
            @Override public void restart() { restartGame(); }
            @Override public void closed() { if (!destroyed) immersive(); }
            @Override public boolean destroyed() { return destroyed; }
        };
    }
    /** The tool scripts come with the runtime, which otherwise unpacks only when the game starts. */
    private void unpackRuntime() throws java.io.IOException {
        try { RuntimeInstaller.ensure(this,new File(FILES),message -> main.post(() -> status.setText(message))); }
        catch (java.io.IOException ex) { throw ex; }
        catch (Exception ex) { throw new java.io.IOException("The runtime could not be installed: "+ex.getMessage()); }
    }
    /** The runtime's tool scripts (mods.sh) run in the rootfs shell, like run-thor.sh. */
    private String[] scriptCommand(String script,String[] args) {
        String[] command=new String[args.length+2];
        command[0]=ROOT+"/bin/sh"; command[1]=BASE+"/"+script;
        System.arraycopy(args,0,command,2,args.length);
        return command;
    }
    private void startPending() {
        SurfaceHolder h=pendingHolder!=null ? pendingHolder : surface.getHolder();
        pendingHolder=null;
        if (h.getSurface()!=null && h.getSurface().isValid() && !startingX && shmid<0) {
            startingX=true; worker.execute(() -> startX(h));
        }
    }

    private void pollFrames() {
        if (destroyed) return;
        Process running=game;
        // A driver from a zip without this app's Mesa patches presents through X11, which the bridge
        // shows without counting it: the start text goes once that game has run for a while.
        if (!hadError && (DisplayBridge.directFrames()>0 || customDriver && running!=null && running.isAlive()
                && android.os.SystemClock.uptimeMillis()-gameStartedAt>20000)) status.setVisibility(View.GONE);
        main.postDelayed(this::pollFrames,1000);
    }

    /** The game's text dialog (the character name, BB_IME_FILE in run-thor.sh): shown here with the
     *  touch keyboard. The request holds the maximum length, the title and the current text. */
    private void pollIme() {
        if (destroyed) return;
        File request=new File(BASE,"ime.request");
        if (!imeOpen && request.isFile()) {
            try (BufferedReader r=new BufferedReader(new InputStreamReader(new java.io.FileInputStream(request),"UTF-8"))) {
                int max=Integer.parseInt(r.readLine().trim());
                String title=r.readLine(), initial=r.readLine();
                request.delete();
                showIme(Math.max(1,max),title==null ? "" : title,initial==null ? "" : initial);
            } catch (Exception ex) { android.util.Log.e("Bloodborne","text request: "+ex); request.delete(); }
        }
        main.postDelayed(this::pollIme,250);
    }
    private void showIme(int max,String title,String initial) {
        imeOpen=true; imeAnswered.set(false); pad.reset(); closeDrawer();
        EditText field=new EditText(this);
        field.setSingleLine(true); field.setText(initial); field.setSelection(field.getText().length());
        field.setFilters(new InputFilter[]{new InputFilter.LengthFilter(max)});
        field.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        FrameLayout box=new FrameLayout(this); box.setPadding(dp(20),dp(8),dp(20),0); box.addView(field);
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle(title.isEmpty() ? "Enter text" : title)
            .setView(box)
            .setPositiveButton("OK",(d,w) -> answerIme(true,field.getText().toString()))
            .setNegativeButton("Cancel",(d,w) -> answerIme(false,""))
            .setOnCancelListener(d -> answerIme(false,"")).create();
        field.setOnEditorActionListener((v,action,event) -> {
            if (action!=EditorInfo.IME_ACTION_DONE) return false;
            dialog.dismiss(); answerIme(true,field.getText().toString()); return true;
        });
        dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        dialog.setOnDismissListener(d -> { imeOpen=false; immersive(); surface.requestFocus(); });
        dialog.show(); field.requestFocus();
    }
    private void answerIme(boolean ok,String value) {
        if (!imeAnswered.compareAndSet(false,true)) return;
        File temporary=new File(BASE,"ime.request.result.tmp"), result=new File(BASE,"ime.request.result");
        try (java.io.Writer w=new java.io.OutputStreamWriter(new java.io.FileOutputStream(temporary),"UTF-8")) {
            w.write((ok ? "ok" : "cancel")+"\n"+value.replace('\n',' ')+"\n");
        } catch (Exception ex) { android.util.Log.e("Bloodborne","text answer: "+ex); return; }
        if (!temporary.renameTo(result)) android.util.Log.e("Bloodborne","text answer: rename failed");
    }
    // ---- GPU driver: the bundled Turnip or one installed from a zip (DriverManager, run-thor.sh) ----
    private void showDrivers() {
        java.util.List<DriverManager.Driver> list=drivers.list();
        DriverManager.Driver current=drivers.find(setting("gpu_driver"));
        String[] names=new String[list.size()+1];
        names[0]="Built-in Turnip (automatic by GPU)";
        int checked=0;
        for (int i=0;i<list.size();++i) {
            names[i+1]=list.get(i).title();
            if (current!=null && current.id.equals(list.get(i).id)) checked=i+1;
        }
        int[] choice={checked};
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle("GPU driver")
            .setSingleChoiceItems(names,checked,(d,which) -> {
                choice[0]=which;
                ((AlertDialog)d).getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(which>0);
            })
            .setPositiveButton("Use",(d,w) -> useDriver(choice[0]==0 ? null : list.get(choice[0]-1)))
            .setNeutralButton("Add driver…",(d,w) -> askAddDriver())
            .setNegativeButton("Delete",(d,w) -> { if (choice[0]>0) deleteDriver(list.get(choice[0]-1)); })
            .create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(choice[0]>0));
        dialog.setOnDismissListener(d -> immersive());
        dialog.show();
    }
    /** A driver comes from the app's GitHub releases or from a zip on the device. */
    private void askAddDriver() {
        new AlertDialog.Builder(this).setTitle("Add a GPU driver")
            .setItems(new String[]{"Download from GitHub","Install a zip from this device"},(d,which) -> {
                if (which==0) showDriverDownloads(); else askDriverZip();
            })
            .setNegativeButton("Cancel",null).setOnDismissListener(d -> immersive()).show();
    }
    private void showDriverDownloads() {
        if (driverDownload!=null) driverDownload.close();
        driverDownload=new DriverDownloadDialog(this,drivers,new File(FILES),new DriverDownloadDialog.Host() {
            @Override public String gpuVariant() { return MainActivity.this.gpuVariant(); }
            @Override public void use(DriverManager.Driver driver) { useDriver(driver); }
            @Override public void closed() { if (!destroyed) immersive(); }
            @Override public boolean destroyed() { return destroyed; }
        });
        driverDownload.show();
    }
    /** The bundled Turnip that suits this GPU, as run-thor.sh chooses it from kgsl-chip-id's output
     *  (0x44... is Adreno 8xx): "gen8", "main", or null when the GPU cannot be read. Blocks. */
    private String gpuVariant() {
        try {
            Process p=process(BASE+"/arm64/bin/kgsl-chip-id");
            try {
                if (!p.waitFor(5,java.util.concurrent.TimeUnit.SECONDS)) return null;
                String line=new BufferedReader(new InputStreamReader(p.getInputStream())).readLine();
                if (line==null || !line.trim().startsWith("0x")) return null;
                return line.trim().startsWith("0x44") ? "gen8" : "main";
            } finally { p.destroyForcibly(); }
        } catch (Exception ex) {
            android.util.Log.w("Bloodborne","GPU detection: "+ex);
            return null;
        }
    }
    private void askDriverZip() {
        new AlertDialog.Builder(this).setTitle("Install a GPU driver")
            .setMessage("Choose a zip with a Linux (glibc) build of Turnip, such as the Turnip builds for "
                +"Winlator or Mobox. Turnip for Android (adrenotools zips) cannot load here.\n\n"
                +"Drivers without this app's Mesa patches show the picture through X11, which is slower, and "
                +"may not work with Lossless Scaling. A driver you install turns off the automatic choice of Turnip by GPU. "
                +"If the game stops, choose Built-in Turnip again.")
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Choose zip",(d,w) -> {
                Intent pick=new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                    .putExtra(Intent.EXTRA_MIME_TYPES,new String[]{"application/zip","application/x-zip-compressed",
                        "application/octet-stream"});
                try { startActivityForResult(pick,REQUEST_DRIVER); }
                catch (Exception ex) { toast("No file picker: "+ex); }
            }).show();
    }
    private void installDriver(android.net.Uri zip) {
        String zipName="driver.zip";
        try (android.database.Cursor c=getContentResolver().query(zip,
                new String[]{android.provider.OpenableColumns.DISPLAY_NAME},null,null,null)) {
            if (c!=null && c.moveToFirst() && c.getString(0)!=null) zipName=c.getString(0);
        } catch (Exception ignored) {}
        final String name=zipName;
        AlertDialog progress=new AlertDialog.Builder(this).setTitle("GPU driver")
            .setMessage("Installing "+name+"…").setCancelable(false).show();
        Thread install=new Thread(() -> {
            DriverManager.Driver installed=null; String failure=null;
            try (java.io.InputStream in=getContentResolver().openInputStream(zip)) {
                if (in==null) throw new java.io.IOException("Cannot open "+name);
                installed=drivers.install(in,name);
            } catch (Exception ex) {
                failure=ex.getMessage()!=null ? ex.getMessage() : ex.toString();
                android.util.Log.e("Bloodborne","driver install: "+ex);
            }
            DriverManager.Driver result=installed; String problem=failure;
            main.post(() -> {
                progress.dismiss();
                if (destroyed) return;
                if (result==null) new AlertDialog.Builder(this).setTitle("Driver not installed").setMessage(problem)
                    .setPositiveButton("OK",null).setOnDismissListener(d -> immersive()).show();
                else new AlertDialog.Builder(this).setTitle("Driver installed")
                    .setMessage(result.title()+" is installed. Use it now?")
                    .setNegativeButton("Later",null).setPositiveButton("Use",(d,w) -> useDriver(result))
                    .setOnDismissListener(d -> immersive()).show();
            });
        },"Driver-install");
        install.setDaemon(true); install.start();
    }
    /** Picks the driver for the next start (null: the bundled Turnip) and offers the restart. */
    private void useDriver(DriverManager.Driver driver) {
        String id=driver==null ? "" : driver.id, before=setting("gpu_driver");
        if (id.equals(before==null ? "" : before)) return;
        if (!new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame).set("gpu_driver",id)) {
            toast("Could not save the GPU driver setting."); return;
        }
        askRestart(driver==null ? "the built-in Turnip" : driver.title());
    }
    private void deleteDriver(DriverManager.Driver driver) {
        new AlertDialog.Builder(this).setTitle("Delete "+driver.title()+"?")
            .setNegativeButton("Cancel",(d,w) -> showDrivers())
            .setPositiveButton("Delete",(d,w) -> {
                boolean active=driver.id.equals(setting("gpu_driver"));
                if (active) new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame).set("gpu_driver","");
                try { drivers.delete(driver.id); }
                catch (Exception ex) { toast("Could not delete the driver: "+ex.getMessage()); return; }
                if (active) askRestart("the built-in Turnip"); else showDrivers();
            }).show();
    }
    private void toast(String message) { android.widget.Toast.makeText(this,message,android.widget.Toast.LENGTH_LONG).show(); }
    private void askRestart(String driver) {
        if (shmid<0 || displayOnly) return; // the game has not started: it starts with this driver
        new AlertDialog.Builder(this).setTitle("Restart Bloodborne?")
            .setMessage("The game uses "+driver+" after a restart. Any unsaved progress will be lost.")
            .setNegativeButton("Later",null).setPositiveButton("Restart",(d,w) -> restartGame())
            .setOnDismissListener(d -> immersive()).show();
    }
    /** The last value of a bbport.ini setting (as run-thor.sh reads it), or null. */
    private String setting(String key) {
        String value=null;
        try (BufferedReader r=new BufferedReader(new java.io.FileReader(new File(BASE,"bbport.ini")))) {
            for (String line; (line=r.readLine())!=null;)
                if (line.startsWith(key+"=")) value=line.substring(key.length()+1).trim();
        } catch (Exception ignored) {}
        return value;
    }
    /** Frame generation (Lossless Scaling or FSR 3.1) presents 2-4 frames per game frame. Lossless
     *  Scaling presents them back to back and FIFO spaces them, evenly only when the display runs at
     *  FPS limit x frames: that mode when the display has it (30x2: 60 Hz, 30x4, 40x3, 60x2: 120 Hz),
     *  else the fastest one (at the system's 60 Hz generated frames were dropped). Without frame
     *  generation, the system's choice. */
    private void pickRefreshRate() {
        String lsfg=setting("lsfg_multiplier");
        int frames="2".equals(lsfg) || "3".equals(lsfg) || "4".equals(lsfg) ? Integer.parseInt(lsfg)
            : "1".equals(setting("frame_generation")) ? 2 : 1;
        String limit=setting("fps_limit"); // as run-thor.sh: 40, 45, 60, 0 (none), else 30
        final float target=frames*("40".equals(limit) || "45".equals(limit) || "60".equals(limit)
            ? Integer.parseInt(limit) : "0".equals(limit) ? 1000 : 30);
        main.post(() -> {
            if (destroyed) return;
            android.view.Display display=Build.VERSION.SDK_INT>=30 ? getDisplay() : getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current=display.getMode();
            int id=0, fastest=0; float best=0;
            if (frames>1) for (android.view.Display.Mode mode:display.getSupportedModes()) {
                if (mode.getPhysicalWidth()!=current.getPhysicalWidth() || mode.getPhysicalHeight()!=current.getPhysicalHeight()) continue;
                if (Math.abs(mode.getRefreshRate()-target)<1) id=mode.getModeId();
                if (mode.getRefreshRate()>best) { best=mode.getRefreshRate(); fastest=mode.getModeId(); }
            }
            if (id==0) id=fastest;
            WindowManager.LayoutParams attrs=getWindow().getAttributes();
            if (attrs.preferredDisplayModeId!=id) { attrs.preferredDisplayModeId=id; getWindow().setAttributes(attrs); }
        });
    }
    private void startGame() {
        if (destroyed) return;
        hadError=false;
        pickRefreshRate();
        customDriver=drivers.find(setting("gpu_driver"))!=null;
        gameStartedAt=android.os.SystemClock.uptimeMillis();
        new File(BASE,"ime.request").delete(); new File(BASE,"ime.request.result").delete();
        try {
            Process launched=process(ROOT+"/bin/sh",BASE+"/run-thor.sh","game"); game=launched;
            Thread log=new Thread(() -> {
                try (BufferedReader r=new BufferedReader(new InputStreamReader(launched.getInputStream()));
                     PrintWriter out=new PrintWriter(new File(BASE,"logs/android-game.log"))) {
                    String line; while ((line=r.readLine())!=null) { out.println(line); out.flush(); }
                    int code=launched.waitFor();
                    if (game==launched) { game=null; error("Game stopped ("+code+"). Open the menu to restart."); }
                } catch (Exception ex) {
                    // A restart (settings, cheats) kills this process on purpose: its reader is
                    // interrupted then, after the next game started. Only the current game's errors show.
                    if (!destroyed && game==launched) error(ex.toString());
                }
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
        destroyed=true; if (driverDownload!=null) driverDownload.close(); if (mods!=null) mods.close(); if (installer!=null) installer.close(); if (updater!=null) updater.close(); pad.reset(); audio.stop(); main.removeCallbacksAndMessages(null);
        DisplayBridge.stop(); stopGame(); if (xvfb!=null) xvfb.destroyForcibly();
        worker.shutdownNow(); super.onDestroy();
    }
}
