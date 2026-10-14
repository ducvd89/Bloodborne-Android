package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.os.SystemClock;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

/** Installs the game from its PKG files (the runtime's install-game, one PKG per run): the base game
 *  first, then the 1.09 update into the same destination. Picks the PKG (a real path: multi-GB files
 *  are never copied), asks where to install, runs the tool under a progress dialog (bytes, rate, time
 *  left; Cancel destroys the process) and reports the result. When the game folder is ready the
 *  activity starts it. */
final class GameInstaller {
    /** The runtime tool's script (bbport/): install-game may move to its own script, change it here. */
    static final String SCRIPT=ModsDialog.SCRIPT;
    static final int REQUEST_PKG=7, REQUEST_DEST=8;
    static final String DEFAULT_DEST="/storage/emulated/0/Bloodborne";
    /** A PKG whose path cannot be read is copied only when it is smaller than this. */
    static final long COPY_LIMIT=1L<<30;

    interface Host extends ModsDialog.Host {
        /** The current game folder, or null. */
        String gameDir();
        boolean gameRunning();
        /** Stops the running game; blocks (called off the UI thread). */
        void stopGame();
        /** The folder has the game ready: make it the game folder and start (or restart) the game. */
        void useGame(File dir);
        /** The flow ended without a ready game; `stopped`: the installer had stopped the running game. */
        void installEnded(boolean stopped);
    }

    private final Activity activity;
    private final Host host;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final AtomicBoolean cancel=new AtomicBoolean();
    private volatile boolean closed=true;
    private PkgProgressDialog progress;
    private String dest=DEFAULT_DEST;
    private File pending; private boolean pendingTemp, destChosen, stopped, busy;

    GameInstaller(Activity activity,Host host) { this.activity=activity; this.host=host; }

    boolean active() { return !closed; }
    private boolean live() { return !closed && !host.destroyed() && !activity.isFinishing(); }
    private void background(Runnable r,String name) { Thread t=new Thread(r,name); t.setDaemon(true); t.start(); }
    private void end() { if (!closed) { closed=true; host.installEnded(stopped); } }
    private void alert(String title,String message,boolean pickAgain) {
        AlertDialog.Builder b=new AlertDialog.Builder(activity).setTitle(title).setMessage(message).setCancelable(false);
        if (pickAgain) b.setPositiveButton("Pick again",(d,w) -> pickPkg()).setNegativeButton("Cancel",(d,w) -> end());
        else b.setPositiveButton("OK",(d,w) -> end());
        b.show();
    }

    /** Explains, then starts with the first PKG. */
    void start() {
        if (busy) return;
        closed=false; cancel.set(false); dest=DEFAULT_DEST; destChosen=false; stopped=false; pending=null;
        if (!host.hasStorage()) { host.toast("Installing needs storage access."); end(); return; }
        new AlertDialog.Builder(activity).setTitle("Install from PKG")
            .setMessage("Pick the game's PKG. Bloodborne needs update 1.09: if your PKG is the base game, you'll pick the "
                +"update PKG next. You need free space about the size of the PKG.")
            .setCancelable(false).setNegativeButton("Cancel",(d,w) -> end())
            .setPositiveButton("Pick PKG…",(d,w) -> pickPkg()).show();
    }
    void close() {
        closed=true; cancel.set(true);
        if (progress!=null) progress.dismiss();
    }

