package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
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

/** Bloodborne-only Android player. Keeps the game surface alive beneath its sliding drawer. */
public final class MainActivity extends Activity {
    /** The app's files (by package: com.ducvd89.bloodborne) and the bbport runtime inside them. */
    private String FILES, BASE;
    /** The game (e.g. CUSA03173, with eboot.bin) in shared storage, chosen by the player. */
    private String gameDir;
    private static final int REQUEST_FOLDER=1, REQUEST_STORAGE=2, REQUEST_DRIVER=3, REQUEST_SAVES=9, REQUEST_DLL=10; // 4-6: ModsDialog, 7-8: GameInstaller
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
    private LinearLayout gameSettingsItem, cheatsItem, modsItem, graphicsItem, frameGenItem, driverItem, controllerItem, folderItem;
    private TouchControllerOverlay controllerOverlay;
    private PadBridge pad;
    private AudioBridge audio;
    private volatile Process game;
    /** The display (BbPresent's listener) is running and the game may start. */
    private volatile boolean displayReady;
    private volatile boolean destroyed;
    /** Vulkan drivers installed from zips (the GPU driver menu). */
    private DriverManager drivers;
    private DriverDownloadDialog driverDownload;
    /** The phone's GPU family (cached once the runtime can read it), what auto resolved to at the last start (for
     *  the problem report), whether the one-time notice about it was shown, and whether the drawer started the
     *  detection (it runs a process: off the UI thread). */
    private volatile GpuFamily gpu;
    private volatile String autoLine;
    private volatile boolean autoNoticeShown;
    private final AtomicBoolean gpuProbing=new AtomicBoolean();
    private ModsDialog mods;
    private GameInstaller installer;
    private AppUpdater updater;
    private ReportDialog report;
    private LinearLayout updateItem;
    /** Runs when the Lossless.dll picker returns (the graphics settings show the new state). */
    private Runnable dllDone;
    private boolean drawerOpen, startingDisplay, hadError, swipeConsumed;
    private float touchX, touchY;
    private boolean edgeGesture, imeOpen, imePolling;
    private final AtomicBoolean imeAnswered=new AtomicBoolean();

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        FILES="/data/data/"+getPackageName()+"/files";
        BASE=FILES+"/bbport";
        drivers=new DriverManager(new File(FILES));
        GameInstaller.Host host=modsHost();
        mods=new ModsDialog(this,host); installer=new GameInstaller(this,host);
        updater=new AppUpdater(this,new AppUpdater.Host() {
            @Override public void stopGame() { MainActivity.this.stopGame(); }
            @Override public void restartGame() { if (!destroyed && displayReady) MainActivity.this.restartGame(); }
            @Override public void detail(String text) { setDetail(updateItem,text); }
            @Override public void closed() { if (!destroyed) immersive(); }
            @Override public boolean destroyed() { return destroyed; }
        });
        report=new ReportDialog(this,reportHost());
        worker.execute(updater::cleanup); // downloads an earlier run left
        gameDir=getSharedPreferences("bloodborne",MODE_PRIVATE).getString("game_dir",null);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (Build.VERSION.SDK_INT>=28) {
            WindowManager.LayoutParams attrs=getWindow().getAttributes();
            attrs.layoutInDisplayCutoutMode=WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attrs);
        }
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
    /** The largest surface of the chosen aspect_ratio (16:9 by default; Auto: the display's own) that fits. */
    private int[] fitSize(int width,int height) {
        double aspect=SettingsDialog.aspectOf(setting("aspect_ratio"),SettingsDialog.displaySize(this));
        int w=width, h=height;
        if (w>h*aspect) w=(int)Math.round(h*aspect); else h=(int)Math.round(w/aspect);
        return new int[]{Math.max(1,Math.min(width,w)),Math.max(1,Math.min(height,h))};
    }
    private void fitSurface(int width,int height) {
        if (width<=0 || height<=0) return;
        int[] fit=fitSize(width,height); int w=fit[0], h=fit[1];
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
                if (displayReady) attach(h);
                // Returning from the installer's pickers recreates the surface: the installer owns the flow then
                // (installEnded asks for the folder when it ends without a game).
                else if (!hasGame()) { pendingHolder=h; if (!installer.active()) askGameFolder(false); }
                else if (!startingDisplay) { startingDisplay=true; worker.execute(() -> startDisplay(h)); }
            }
            @Override public void surfaceChanged(SurfaceHolder h,int format,int w,int height) {}
            // BbPresent drops the game's frames while there is no surface.
            @Override public void surfaceDestroyed(SurfaceHolder h) {}
        });
        // Touches on the game display send no mouse input (the game is played with a controller).
        surface.setOnTouchListener((v,e) -> true);
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

        // The game's weathered serif look: IM Fell English (OFL, res/font), the plain serif if it cannot load.
        TextView title=text("Bloodborne",30,0xffe8dfc9); title.setTypeface(Typeface.SERIF,Typeface.BOLD);
        try { title.setTypeface(getResources().getFont(getResources().getIdentifier("im_fell_english","font",getPackageName()))); }
        catch (Exception ex) { android.util.Log.w("Bloodborne","title font: "+ex); }
        title.setLetterSpacing(0.02f); title.setPadding(dp(10),0,0,0); title.setSingleLine(true);
        LinearLayout titleRow=new LinearLayout(this); titleRow.setGravity(Gravity.CENTER_VERTICAL);
        titleRow.addView(title,new LinearLayout.LayoutParams(0,-2,1)); // the pill sits at the right edge
        LinearLayout.LayoutParams bp=new LinearLayout.LayoutParams(-2,-2); bp.leftMargin=dp(6);
        titleRow.addView(donateBadge(),bp);
        drawerItems.addView(titleRow,new LinearLayout.LayoutParams(-1,-2));
        TextView hint=text("Swipe left or press ○ to return",12,0xff8e9097);
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
        // Lossless Scaling frame generation: applies at once, no restart (bbport/lsfg.state, which libbbgpu follows).
        frameGenItem=menuItem("ic_layers","Bloodframe","",() -> {
            closeDrawer();
            SettingsDialog ini=new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame);
            new FrameGenDialog(this,ini,new File(BASE,"lsfg.state"),lossless,this::pickRefreshRate).show();
        });
        driverItem=menuItem("ic_memory","GPU driver","",() -> { closeDrawer(); showDrivers(); });

        section("Controls");
        controllerItem=menuItem("ic_gamepad","On-screen controller","",() -> {
            boolean enabled=!controllerEnabled();
            getSharedPreferences("bloodborne",MODE_PRIVATE).edit().putBoolean("controller_overlay",enabled).apply();
            controllerOverlay.release();
            closeDrawer();
        });
        menuItem("ic_edit","Edit controller layout","Move, resize or reset the buttons",() -> {
            getSharedPreferences("bloodborne",MODE_PRIVATE).edit().putBoolean("controller_overlay",true).apply();
            closeDrawer();
            controllerOverlay.edit(null);
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
        menuItem("ic_bug_report","Report a problem","Save logs for a GitHub issue",() -> { closeDrawer(); report.show(); });
        menuItem("ic_tune","Debug options","Performance switches for testing",() -> {
            closeDrawer();
            new DebugDialog(this,new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame),this::restartGame).show();
        });

        // The version and the credit, pinned under the scrolling menu.
        String version="";
        try { version=" "+getPackageManager().getPackageInfo(getPackageName(),0).versionName; }
        catch (Exception ignored) {}
        TextView about=text("Bloodborne for Android"+version,12,0xff8e9097);
        about.setPadding(dp(24),dp(8),dp(14),0);
        drawer.addView(about,new LinearLayout.LayoutParams(-1,-2));
        TextView credit=text("by ducvd89@gmail.com\ngithub.com/ducvd89/Bloodborne-Android",10,0xff6f7178);
        credit.setPadding(dp(24),dp(2),dp(14),dp(14));
        drawer.addView(credit,new LinearLayout.LayoutParams(-1,-2));
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

    /** The "Buy me a Claude token" pill beside the title: the coin and two lines of script on yellow,
     *  opens PayPal.Me in the browser. Focusable for the gamepad, with a gold edge when focused. */
    private View donateBadge() {
        LinearLayout pill=new LinearLayout(this); pill.setGravity(Gravity.CENTER_VERTICAL);
        pill.setPadding(dp(4),dp(3),dp(5),dp(3));
        pill.setContentDescription("Buy me a Claude token");
        pill.setFocusable(true); pill.setClickable(true);
        pill.setOnClickListener(v -> {
            try { startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse("https://paypal.me/Ducvd89"))); }
            catch (Exception ex) { android.util.Log.w("Bloodborne","donate link: "+ex); }
        });
        android.graphics.drawable.StateListDrawable states=new android.graphics.drawable.StateListDrawable();
        GradientDrawable lit=new GradientDrawable(); lit.setColor(0xffffdd00); lit.setCornerRadius(dp(8));
        lit.setStroke(dp(2),0xffc6ad76);
        GradientDrawable pressed=new GradientDrawable(); pressed.setColor(0xffe6c700); pressed.setCornerRadius(dp(8));
        GradientDrawable idle=new GradientDrawable(); idle.setColor(0xffffdd00); idle.setCornerRadius(dp(8));
        states.addState(new int[]{android.R.attr.state_pressed},pressed);
        states.addState(new int[]{android.R.attr.state_focused},lit);
        states.addState(new int[]{},idle);
        pill.setBackground(states);
        android.widget.ImageView coin=new android.widget.ImageView(this);
        coin.setImageResource(getResources().getIdentifier("ic_claude_token","drawable",getPackageName()));
        LinearLayout.LayoutParams cp=new LinearLayout.LayoutParams(dp(16),dp(16)); cp.rightMargin=dp(3);
        pill.addView(coin,cp);
        // Handwritten, like the coffee badge: Android's cursive family (Dancing Script), "Claude token" larger in clay.
        android.text.SpannableString words=new android.text.SpannableString("Buy me a\nClaude token");
        words.setSpan(new android.text.style.RelativeSizeSpan(0.85f),0,8,0);
        words.setSpan(new android.text.style.ForegroundColorSpan(0xff9b4428),9,words.length(),0);
        TextView label=text("",11,0xff1a1a1a); label.setText(words);
        label.setTypeface(Typeface.create("cursive",Typeface.BOLD));
        label.setIncludeFontPadding(false); label.setLineSpacing(0,0.8f);
        pill.addView(label);
        return pill;
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
        String upscaler="off".equals(setting("upscaler")) ? "No upscaling"
            : setting("upscaler")==null || "gsr2".equals(setting("upscaler")) ? "Snapdragon GSR 2" : "FSR 3.1";
        String limit=setting("fps_limit");
        setDetail(graphicsItem,upscaler+" · "+("0".equals(limit) ? "no FPS limit"
            : (limit==null || !limit.matches("40|45|60") ? "30" : limit)+" FPS limit"));
        setDetail(frameGenItem,FrameGenDialog.summary(setting("frame_generation"),setting("upscaler"),setting("lsfg_multiplier"),
            losslessDll()!=null));
        String choice=gpuSetting();
        DriverManager.Driver driver=drivers.find(choice);
        if (choice.equals("auto") && gpu==null && gpuProbing.compareAndSet(false,true)) {
            Thread probe=new Thread(() -> { gpu(); main.post(() -> { if (!destroyed && drawerOpen) refreshDrawer(); }); },"Gpu-probe");
            probe.setDaemon(true); probe.start();
        }
        setDetail(driverItem,choice.equals("auto") ? "Automatic: "+AutoDriver.describe(gpu,Build.VERSION.SDK_INT)
            : driver==null ? "Phone's own driver" : driver.title());
        setDetail(controllerItem,controllerEnabled() ? "On" : "Off");
        setDetail(folderItem,gameDir==null ? "Not chosen" : gameDir);
    }

    private void openDrawer() {
        if (drawerOpen) return;
        drawerOpen=true; controllerOverlay.release(); pad.reset();
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

    /** The game's launch script, run with Android's own shell. extra: NAME=value pairs added to the environment.
     *  The environment is cleared: the script sets what the game process needs (run-bionic.sh). */
    private Process launchScript(String[] extra) throws java.io.IOException {
        ProcessBuilder b=new ProcessBuilder("/system/bin/sh",BASE+"/run-bionic.sh","game").redirectErrorStream(true);
        Map<String,String> env=b.environment(); env.clear();
        env.put("PATH","/system/bin");
        env.put("HOME",FILES+"/home/bbport"); env.put("LANG","C.UTF-8");
        env.put("TMPDIR",FILES+"/tmp");
        env.put("FXD_FILES",FILES); env.put("BB_ANDROID_INPUT","1");
        if (gameDir!=null) env.put("BB_GAME_DIR",gameDir);
        for (String pair:extra) { int eq=pair.indexOf('='); env.put(pair.substring(0,eq),pair.substring(eq+1)); }
        return b.start();
    }
    /** The runtime's PKG tool (bionic/bin/ps4-pkg-tool) is needed before any game has started: the runtime is
     *  otherwise unpacked only by startDisplay. Blocks (off the UI thread). */
    private void ensureRuntime() throws java.io.IOException {
        try { RuntimeInstaller.ensure(this,new File(FILES),message -> {}); }
        catch (java.io.IOException e) { throw e; }
        catch (Exception e) { throw new java.io.IOException("The app's runtime could not be installed: "+e.getMessage(),e); }
    }
    private void startDisplay(SurfaceHolder holder) {
        try {
            RuntimeInstaller.ensure(this, new File(FILES), message -> main.post(() -> status.setText(message)));
            // The runtime brought bbport.default.ini: settings defaults apply before the game reads bbport.ini.
            try { SettingsDefaults.apply(new File(BASE)); main.post(() -> { if (drawerOpen) refreshDrawer(); }); }
            catch (Exception ex) { android.util.Log.e("Bloodborne","default settings: "+ex); }
            if (!new File(BASE,"bionic/bin/bb-probe").canExecute() || !new File(BASE,"run-bionic.sh").isFile())
                throw new java.io.IOException("The Bloodborne runtime is missing. Install the runtime bundle.");
            new File(FILES,"home/bbport").mkdirs();
            new File(FILES,"tmp").mkdirs();
            // No display server: the game presents through the app (BB_PRESENT_SOCKET, BbPresent).
            displayReady=true;
            main.post(() -> attach(holder));
        } catch (Exception ex) { error(ex.toString()); }
    }
    private void attach(SurfaceHolder holder) {
        if (destroyed || !holder.getSurface().isValid()) return;
        // Frames arrive as AHardwareBuffers on a socket (BbPresent).
        String result=BbPresent.start(holder,getFilesDir()+"/bbpresent.sock");
        android.util.Log.i("Bloodborne",result);
        if (!result.startsWith("bridge running:")) { error(result); return; }
        if (game==null) worker.execute(() -> startGame());
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
        new AlertDialog.Builder(this).setTitle("Bloodborne game folder")
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
        if (code==REQUEST_DLL) {
            Uri dll=result==RESULT_OK && data!=null ? data.getData() : null;
            Runnable done=dllDone; dllDone=null;
            if (dll!=null) copyDll(dll,done); else { immersive(); if (done!=null) done.run(); }
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
    // ---- Lossless.dll for Lossless Scaling frame generation (libbloodframe.so reads its shaders) ----
    /** The Lossless.dll run-bionic.sh uses: the app's own copy (bbport/, from Steam or the picker), else one
     *  in the Bloodborne folder of shared storage. Null when there is none. */
    private File losslessDll() {
        for (File dll:new File[]{new File(BASE,"Lossless.dll"),new File("/storage/emulated/0/Bloodborne/Lossless.dll")})
            if (dll.isFile() && dll.length()>0) return dll;
        return null;
    }
    private final FrameGenDialog.LosslessHost lossless=new FrameGenDialog.LosslessHost() {
        @Override public File dll() { return losslessDll(); }
        @Override public void signIn(Runnable done) {
            new SteamDllDialog(MainActivity.this,new File(BASE,"Lossless.dll"),() -> { immersive(); if (done!=null) done.run(); }).show();
        }
        @Override public void choose(Runnable done) {
            Intent pick=new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
            dllDone=done;
            try { startActivityForResult(pick,REQUEST_DLL); }
            catch (Exception ex) { dllDone=null; toast("No file picker: "+ex); }
        }
    };
    /** Copies the chosen file to bbport/Lossless.dll when it is a Windows DLL (MZ header, PE signature), as
     *  large as Lossless.dll can be (about 1-2 MB; 64 MB at most). */
    private void copyDll(Uri source,Runnable done) {
        Thread copy=new Thread(() -> {
            String problem=null;
            File target=new File(BASE,"Lossless.dll"), part=new File(BASE,"Lossless.dll.part");
            try (java.io.InputStream in=getContentResolver().openInputStream(source);
                 java.io.FileOutputStream out=new java.io.FileOutputStream(part)) {
                if (in==null) throw new java.io.IOException("Cannot open the file");
                byte[] buffer=new byte[65536]; long total=0; int count;
                while ((count=in.read(buffer))>0) {
                    total+=count;
                    if (total>64L<<20) throw new java.io.IOException("The file is too large for Lossless.dll");
                    out.write(buffer,0,count);
                }
                out.getFD().sync();
                if (total==0) throw new java.io.IOException("The file is empty");
                if (!isPeFile(part)) throw new java.io.IOException("That is not a Windows DLL (no PE header)");
                if (!part.renameTo(target)) throw new java.io.IOException("Could not save Lossless.dll");
            } catch (Exception ex) {
                part.delete();
                problem=ex.getMessage()!=null ? ex.getMessage() : ex.toString();
                android.util.Log.w("Bloodborne","Lossless.dll: "+ex);
            }
            final String failure=problem;
            main.post(() -> {
                if (destroyed) return;
                toast(failure==null ? "Lossless.dll is ready." : "Lossless.dll not saved: "+failure);
                immersive();
                if (done!=null) done.run();
            });
        },"Lossless-dll");
        copy.setDaemon(true); copy.start();
    }
    /** "MZ" at 0 and "PE\0\0" where the DOS header's e_lfanew (offset 0x3c) points. */
    private static boolean isPeFile(File file) {
        try (java.io.RandomAccessFile f=new java.io.RandomAccessFile(file,"r")) {
            if (f.length()<0x40) return false;
            byte[] mz=new byte[2]; f.readFully(mz);
            if (mz[0]!='M' || mz[1]!='Z') return false;
            f.seek(0x3c);
            int offset=Integer.reverseBytes(f.readInt());
            if (offset<0x40 || offset>f.length()-4) return false;
            f.seek(offset);
            byte[] pe=new byte[4]; f.readFully(pe);
            return pe[0]=='P' && pe[1]=='E' && pe[2]==0 && pe[3]==0;
        } catch (Exception ex) { return false; }
    }
    // ---- Save data backup and restore (SaveBackup) ----
    private File savesDir() { return SaveBackup.saves(new File(BASE)); }
    private boolean gameRunning() { Process p=game; return p!=null && p.isAlive(); }
    private boolean saveStorage() { return saveStorage(true); }
    private boolean saveStorage(boolean explain) {
        if (Build.VERSION.SDK_INT<23 || checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                ==android.content.pm.PackageManager.PERMISSION_GRANTED) return true;
        if (explain) toast("Allow storage access first (it is asked when the game starts)."); return false;
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
    /** What the problem report needs from the app (see {@link ReportDialog}). */
    private ReportDialog.Host reportHost() {
        return new ReportDialog.Host() {
            @Override public java.util.concurrent.Executor worker() { return worker; }
            @Override public void post(Runnable r) { main.post(r); }
            @Override public boolean destroyed() { return destroyed; }
            @Override public void closed() { if (!destroyed) immersive(); }
            @Override public File base() { return new File(BASE); }
            @Override public File downloads() { return saveStorage(false) ? MainActivity.this.downloads() : null; }
            @Override public LogReport.Facts facts() { return reportFacts(); }
        };
    }
    /** Blocks (lists the mods). The game folder is shown by its last component only. */
    private LogReport.Facts reportFacts() {
        LogReport.Facts f=new LogReport.Facts();
        if (gameDir!=null) { f.gameFolder=new File(gameDir).getName(); f.ebootFound=isGame(new File(gameDir)); }
        String choice=gpuSetting();
        DriverManager.Driver driver=drivers.find(choice);
        GpuFamily detected=gpu;
        f.driverLine=choice.equals("auto") ? "auto (setting gpu_driver="+(setting("gpu_driver")==null ? "" : setting("gpu_driver"))+"): "
                +(autoLine!=null ? autoLine : AutoDriver.describe(detected,Build.VERSION.SDK_INT)+" (not resolved yet)")
            : driver==null ? "system (stock), setting gpu_driver=system" : driver.title()+" (setting gpu_driver="+driver.id+")";
        f.losslessDll=losslessDll()!=null;
        if ("0".equals(setting("mods_enabled"))) f.modsLine="off";
        else {
            try {
                int on=0, usable=0, bad=0;
                for (ModManager.Mod m:modManager().list().mods) { if (m.valid) { ++usable; if (m.enabled) ++on; } else ++bad; }
                f.modsLine=on+" enabled of "+usable+" usable"+(bad>0 ? ", "+bad+" not usable" : "");
            } catch (Exception ex) { f.modsLine="unknown"; }
        }
        return f;
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
                if (wasRunning && displayReady) restartGame();
            });
        });
    }
    /** The game was installed from PKGs: the same as choosing its folder, and the game starts (again). */
    private void useInstalledGame(File dir) {
        gameDir=dir.getPath();
        getSharedPreferences("bloodborne",MODE_PRIVATE).edit().putString("game_dir",gameDir).apply();
        status.setText("Starting Bloodborne…"); status.setVisibility(View.VISIBLE);
        if (displayReady) restartGame(); else startPending(); // the display runs already when the game was stopped for the install
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
            @Override public ModManager manager() { return modManager(); }
            @Override public PkgInstaller pkgInstaller() throws java.io.IOException {
                ensureRuntime();
                return new PkgInstaller(new File(BASE,"bionic/bin/ps4-pkg-tool"),line -> android.util.Log.i("Bloodborne-pkg",line));
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
                else if (stopped && displayReady) restartGame(); // the install stopped the game and did not finish
            }
            @Override public void toast(String message) { MainActivity.this.toast(message); }
            @Override public void restart() { restartGame(); }
            @Override public void closed() { if (!destroyed) immersive(); }
            @Override public boolean destroyed() { return destroyed; }
        };
    }
    /** The mods folder (the mods_dir setting, else the default) and mods.json, as the game reads them at launch. */
    private ModManager modManager() {
        String configured=setting("mods_dir");
        return new ModManager(new File(configured==null || configured.isEmpty() ? ModsDialog.DEFAULT_DIR : configured),
            new File(BASE,"mods.json"));
    }
    /** The folder the game runs from (BB_APP0_DIR): the game folder with the enabled mods merged over it as a tree of
     *  links (ModManager.apply), or the game folder itself without mods. null when no game folder is chosen (the launch
     *  script then uses its own). Never throws: the game must start unmodded. Appends the "Mods: ..." lines to `log`. */
    private String prepareMods(java.util.List<String> log) {
        String gameFolder=gameDir;
        if (gameFolder==null) return null;
        File overlays=new File(BASE,"mod-game");
        if ("0".equals(setting("mods_enabled"))) {
            log.add("Mods: off");
            FsUtil.deleteTree(overlays);
            return gameFolder;
        }
        String folder=modManager().apply(new File(gameFolder),overlays,log::add);
        if (!folder.equals(gameFolder) && !new File(folder,"eboot.bin").isFile()) {
            log.add("Mods: could not be set up, starting without them");
            FsUtil.deleteTree(overlays);
            return gameFolder;
        }
        return folder;
    }
    private void startPending() {
        SurfaceHolder h=pendingHolder!=null ? pendingHolder : surface.getHolder();
        pendingHolder=null;
        if (h.getSurface()!=null && h.getSurface().isValid() && !startingDisplay && !displayReady) {
            startingDisplay=true; worker.execute(() -> startDisplay(h));
        }
    }

    private void pollFrames() {
        if (destroyed) return;
        if (!hadError && BbPresent.framesIfLoaded()>0) {
            status.setVisibility(View.GONE);
            if (game!=null && !playing) { playing=true; gameState(false); }
        }
        main.postDelayed(this::pollFrames,1000);
    }
    /** Tells the OS (GameManager, Android 13+) whether the game is loading or being played: game boosters and
     *  the scheduler treat uninterruptible gameplay as such. The game counts as loading from its start until its
     *  first frame (pollFrames), and as not playing once stopped. */
    private boolean playing;
    private void gameState(boolean loading) { gameState(loading,android.app.GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE); }
    private void gameState(boolean loading,int mode) {
        if (Build.VERSION.SDK_INT<33) return;
        try {
            android.app.GameManager gm=getSystemService(android.app.GameManager.class);
            if (gm!=null) gm.setGameState(new android.app.GameState(loading,mode));
        } catch (RuntimeException e) { android.util.Log.w("Bloodborne","GameManager.setGameState: "+e); }
    }

    /** The game's text dialog (the character name, BB_IME_FILE in run-bionic.sh): shown here with the
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
    // ---- GPU driver: the phone's own or one installed from a zip (DriverManager, run-bionic.sh) ----
    private void showDrivers() {
        if (gpu==null) { // the GPU family names what Automatic is: found off the UI thread
            Thread probe=new Thread(() -> { gpu(); main.post(() -> { if (!destroyed) showDriversDialog(); }); },"Gpu-probe");
            probe.setDaemon(true); probe.start();
        } else showDriversDialog();
    }
    private void showDriversDialog() {
        java.util.List<DriverManager.Driver> list=drivers.list();
        String setting=gpuSetting();
        DriverManager.Driver current=drivers.find(setting);
        // Automatic (with what it is on this phone), the phone's own driver, then the installed ones.
        CharSequence[] names=new CharSequence[list.size()+2];
        String automatic="Automatic (recommended)", resolves=AutoDriver.describe(gpu,Build.VERSION.SDK_INT);
        android.text.SpannableString first=new android.text.SpannableString(automatic+"\n"+resolves);
        first.setSpan(new android.text.style.RelativeSizeSpan(0.8f),automatic.length()+1,first.length(),0);
        first.setSpan(new android.text.style.ForegroundColorSpan(0xff8e9097),automatic.length()+1,first.length(),0);
        names[0]=first;
        names[1]="Phone's own driver";
        int checked=setting.equals("auto") ? 0 : current==null ? 1 : 0;
        for (int i=0;i<list.size();++i) {
            names[i+2]=list.get(i).title();
            if (current!=null && current.id.equals(list.get(i).id)) checked=i+2;
        }
        int[] choice={checked};
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle("GPU driver")
            .setSingleChoiceItems(names,checked,(d,which) -> {
                choice[0]=which;
                ((AlertDialog)d).getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(which>1);
            })
            .setPositiveButton("Use",(d,w) -> useDriver(choice[0]==0 ? AUTO : choice[0]==1 ? null : list.get(choice[0]-2)))
            .setNeutralButton("Add driver…",(d,w) -> askAddDriver())
            .setNegativeButton("Delete",(d,w) -> { if (choice[0]>1) deleteDriver(list.get(choice[0]-2)); })
            .create();
        dialog.setOnShowListener(d -> dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(choice[0]>1));
        dialog.setOnDismissListener(d -> immersive());
        dialog.show();
    }
    /** useDriver's argument for Automatic (null is the phone's own driver). */
    private static final DriverManager.Driver AUTO=new DriverManager.Driver("auto","Automatic","","");
    /** The effective gpu_driver setting: "auto", "system", or the id of an installed driver. An empty or missing
     *  value, and an id whose folder is gone (such as a Linux driver the cleanup removed), are "auto". */
    private String gpuSetting() {
        String value=setting("gpu_driver");
        if (value==null) return "auto";
        value=value.trim();
        if (value.equals("system")) return "system";
        if (value.isEmpty() || value.equals("auto") || drivers.find(value)==null) return "auto";
        return value;
    }
    /** The phone's GPU family; blocks the first time (kgsl-chip-id). Cached once the runtime can read it. */
    private GpuFamily gpu() {
        GpuFamily g=gpu;
        if (g==null) {
            g=GpuFamily.detect(BASE);
            if (new File(BASE,"bionic/bin/kgsl-chip-id").exists()) gpu=g;
        }
        return g;
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
    /** The GPU family (GpuFamily): "gen8", "main", "mali", or null when unknown. The download list marks the
     *  builds for it. Blocks. */
    private String gpuVariant() { return gpu().family; }
    private void askDriverZip() {
        new AlertDialog.Builder(this).setTitle("Install a GPU driver")
            .setMessage("Choose a zip with a Turnip (or other Vulkan driver) built for Android, as the adrenotools "
                +"driver packs are: a meta.json with the library's name, and the .so. Linux (glibc) builds do not "
                +"load.\n\nA driver you install replaces the phone's stock driver. "
                +"If the game stops, choose System driver again.")
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
    /** Picks the driver for the next start (null: the phone's own driver, "system"; AUTO: "auto", the bundled driver
     *  for this GPU) and offers the restart. */
    private void useDriver(DriverManager.Driver driver) {
        String id=driver==null ? "system" : driver.id, before=gpuSetting();
        if (id.equals(before)) return;
        if (!new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame).set("gpu_driver",id)) {
            toast("Could not save the GPU driver setting."); return;
        }
        askRestart(driver==null ? "the phone's own driver" : driver==AUTO ? "the automatic driver ("+AutoDriver.describe(gpu,Build.VERSION.SDK_INT)+")" : driver.title());
    }
    private void deleteDriver(DriverManager.Driver driver) {
        new AlertDialog.Builder(this).setTitle("Delete "+driver.title()+"?")
            .setNegativeButton("Cancel",(d,w) -> showDrivers())
            .setPositiveButton("Delete",(d,w) -> {
                boolean active=driver.id.equals(gpuSetting());
                if (active) new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame).set("gpu_driver","system");
                try { drivers.delete(driver.id); }
                catch (Exception ex) { toast("Could not delete the driver: "+ex.getMessage()); return; }
                if (active) askRestart("the phone's own driver"); else showDrivers();
            }).show();
    }
    private void toast(String message) { android.widget.Toast.makeText(this,message,android.widget.Toast.LENGTH_LONG).show(); }
    private void askRestart(String driver) {
        if (!displayReady) return; // the game has not started: it starts with this driver
        new AlertDialog.Builder(this).setTitle("Restart Bloodborne?")
            .setMessage("The game uses "+driver+" after a restart. Any unsaved progress will be lost.")
            .setNegativeButton("Later",null).setPositiveButton("Restart",(d,w) -> restartGame())
            .setOnDismissListener(d -> immersive()).show();
    }
    /** The last value of a bbport.ini setting (as run-bionic.sh reads it), or null. */
    private String setting(String key) {
        String value=null;
        try (BufferedReader r=new BufferedReader(new java.io.FileReader(new File(BASE,"bbport.ini")))) {
            for (String line; (line=r.readLine())!=null;)
                if (line.startsWith(key+"=")) value=line.substring(key.length()+1).trim();
        } catch (Exception ignored) {}
        return value;
    }
    /** Frame generation (Lossless Scaling, or FSR 3.1) presents 2-4 frames per game frame, spaced by FIFO evenly
     *  only when the display runs at FPS limit x frames: that mode when the display has it (30x2: 60 Hz, 30x4,
     *  40x3, 60x2: 120 Hz), else the fastest one. Without frame generation, the system's choice. VSync (the vsync
     *  setting, which Lossless Scaling turns on by itself: a FIFO in the app's bbpresent) is what makes that
     *  spacing reliable: each presented frame then stays up for exactly one refresh, so a real frame and its
     *  generated ones alternate evenly; without it frames that land in the same refresh replace each other and
     *  the pairs skip. Lossless Scaling counts only with a Lossless.dll (else run-bionic.sh leaves it off). */
    private void pickRefreshRate() {
        String lsfg=setting("lsfg_multiplier");
        int frames=lsfg!=null && lsfg.matches("[234]") && losslessDll()!=null ? Integer.parseInt(lsfg)
            : "1".equals(setting("frame_generation")) ? 2 : 1;
        String limit=setting("fps_limit"); // as run-bionic.sh: 40, 45, 60, 0 (none), else 30
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
        playing=false; gameState(true);
        pickRefreshRate();
        new File(BASE,"ime.request").delete(); new File(BASE,"ime.request.result").delete();
        try {
            // The game folder with the mods merged over it: the launch script takes it from BB_APP0_DIR.
            java.util.List<String> modsLog=new java.util.ArrayList<>();
            String app0=prepareMods(modsLog);
            for (String line:modsLog) android.util.Log.i("Bloodborne",line);
            java.util.List<String> extra=new java.util.ArrayList<>();
            if (app0!=null) extra.add("BB_APP0_DIR="+app0);
            // Automatic GPU driver: the bundled one for this GPU (installed from the APK on first use) or the phone's own.
            if (gpuSetting().equals("auto")) {
                AutoDriver.Result auto=AutoDriver.resolve(this,drivers,gpu(),Build.VERSION.SDK_INT,
                    message -> main.post(() -> { if (!destroyed) { status.setText(message); status.setVisibility(View.VISIBLE); } }));
                autoLine=auto.line; modsLog.add(auto.line);
                android.util.Log.i("Bloodborne",auto.line);
                extra.add("BB_GPU_DRIVER="+(auto.id==null ? "system" : auto.id));
                extra.add("BB_GPU_DRIVER_WHY="+auto.line);
                if (auto.notice!=null && !autoNoticeShown) { autoNoticeShown=true; main.post(() -> { if (!destroyed) toast(auto.notice); }); }
                main.post(() -> { if (!destroyed && drawerOpen) refreshDrawer(); });
            } else autoLine=null;
            // The surface's size in pixels: the present target of libbbgpu (BB_SCREEN_W/H).
            // The aspect setting may have changed since the layout: refit, and report the size the surface gets.
            int w=surface.getWidth(), h=surface.getHeight();
            if (screen.getWidth()>0 && screen.getHeight()>0) {
                int[] fit=fitSize(screen.getWidth(),screen.getHeight()); w=fit[0]; h=fit[1];
                main.post(() -> fitSurface(screen.getWidth(),screen.getHeight()));
            }
            // The physical display (landscape, long side first), for output_res=native and aspect_ratio=auto.
            int[] display=SettingsDialog.displaySize(this);
            new SettingsDialog(this,new File(BASE,"bbport.ini"),this::restartGame).set("display_size",display[0]+"x"+display[1]);
            extra.add("BB_SCREEN_W="+(w>0 ? w : 1280)); extra.add("BB_SCREEN_H="+(h>0 ? h : 720));
            // The last run's log stays as android-game.prev.log (one generation): a crash log survives the restart.
            File gameLog=new File(BASE,"logs/android-game.log"), previous=new File(BASE,"logs/android-game.prev.log");
            gameLog.getParentFile().mkdirs();
            if (gameLog.length()>0) { previous.delete(); gameLog.renameTo(previous); }
            final Process launched=launchScript(extra.toArray(new String[0]));
            game=launched;
            Thread log=new Thread(() -> {
                try (BufferedReader r=new BufferedReader(new InputStreamReader(launched.getInputStream()));
                     PrintWriter out=new PrintWriter(gameLog)) {
                    for (String line:modsLog) out.println(line);
                    out.flush();
                    boolean driverTrouble=false;
                    String line; while ((line=r.readLine())!=null) {
                        out.println(line); out.flush();
                        if (!driverTrouble && driverTrouble(line)) driverTrouble=true;
                    }
                    int code=launched.waitFor();
                    if (game==launched) {
                        game=null;
                        error("Game stopped ("+code+")."+(driverTrouble
                            ? " It looks like a GPU driver problem: Menu \u2192 GPU driver, pick another driver package"
                              +" (Download lists more), then restart."
                            : " Open the menu to restart."));
                    }
                } catch (Exception ex) {
                    // A restart (settings, cheats) kills this process on purpose: its reader is
                    // interrupted then, after the next game started. Only the current game's errors show.
                    if (!destroyed && game==launched) error(ex.toString());
                }
            },"Bloodborne-log"); log.setDaemon(true); log.start();
        } catch (Exception ex) { error(ex.toString()); }
    }
    /** Whether a game log line points at the GPU driver: the GPU lost the device (a hang or a driver fault), the
     *  driver did not load or start (vk_driver_loader, vk_instance), or a native crash inside the Vulkan driver
     *  library (probe.c's fault lines name it). The error screen then suggests another driver package. */
    static boolean driverTrouble(String line) {
        return line.contains("Device lost") || line.contains("DeviceLost")
            || line.startsWith("Vulkan: cannot load driver") || line.startsWith("Vulkan: no system loader")
            || line.startsWith("Vulkan: driver ")
            || line.contains("Failed to create device") || line.contains("ErrorInitializationFailed")
            || line.contains("ErrorIncompatibleDriver")
            || ((line.startsWith("Host fault") || line.startsWith("  return address")) && line.contains("libvulkan"));
    }
    private void error(String message) {
        hadError=true;
        android.util.Log.e("Bloodborne",message);
        main.post(() -> { if (!destroyed) { status.setText(message+"\nMenu \u2192 Report a problem saves the logs."); status.setVisibility(View.VISIBLE); } });
    }
    private void stopGame() {
        Process p=game; game=null;
        playing=false; gameState(false,android.app.GameState.MODE_NONE);
        if (p!=null) { p.destroyForcibly(); try { p.waitFor(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); } }
    }
    @Override protected void onDestroy() {
        destroyed=true; if (driverDownload!=null) driverDownload.close(); if (mods!=null) mods.close(); if (installer!=null) installer.close(); if (updater!=null) updater.close(); pad.reset(); audio.stop(); main.removeCallbacksAndMessages(null);
        stopGame();
        worker.shutdownNow(); super.onDestroy();
    }
}
