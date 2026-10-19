package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import org.json.JSONArray;
import org.json.JSONObject;

/** The Mods menu: loose-file mods for Bloodborne 1.09, folders in the mods folder. Lists them in load
 *  order (a later mod replaces the files of an earlier one) with a switch, move up/down and delete,
 *  installs .zip and .7z archives (ModArchive) and .pkg mods (PkgInstaller), and on Apply writes mods.json
 *  and mods_enabled and restarts the game: the app (ModManager.apply) reads those when the game starts. Rows take the gamepad
 *  (D-pad, A, B) and touch. */
final class ModsDialog {
    static final int REQUEST_FOLDER=4, REQUEST_ARCHIVE=5, REQUEST_STORAGE=6;
    static final String DEFAULT_DIR="/storage/emulated/0/Bloodborne/mods";
    private static final int TEXT=0xffe8dfc9, GOLD=0xffc6ad76, WARN=0xffe5a04b;

    /** What the dialog needs from the activity. */
    interface Host {
        /** The mods folder (the mods_dir setting, else the default) with mods.json: list, validate. Cheap. */
        ModManager manager();
        /** The PKG installer: ps4-pkg-tool from the app's runtime, which is unpacked first when needed. Blocks:
         *  called off the UI thread. */
        PkgInstaller pkgInstaller() throws IOException;
        /** The current game folder (an eboot.bin in a PKG is compared with it), or null. */
        String gameDir();
        /** The file path of a picked document (shared storage), or null when it has none to read. */
        File pathOf(Uri document);
        boolean hasStorage();
        void requestStorage();
        /** Starts a picker; the result comes back to onResult. */
        void pick(Intent intent,int code);
        File folderOf(Uri tree);
        /** bbport.ini values. */
        String setting(String key);
        boolean set(String key,String value);
        /** The bbport folder (mods.json). */
        File baseDir();
        void toast(String message);
        void restart();
        void closed();
        boolean destroyed();
    }

    private static final class Mod {
        String name, problem; boolean enabled, valid; int files; long bytes;
        /** Files shared with another mod, by its name (from the runtime's overrides / overriddenBy). */
        final Map<String,Integer> shared=new HashMap<>();
    }
    private static final class Listing {
        String dir, error; boolean exists; final List<Mod> mods=new ArrayList<>();
    }

    private final Activity activity;
    private final Host host;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final AtomicBoolean cancel=new AtomicBoolean();
    private volatile boolean closed=true;
    // The last list, for the drawer's second line:
    private volatile Listing last;
    private volatile long lastAt;
    private boolean pendingShow;
    // UI-thread state behind the dialog:
    private AlertDialog dialog;
    private PkgProgressDialog progress;
    private LinearLayout rows;
    private TextView folderText, status;
    private Button useDefault;
    private Switch master;
    private List<Mod> mods=new ArrayList<>();
    private Listing shown;
    private List<String> baselineOrder;
    private Set<String> baselineOn;
    private boolean masterWas, dirty, loading, busy;

    ModsDialog(Activity activity,Host host) { this.activity=activity; this.host=host; }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private boolean live() { return !closed && !host.destroyed() && !activity.isFinishing(); }
    private void background(Runnable r,String name) { Thread t=new Thread(r,name); t.setDaemon(true); t.start(); }

    // ---- The list: ModManager.list ----
    private static Mod parse(ModManager.Mod o) {
        Mod m=new Mod();
        m.name=o.name; m.enabled=o.enabled; m.valid=o.valid; m.problem=o.problem; m.files=o.files; m.bytes=o.bytes;
        if (!m.valid && m.problem==null) m.problem="Not a usable mod";
        if (!m.valid) m.enabled=false;
        for (ModManager.Shared s:o.overrides) m.shared.put(s.mod,s.files);
        for (ModManager.Shared s:o.overriddenBy) m.shared.put(s.mod,s.files);
        return m;
    }
    /** Lists the mods folder. Blocks. */
    private Listing fetch() {
        Listing l=new Listing();
        try {
            ModManager.Listing o=host.manager().list();
            l.dir=o.modsDir; l.exists=o.exists; l.error=o.error;
            for (ModManager.Mod m:o.mods) l.mods.add(parse(m));
        } catch (Exception e) {
            android.util.Log.w("Bloodborne","mods list: "+e);
            l.error="The mods could not be read ("+e.getMessage()+").";
        }
        last=l; lastAt=SystemClock.elapsedRealtime();
        return l;
    }
    /** Checks one mod folder. Blocks. */
    private Mod validate(String name) throws Exception {
        return parse(host.manager().validate(name));
    }