    private void pickPkg() {
        try { host.pick(new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*"),REQUEST_PKG); }
        catch (Exception ex) { host.toast("No file picker: "+ex); end(); }
    }

    /** Picker results: the PKG, the destination folder. */
    void onResult(int code,int result,Intent data) {
        if (closed) return;
        boolean ok=result==Activity.RESULT_OK && data!=null && data.getData()!=null;
        if (code==REQUEST_PKG) { if (ok) resolve(data.getData()); else end(); }
        else if (code==REQUEST_DEST) {
            if (ok) {
                File dir=host.folderOf(data.getData());
                if (dir==null) host.toast("That folder cannot be used."); else dest=dir.getPath();
            }
            askDestination();
        }
    }

    /** The PKG as a readable file: its own path, or a copy in the cache when it is small. */
    private void resolve(Uri uri) {
        File own=host.pathOf(uri);
        if (own!=null && own.isFile() && own.canRead()) { picked(own,false); return; }
        String name="game.pkg"; long size=-1;
        try (android.database.Cursor c=activity.getContentResolver().query(uri,
                new String[]{android.provider.OpenableColumns.DISPLAY_NAME,android.provider.OpenableColumns.SIZE},null,null,null)) {
            if (c!=null && c.moveToFirst()) {
                if (c.getString(0)!=null) name=c.getString(0);
                if (!c.isNull(1)) size=c.getLong(1);
            }
        } catch (Exception ignored) {}
        final String label=name; final long total=size;
        if (total<0 || total>COPY_LIMIT) {
            alert("Move the PKG first","The app cannot read this file where it is, and it is too large to copy. Move the PKG into the "
                +"phone's storage (for example the Download folder), then pick it again from there.",true);
            return;
        }
        File cache=activity.getCacheDir();
        if (total+(64L<<20)>new StatFs(cache.getPath()).getAvailableBytes()) {
            alert("Not enough space","There is not enough free space to copy "+label+". Move the PKG into the phone's storage "
                +"(for example the Download folder), then pick it again from there.",true);
            return;
        }
        busy=true; cancel.set(false);
        progress=new PkgProgressDialog(activity,"Install from PKG","Reading "+label+"…",cancel);
        background(() -> {
            File tmp=null; boolean cancelled=false; String failure=null;
            try {
                tmp=File.createTempFile("game-",".pkg",cache);
                try (InputStream in=activity.getContentResolver().openInputStream(uri)) {
                    if (in==null) throw new IOException("Cannot open "+label);
                    ModArchive.copy(in,tmp,total,(d,t) -> main.post(() -> { if (live() && !cancel.get()) progress.fraction("Reading "+label,d,t); }),cancel::get);
                }
            } catch (CancellationException e) { cancelled=true;
            } catch (Exception e) { failure=e.getMessage()!=null ? e.getMessage() : e.toString(); }
            File copy=tmp; String problem=failure; boolean stop=cancelled;
            if ((stop || problem!=null) && copy!=null) copy.delete();
            main.post(() -> {
                progress.dismiss(); busy=false;
                if (!live()) { if (copy!=null) copy.delete(); return; }
                if (stop) { if (copy!=null) copy.delete(); end(); }
                else if (problem!=null) alert("Could not read the PKG",problem,false);
                else picked(copy,true);
            });
        },"Game-copy");
    }

    private void picked(File pkg,boolean temporary) {
        if (!"pkg".equals(ModArchive.kind(pkg))) {
            if (temporary) pkg.delete();
            alert("Not a PKG","That file is not a PS4 .pkg package.",true); return;
        }
        pending=pkg; pendingTemp=temporary;
        if (destChosen) run(); else askDestination();
    }

    private void askDestination() {
        if (pending==null) { end(); return; }
        new AlertDialog.Builder(activity).setTitle("Install to").setCancelable(false)
            .setMessage("The game folder is created inside:\n"+dest+"\n\nKeep the base game's and the update's destination the same.")
            .setNegativeButton("Cancel",(d,w) -> { discard(); end(); })
            .setNeutralButton("Change…",(d,w) -> {
                try { host.pick(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE),REQUEST_DEST); }
                catch (Exception ex) { host.toast("No folder picker: "+ex); askDestination(); }
            })
            .setPositiveButton("Install",(d,w) -> { destChosen=true; confirmStop(); }).show();
    }
    private void discard() { if (pending!=null && pendingTemp) pending.delete(); pending=null; }

    /** The running game blocks a destination that holds its folder: ask, then stop it. */
    private void confirmStop() {
        if (holdsGame())
            new AlertDialog.Builder(activity).setTitle("Stop the game?").setCancelable(false)
                .setMessage("The game will close while installing.")
                .setNegativeButton("Cancel",(d,w) -> { discard(); end(); })
                .setPositiveButton("Continue",(d,w) -> run()).show();
        else run();
    }

    /** The game is running from a folder inside the destination. */
    private boolean holdsGame() {
        String current=host.gameDir();
        return host.gameRunning() && current!=null && (current.equals(dest) || current.startsWith(dest.endsWith("/") ? dest : dest+"/"));
    }
    private void run() {
        final File pkg=pending; final boolean temporary=pendingTemp; pending=null;
        busy=true; cancel.set(false);
        progress=new PkgProgressDialog(activity,"Install game","Starting…",cancel);
        background(() -> {
            JSONObject result=null; String failure=null; boolean cancelled=false;
            try {
                if (holdsGame()) { stopped=true; host.stopGame(); }
                PkgProgress track=new PkgProgress();
                String[] answer={null}; StringBuilder other=new StringBuilder();
                boolean known=host.scriptStream(SCRIPT,line -> {
                    line=line.trim();
                    long now=SystemClock.elapsedRealtime();
                    if (track.feed(line,now)) {
                        String label=track.label, text=track.detail(now); int permille=track.permille();
                        main.post(() -> { if (live() && !cancel.get()) progress.pkg(label,"Installing the game…",permille,text); });
                    } else if (line.startsWith("{")) answer[0]=line;
                    else if (!line.isEmpty() && other.length()<400) other.append(line).append('\n');
                },cancel,"install-game","--dest",dest,"--pkg",pkg.getAbsolutePath());
                if (!known) throw new IOException("Update the app's runtime to install from PKG");
                if (cancel.get()) throw new CancellationException();
                if (answer[0]==null) {
                    if (other.indexOf("usage:")>=0) throw new IOException("Update the app's runtime to install from PKG");
                    throw new IOException("The PKG could not be installed"+(other.length()>0 ? ": "+other.toString().trim() : "."));
                }
                result=ModsDialog.json(answer[0]);
            } catch (CancellationException e) { cancelled=true;
            } catch (Exception e) {
                failure=e.getMessage()!=null ? e.getMessage() : e.toString();
                android.util.Log.e("Bloodborne","game install: "+e);
            } finally { if (temporary) pkg.delete(); }
            JSONObject o=result; String problem=failure; boolean stop=cancelled;
            main.post(() -> {
                progress.dismiss(); busy=false;
                if (!live()) return;
                if (stop) { host.toast("Install cancelled."); end(); }
                else if (problem!=null) alert("Not installed",problem,false);
                else if (!o.optBoolean("ok")) alert("Not installed",o.isNull("problem") ? "The PKG could not be installed." : o.optString("problem"),false);
                else finished(o);
            });
        },"Game-install");
    }

    private void finished(JSONObject o) {
        String where=o.optString("gameDir");
        if (o.optBoolean("ready") && !where.isEmpty()) {
            closed=true; host.useGame(new File(where)); return;
        }
        String what=o.optString("titleId","the game")+(o.isNull("appVer") || o.optString("appVer").isEmpty() ? "" : " "+o.optString("appVer"))+" is installed"
            +(where.isEmpty() ? "." : " in "+where+".");
        String next=o.isNull("next") || o.optString("next").isEmpty() ? "The game is not ready yet: install the 1.09 update PKG." : o.optString("next");
        new AlertDialog.Builder(activity).setTitle("Next: the update").setMessage(what+"\n\n"+next).setCancelable(false)
            .setNegativeButton("Later",(d,w) -> end())
            .setPositiveButton("Pick update PKG…",(d,w) -> pickPkg()).show();
    }
}