    // ---- The drawer's row ----
    private static String describe(Listing l) {
        if (l==null) return "";
        if (l.error!=null) return "Could not read the mods";
        int usable=0, on=0, bad=0;
        for (Mod m:l.mods) { if (m.valid) { ++usable; if (m.enabled) ++on; } else ++bad; }
        if (l.mods.isEmpty()) return "No mods";
        return (usable>0 ? on+" of "+usable+" on" : "None usable")+(bad>0 ? " · "+bad+" not usable" : "");
    }
    /** The row's second line: now from the last list, then again from a fresh one. */
    void summary(Consumer<String> shownAs) {
        if ("0".equals(host.setting("mods_enabled"))) { shownAs.accept("Off"); return; }
        Listing l=last;
        shownAs.accept(describe(l));
        if (l!=null && SystemClock.elapsedRealtime()-lastAt<2000) return;
        background(() -> {
            String text=describe(fetch());
            main.post(() -> { if (!host.destroyed()) shownAs.accept(text); });
        },"Mods-summary");
    }

    // ---- The dialog ----
    void show() {
        if (!host.hasStorage()) { pendingShow=true; host.requestStorage(); return; }
        close(); closed=false; cancel.set(false);
        shown=null; mods=new ArrayList<>(); baselineOrder=null; baselineOn=null; dirty=false; busy=false;
        masterWas=!"0".equals(host.setting("mods_enabled"));

        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(8),dp(24),dp(8));
        master=new Switch(activity); master.setText("Use mods"); master.setTextSize(15);
        master.setChecked(masterWas); master.setPadding(0,dp(6),0,dp(6));
        box.addView(master);
        TextView note=new TextView(activity); note.setTextSize(13); note.setAlpha(0.8f);
        note.setText("Mods must be made for Bloodborne 1.09. Mods that change the game's files take effect when the game restarts.");
        box.addView(note);
        folderText=new TextView(activity); folderText.setTextSize(13); folderText.setTextColor(TEXT);
        folderText.setPadding(0,dp(10),0,0);
        box.addView(folderText);
        LinearLayout folderButtons=new LinearLayout(activity); folderButtons.setOrientation(LinearLayout.HORIZONTAL);
        Button change=new Button(activity); change.setAllCaps(false); change.setText("Change folder…");
        change.setOnClickListener(v -> pickFolder());
        useDefault=new Button(activity); useDefault.setAllCaps(false); useDefault.setText("Use default");
        useDefault.setOnClickListener(v -> resetFolder());
        folderButtons.addView(change); folderButtons.addView(useDefault);
        box.addView(folderButtons);
        status=new TextView(activity); status.setTextSize(13); status.setTextColor(GOLD);
        status.setPadding(0,dp(8),0,dp(4));
        box.addView(status);
        rows=new LinearLayout(activity); rows.setOrientation(LinearLayout.VERTICAL);
        box.addView(rows,new LinearLayout.LayoutParams(-1,-2));
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);

        dialog=new AlertDialog.Builder(activity).setTitle("Mods").setView(scroll)
            .setPositiveButton("Apply & restart",null).setNeutralButton("Install mod…",null)
            .setNegativeButton("Close",null).create();
        dialog.setOnKeyListener((d,code,e) -> { // B closes it, A taps the focused control, like the drawer
            if (code==KeyEvent.KEYCODE_BUTTON_B || e.getScanCode()==305) {
                if (e.getAction()==KeyEvent.ACTION_UP && !busy) dialog.dismiss();
                return true;
            }
            if (code==KeyEvent.KEYCODE_BUTTON_A || e.getScanCode()==304) {
                View f=dialog.getCurrentFocus();
                if (e.getAction()==KeyEvent.ACTION_UP && f!=null) f.performClick();
                return true;
            }
            return false;
        });
        dialog.setOnDismissListener(d -> { close(); host.closed(); });
        dialog.show();
        // Set after show(): these must not close the dialog by themselves.
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> apply());
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> pickArchive());
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> dialog.dismiss());
        reload();
    }

    /** Ends everything (also when the activity is destroyed): cancels an install, closes the dialogs. */
    void close() {
        closed=true; cancel.set(true);
        if (progress!=null) progress.dismiss();
        try { if (dialog!=null && dialog.isShowing()) dialog.dismiss(); } catch (Exception ignored) {}
    }

    /** The storage permission answer (the dialog opens once it is granted). */
    void storageResult(boolean granted) {
        boolean wanted=pendingShow; pendingShow=false;
        if (!wanted) return;
        if (granted) show(); else host.toast("Mods need storage access to read the mods folder.");
    }

    /** Picker results: the mods folder, the archive to install. */
    void onResult(int code,int result,Intent data) {
        if (!live() || result!=Activity.RESULT_OK || data==null || data.getData()==null) return;
        if (code==REQUEST_FOLDER) {
            File dir=host.folderOf(data.getData());
            if (dir==null) { host.toast("That folder cannot be used."); return; }
            setFolder(dir.getPath().equals(DEFAULT_DIR) ? "" : dir.getPath());
        } else if (code==REQUEST_ARCHIVE) install(data.getData(),true);
    }

    private void pickFolder() {
        try { host.pick(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE),REQUEST_FOLDER); }
        catch (Exception ex) { host.toast("No folder picker: "+ex); }
    }
    private void resetFolder() { setFolder(""); }
    private void setFolder(String path) {
        if (!host.set("mods_dir",path)) { host.toast("Could not save the mods folder."); return; }
        mods=new ArrayList<>(); baselineOrder=null; baselineOn=null; dirty=true;
        reload();
    }
    private void pickArchive() {
        if (busy || shown==null || shown.error!=null) return;
        Intent pick=new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
            .putExtra(Intent.EXTRA_MIME_TYPES,new String[]{"application/zip","application/x-zip-compressed",
                "application/x-7z-compressed","application/octet-stream"}); // .pkg is octet-stream; found by its first bytes
        try { host.pick(pick,REQUEST_ARCHIVE); }
        catch (Exception ex) { host.toast("No file picker: "+ex); }
    }

    /** Lists the mods folder (creating it first) and shows the answer. */
    private void reload() {
        loading=true; render();
        background(() -> {
            String configured=host.setting("mods_dir");
            File folder=new File(configured==null || configured.isEmpty() ? DEFAULT_DIR : configured);
            if (!folder.isDirectory()) folder.mkdirs();
            Listing l=fetch();
            main.post(() -> {
                if (!live()) return;
                loading=false; shown=l;
                if (l.error==null) merge(l);
                render();
            });
        },"Mods-list");
    }
    /** New list in, with what the player has changed in the dialog kept (order, switches). */
    private void merge(Listing l) {
        Map<String,Mod> fresh=new HashMap<>();
        for (Mod m:l.mods) fresh.put(m.name,m);
        List<Mod> next=new ArrayList<>();
        for (Mod old:mods) {
            Mod m=fresh.remove(old.name);
            if (m!=null) { m.enabled=m.valid && old.enabled; next.add(m); }
        }
        for (Mod m:l.mods) if (fresh.containsKey(m.name)) next.add(m);
        mods=next;
        if (baselineOrder==null) {
            baselineOrder=new ArrayList<>(); baselineOn=new HashSet<>();
            for (Mod m:l.mods) { baselineOrder.add(m.name); if (m.enabled) baselineOn.add(m.name); }
        }
    }
    private boolean changed() {
        if (dirty || master.isChecked()!=masterWas) return true;
        if (baselineOrder==null) return false;
        List<String> order=new ArrayList<>(); Set<String> on=new HashSet<>();
        for (Mod m:mods) { order.add(m.name); if (m.enabled) on.add(m.name); }
        return !order.equals(baselineOrder) || !on.equals(baselineOn);
    }

    // ---- Rows ----
    private static String plural(int n,String word) { return n+" "+word+(n==1 ? "" : "s"); }
    private static String size(long bytes) {
        if (bytes>=1L<<30) return String.format(java.util.Locale.ROOT,"%.1f GB",bytes/1073741824.0);
        if (bytes>=1L<<20) return Math.round(bytes/1048576.0)+" MB";
        return Math.max(1,Math.round(bytes/1024.0))+" KB";
    }
    /** Line 2: size, then the files this mod replaces (earlier mods) or that later mods replace. */
    private String detail(Mod m,int index) {
        if (m.problem!=null) return m.problem;
        List<String> parts=new ArrayList<>();
        parts.add(plural(m.files,"file")+" · "+size(m.bytes));
        if (m.enabled) {
            List<String> replaces=new ArrayList<>(), replaced=new ArrayList<>();
            for (int i=0;i<mods.size();++i) {
                Mod o=mods.get(i);
                if (i==index || !o.enabled) continue;
                Integer a=m.shared.get(o.name), b=o.shared.get(m.name);
                int n=Math.max(a==null ? 0 : a,b==null ? 0 : b);
                if (n==0) continue;
                (i<index ? replaces : replaced).add(i<index ? plural(n,"file")+" of "+o.name : plural(n,"file")+" replaced by "+o.name);
            }
            if (!replaces.isEmpty()) parts.add("replaces "+String.join(", ",replaces));
            parts.addAll(replaced);
        }
        return String.join(" · ",parts);
    }

    /** A small focusable text button (gold when focused or pressed). */
    private TextView iconButton(String label,String description,boolean enabled,Runnable action) {
        TextView b=new TextView(activity); b.setText(label); b.setTextSize(18); b.setGravity(Gravity.CENTER);
        b.setTextColor(TEXT); b.setContentDescription(description);
        b.setMinWidth(dp(40)); b.setMinHeight(dp(40));
        GradientDrawable lit=new GradientDrawable(); lit.setColor(0x33c6ad76); lit.setCornerRadius(dp(6));
        lit.setStroke(dp(1),0x99c6ad76);
        StateListDrawable background=new StateListDrawable();
        background.addState(new int[]{android.R.attr.state_focused},lit);
        background.addState(new int[]{android.R.attr.state_pressed},lit);
        b.setBackground(background);
        b.setEnabled(enabled); b.setAlpha(enabled ? 1f : 0.3f);
        b.setFocusable(enabled); b.setClickable(enabled);
        if (enabled) b.setOnClickListener(v -> action.run());
        return b;
    }

    /** Rebuilds the text and rows from the state; the focused control stays focused. */
    private void render() {
        if (dialog==null || !dialog.isShowing()) return;
        View focused=dialog.getCurrentFocus();
        String focusKey=focused!=null && focused.getTag() instanceof String ? (String)focused.getTag() : null;
        rows.removeAllViews();
        String dir=shown!=null && shown.dir!=null ? shown.dir : null;
        String configured=host.setting("mods_dir");
        if (dir==null) dir=configured==null || configured.isEmpty() ? DEFAULT_DIR : configured;
        folderText.setText("Mods folder: "+dir);
        useDefault.setEnabled(configured!=null && !configured.isEmpty());
        boolean usable=shown!=null && shown.error==null;
        status.setTextColor(usable ? GOLD : WARN);
        if (loading && shown==null) status.setText("Loading the mods…");
        else if (shown!=null && shown.error!=null) status.setText(shown.error);
        else if (shown!=null && !shown.exists) status.setText("The mods folder does not exist and could not be created.");
        else if (mods.isEmpty()) status.setText("No mods yet. Install one, or put a mod's folder in the mods folder.");
        else status.setText("");
        status.setVisibility(status.getText().length()==0 ? View.GONE : View.VISIBLE);
        for (int i=0;i<mods.size();++i) rows.addView(row(mods.get(i),i));
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(usable && !busy);
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(usable && !busy);
        if (focusKey!=null) {
            View again=rows.findViewWithTag(focusKey);
            // A button that went inert (a mod now at the end): its neighbour takes the focus.
            if (again==null || !again.isFocusable()) {
                String name=focusKey.substring(0,focusKey.lastIndexOf('#'));
                again=rows.findViewWithTag(name+"#up");
                if (focusKey.endsWith("#up") || again==null || !again.isFocusable()) again=rows.findViewWithTag(name+"#down");
                if (again==null || !again.isFocusable()) again=rows.findViewWithTag(name+"#on");
            }
            if (again!=null && again.isFocusable()) again.requestFocus();
        }
    }

    private View row(Mod m,int index) {
        LinearLayout row=new LinearLayout(activity); row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL); row.setPadding(dp(4),dp(8),dp(0),dp(8));
        LinearLayout texts=new LinearLayout(activity); texts.setOrientation(LinearLayout.VERTICAL);
        TextView title=new TextView(activity); title.setTextSize(16); title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(TEXT); title.setText(m.name); title.setAlpha(m.enabled ? 1f : 0.55f);
        TextView sub=new TextView(activity); sub.setTextSize(13); sub.setText(detail(m,index));
        sub.setTextColor(m.problem!=null ? WARN : TEXT); sub.setAlpha(m.problem!=null ? 1f : m.enabled ? 0.75f : 0.45f);
        texts.addView(title); texts.addView(sub);
        texts.setLongClickable(true); texts.setOnLongClickListener(v -> { askDelete(m); return true; });
        row.addView(texts,new LinearLayout.LayoutParams(0,-2,1));

        Switch on=new Switch(activity); on.setChecked(m.enabled); on.setEnabled(m.valid);
        on.setContentDescription("Use "+m.name); on.setTag(m.name+"#on");
        on.setOnCheckedChangeListener((b,checked) -> { m.enabled=checked; render(); });
        LinearLayout.LayoutParams sp=new LinearLayout.LayoutParams(-2,-2); sp.leftMargin=dp(8); sp.rightMargin=dp(4);
        row.addView(on,sp);
        TextView up=iconButton("▲","Move "+m.name+" up",index>0,() -> move(index,-1));
        up.setTag(m.name+"#up");
        TextView down=iconButton("▼","Move "+m.name+" down",index<mods.size()-1,() -> move(index,1));
        down.setTag(m.name+"#down");
        TextView more=iconButton("…","More for "+m.name,true,() -> new AlertDialog.Builder(activity).setTitle(m.name)
            .setItems(new String[]{"Delete mod…"},(d,which) -> askDelete(m)).setNegativeButton("Cancel",null).show());
        more.setTag(m.name+"#more");
        row.addView(up); row.addView(down); row.addView(more);
        return row;
    }
    private void move(int index,int by) {
        int to=index+by;
        if (to<0 || to>=mods.size()) return;
        Mod m=mods.remove(index); mods.add(to,m);
        render();
    }

    // ---- Delete ----
    private void askDelete(Mod m) {
        if (busy || shown==null || shown.dir==null) return;
        new AlertDialog.Builder(activity).setTitle("Delete "+m.name+"?")
            .setMessage("This deletes the mod's folder and its files from "+shown.dir+". It cannot be undone.")
            .setNegativeButton("Cancel",null).setPositiveButton("Delete",(d,w) -> delete(m.name)).show();
    }
    /** Removes mods folder/name, only when it is a direct child of the mods folder. */
    private void delete(String name) {
        final String dir=shown==null ? null : shown.dir;
        if (dir==null) return;
        busy=true;
        background(() -> {
            String failure=null;
            try {
                File base=new File(dir).getCanonicalFile(), mod=new File(base,name);
                if (name.isEmpty() || name.equals(".") || name.equals("..") || name.indexOf('/')>=0 || name.indexOf('\\')>=0)
                    throw new IOException("Not a mod folder name: "+name);
                if (java.nio.file.Files.isSymbolicLink(mod.toPath())) mod.delete(); // only the link, never what it points at
                else {
                    if (!base.equals(mod.getCanonicalFile().getParentFile())) throw new IOException(name+" is not inside the mods folder.");
                    ModArchive.deleteTree(mod);
                }
                if (mod.exists()) throw new IOException("Some files could not be deleted.");
            } catch (IOException e) { failure=e.getMessage(); }
            String problem=failure;
            main.post(() -> {
                busy=false;
                if (!live()) return;
                if (problem!=null) host.toast("Could not delete "+name+": "+problem);
                else { dirty=true; last=null; }
                mods.removeIf(m -> m.name.equals(name));
                reload();
            });
        },"Mods-delete");
    }

    // ---- Install ----
    /** `check`: a .pkg is first checked against the checksums stored in it ("Install anyway" skips that). */
    private void install(Uri uri,boolean check) {
        if (busy || shown==null || shown.dir==null) return;
        String archiveName="mod.zip"; long total=-1;
        try (android.database.Cursor c=activity.getContentResolver().query(uri,
                new String[]{android.provider.OpenableColumns.DISPLAY_NAME,android.provider.OpenableColumns.SIZE},null,null,null)) {
            if (c!=null && c.moveToFirst()) {
                if (c.getString(0)!=null) archiveName=c.getString(0);
                if (!c.isNull(1)) total=c.getLong(1);
            }
        } catch (Exception ignored) {}
        final String name=archiveName; final long size=total; final File modsDir=new File(shown.dir);
        busy=true; cancel.set(false); render();
        progress=new PkgProgressDialog(activity,"Install mod","Reading "+name+"…",cancel);
        // A phase's text, and its bar (total unknown: no bar).
        class Phase implements ModArchive.Progress {
            final String text;
            Phase(String text) { this.text=text; }
            @Override public void update(long done,long of) {
                main.post(() -> { if (live() && !cancel.get()) progress.fraction(text,done,of); });
            }
        }
        background(() -> {
            File tmp=null, target=null; Mod verdict=null; int files=0;
            String failure=null, unchecked=null, damage=null; boolean cancelled=false, installed=false;
            try {
                // The file itself when its path is readable; else a copy in the cache.
                File source=host.pathOf(uri);
                if (source==null || !source.isFile() || !source.canRead()) {
                    File cache=activity.getCacheDir();
                    if (size>0 && size+(32L<<20)>new StatFs(cache.getPath()).getAvailableBytes())
                        throw new IOException("Not enough free space on the phone to unpack "+name+".");
                    tmp=File.createTempFile("mod-",".archive",cache);
                    try (InputStream in=activity.getContentResolver().openInputStream(uri)) {
                        if (in==null) throw new IOException("Cannot open "+name);
                        ModArchive.copy(in,tmp,size,new Phase("Reading "+name),cancel::get);
                    }
                    source=tmp;
                }
                String kind=ModArchive.kind(source);
                if (kind==null) throw new ModArchive.Rejected(ModArchive.UNSUPPORTED);
                if (!modsDir.isDirectory() && !modsDir.mkdirs()) throw new IOException("Cannot create the mods folder "+modsDir);
                target=ModArchive.uniqueDir(modsDir,ModArchive.modName(name));
                if ("pkg".equals(kind)) {
                    // ps4-pkg-tool unpacks it (PkgInstaller); the progress lines come as bytes done of total.
                    main.post(() -> progress.indeterminate("Unpacking "+name+"…"));
                    PkgProgress track=new PkgProgress();
                    PkgInstaller installer=host.pkgInstaller();
                    String gameFolder=host.gameDir();
                    PkgInstaller.ModResult o=installer.installMod(source,target.getName(),modsDir,
                        gameFolder==null || gameFolder.isEmpty() ? null : new File(gameFolder),(done,of,label) -> {
                            track.update(done,of,label,SystemClock.elapsedRealtime());
                            String text=track.detail(SystemClock.elapsedRealtime()), what=track.label; int permille=track.permille();
                            main.post(() -> { if (live() && !cancel.get()) progress.pkg(what,"Unpacking "+name+"…",permille,text); });
                        },cancel::get,check);
                    if (cancel.get()) throw new CancellationException();
                    if (o.damaged!=null) damage=o.damaged;
                    else if (!o.ok) throw new ModArchive.Rejected(o.problem==null ? "The .pkg could not be installed." : o.problem);
                    if (damage==null) {
                        String made=o.name!=null ? o.name : target.getName();
                        target=new File(modsDir,made); installed=true;
                        if (o.mod!=null) verdict=parse(o.mod);
                        else try { verdict=validate(made); } catch (Exception e) { unchecked=e.getMessage()!=null ? e.getMessage() : e.toString(); }
                        if (verdict!=null) files=verdict.files;
                    }
                } else {
                    main.post(() -> progress.indeterminate("Unpacking "+name+"…"));
                    ModArchive.Result result=ModArchive.extract(source,target,new StatFs(modsDir.getPath()).getAvailableBytes(),new Phase("Unpacking"),cancel::get);
                    installed=true; files=result.files;
                    main.post(() -> progress.locked("Checking the mod…"));
                    try { verdict=validate(target.getName()); }
                    catch (Exception e) { unchecked=e.getMessage()!=null ? e.getMessage() : e.toString(); }
                }
            } catch (CancellationException e) {
                cancelled=true;
            } catch (Exception e) {
                failure=e.getMessage()!=null ? e.getMessage() : e.toString();
                if (!(e instanceof ModArchive.Rejected)) android.util.Log.e("Bloodborne","mod install: "+e);
            } finally {
                if (tmp!=null) tmp.delete();
            }
            // Nothing half-installed stays (the zip/7z unpacker already removed its folder).
            if (!installed && target!=null && target.isDirectory()) ModArchive.deleteTree(target);
            final Mod checked=verdict; final boolean done=installed; final int count=files; final File dest=target;
            final String problem=failure, notChecked=unchecked, damaged=damage; final boolean stopped=cancelled;
            main.post(() -> {
                progress.dismiss();
                busy=false;
                if (!live()) return;
                if (done) { dirty=true; last=null; }
                reload();
                if (stopped) { host.toast("Install cancelled."); return; }
                if (damaged!=null) { damagedDialog(uri,name,damaged); return; }
                if (problem!=null) { alert("Mod not installed",problem); return; }
                if (checked==null) { alert("Mod installed",dest.getName()+" was unpacked, but could not be checked: "+notChecked); return; }
                if (!checked.valid || checked.problem!=null) {
                    new AlertDialog.Builder(activity).setTitle("Not a usable mod")
                        .setMessage(dest.getName()+" was unpacked, but cannot be used:\n\n"+checked.problem)
                        .setPositiveButton("Delete it",(d,w) -> delete(dest.getName()))
                        .setNegativeButton("Keep it",null).show();
                    return;
                }
                new AlertDialog.Builder(activity).setTitle("Mod installed")
                    .setMessage(dest.getName()+" ("+plural(count,"file")+") is on. It loads when the game restarts.")
                    .setNegativeButton("Later",null).setPositiveButton("Restart now",(d,w) -> apply()).show();
            });
        },"Mods-install");
    }
    /** The PKG does not match its own checksums: which parts, and Pick again / Cancel / Install anyway (no check). */
    private void damagedDialog(Uri uri,String name,String parts) {
        new AlertDialog.Builder(activity).setTitle("This PKG is damaged").setCancelable(false)
            .setMessage(name+" doesn't match the checksums stored in it:\n\n"+parts
                +"\n\nThe file is damaged or incomplete (a download or copy that went wrong). Download or copy it again, "
                +"then pick it. Installing it as it is may fail or give a broken mod.")
            .setPositiveButton("Pick again",(d,w) -> pickArchive())
            .setNegativeButton("Cancel",null)
            .setNeutralButton("Install anyway",(d,w) -> install(uri,false)).show();
    }
    private void alert(String title,String message) {
        new AlertDialog.Builder(activity).setTitle(title).setMessage(message).setPositiveButton("OK",null).show();
    }

    // ---- Apply ----
    private void apply() {
        if (busy) return;
        if (!changed()) { dialog.dismiss(); return; }
        boolean ok=host.set("mods_enabled",master.isChecked() ? "1" : "0");
        if (shown!=null && shown.error==null) ok&=writeConfig();
        if (!ok) { host.toast("Could not save the mod settings."); return; }
        last=null; lastAt=0;
        dialog.dismiss();
        host.restart();
    }
    /** mods.json: every mod in load order, and the ones switched off (a mod that cannot be used is
     *  never listed as off by the player). Written whole and synced, then renamed. */
    private boolean writeConfig() {
        try {
            JSONArray order=new JSONArray(), disabled=new JSONArray();
            for (Mod m:mods) { order.put(m.name); if (m.valid && !m.enabled) disabled.put(m.name); }
            JSONObject o=new JSONObject(); o.put("order",order); o.put("disabled",disabled);
            SafeWrite.write(new File(host.baseDir(),"mods.json"),o.toString());
            return true;
        } catch (Exception e) {
            android.util.Log.e("Bloodborne","mods.json: "+e);
            return false;
        }
    }
}
