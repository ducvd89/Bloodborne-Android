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

/** Installs the game from its PKG files (PkgInstaller.installGame, one PKG per run): the base game
 *  first, then the 1.09 update into the same destination. Picks the PKG (a real path: multi-GB files are
 *  never copied), then asks where to install it (the tool makes the CUSAxxxxx folder inside), runs the tool
 *  under a progress dialog (bytes, rate, time left; Cancel destroys the process) and reports the result. When the game folder is ready the
 *  activity starts it. */
final class GameInstaller {
    static final int REQUEST_PKG=7, REQUEST_DEST=8;
    static final String DEFAULT_DEST="/storage/emulated/0/Bloodborne";
    /** A PKG whose path cannot be read is copied only when it is smaller than this. */
    static final long COPY_LIMIT=1L<<30;

    interface Host extends ModsDialog.Host {
        boolean gameRunning();
        /** Stops the running game; blocks (called off the UI thread). */
        void stopGame();
        /** The folder has the game ready: make it the game folder and start (or restart) the game. */
        void useGame(File dir);
        /** Whether the native code (libbbaot.so) that fits the game is installed: without it the game cannot start. */
        boolean nativeReady();
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
    private File pending; private String pendingName=""; private boolean pendingTemp, destChosen, stopped, busy, skipCheck;

    GameInstaller(Activity activity,Host host) { this.activity=activity; this.host=host; }

    private boolean live() { return !closed && !host.destroyed() && !activity.isFinishing(); }
    private void background(Runnable r,String name) { Thread t=new Thread(r,name); t.setDaemon(true); t.start(); }
    private void end() { if (!closed) { closed=true; host.installEnded(stopped); } }
    private void alert(String title,String message,boolean pickAgain) {
        AlertDialog.Builder b=new AlertDialog.Builder(activity).setTitle(title).setMessage(message).setCancelable(false);
        if (pickAgain) b.setPositiveButton("Pick again",(d,w) -> pickPkg()).setNegativeButton("Cancel",(d,w) -> end());
        else b.setPositiveButton("OK",(d,w) -> end());
        b.show();
    }

    /** Picks the PKG; where to install it is asked next. */
    void start() {
        if (busy) return;
        closed=false; cancel.set(false); dest=DEFAULT_DEST; destChosen=false; stopped=false; pending=null; skipCheck=false;
        if (!host.hasStorage()) { host.toast("Installing needs storage access."); end(); return; }
        pickPkg();
    }
    /** The install flow is open (from start() until it ends). */
    boolean active() { return !closed; }
    void close() {
        closed=true; cancel.set(true);
        if (progress!=null) progress.dismiss();
    }

    private void pickDest() {
        try { host.pick(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE),REQUEST_DEST); }
        catch (Exception ex) { host.toast("No folder picker: "+ex); discard(); end(); }
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
            if (!ok) { askDestination(); return; }
            File dir=host.folderOf(data.getData());
            if (dir==null) { host.toast("That folder cannot be used: choose one in the phone's storage or on the SD card."); askDestination(); return; }
            dest=dir.getPath(); destChosen=true;
            confirmStop();
        }
    }

    /** The PKG as a readable file: its own path, or a copy in the cache when it is small. */
    private void resolve(Uri uri) {
        File own=host.pathOf(uri);
        if (own!=null && own.isFile() && own.canRead()) { picked(own,false,own.getName()); return; }
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
                else picked(copy,true,label);
            });
        },"Game-copy");
    }

    private void picked(File pkg,boolean temporary,String name) {
        if (!"pkg".equals(ModArchive.kind(pkg))) {
            if (temporary) pkg.delete();
            alert("Not a PKG","That file is not a PS4 .pkg package.",true); return;
        }
        pending=pkg; pendingTemp=temporary;
        if (destChosen) confirmStop(); else { pendingName=name; askDestination(); }
    }

    /** The PKG is picked: where to install it (the update goes where the base game went, without asking). */
    private void askDestination() {
        if (pending==null) { end(); return; }
        new AlertDialog.Builder(activity).setTitle("Install "+pendingName).setCancelable(false)
            .setMessage("Choose the folder to install into. A folder named after the game's serial (for example "
                +"CUSA03173) is created inside it and becomes the game folder.\n\nYou need free space about the size of the PKG.")
            .setNegativeButton("Cancel",(d,w) -> { discard(); end(); })
            .setPositiveButton("Choose folder…",(d,w) -> pickDest()).show();
    }
    private void discard() { if (pending!=null && pendingTemp) pending.delete(); pending=null; }

    /** The PKG does not match its own checksums: say which parts, and offer to pick it again, cancel, or go on. The
     *  PKG (a cache copy too) is kept for "Install anyway". */
    private void damaged(File pkg,boolean temporary,String parts) {
        pending=pkg; pendingTemp=temporary;
        new AlertDialog.Builder(activity).setTitle("This PKG is damaged").setCancelable(false)
            .setMessage("The PKG doesn't match the checksums stored in it:\n\n"+parts
                +"\n\nThe file is damaged or incomplete (a download or copy that went wrong). Download or copy it again, "
                +"then pick it. Installing it as it is may fail or leave a game that crashes.")
            .setPositiveButton("Pick again",(d,w) -> { discard(); pickPkg(); })
            .setNegativeButton("Cancel",(d,w) -> { discard(); end(); })
            .setNeutralButton("Install anyway",(d,w) -> { skipCheck=true; run(); }).show();
    }

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
        final boolean check=!skipCheck; skipCheck=false;
        busy=true; cancel.set(false);
        // The first install on a new app also unpacks the app's runtime (host.pkgInstaller): some seconds.
        progress=new PkgProgressDialog(activity,"Install game","Preparing…",cancel);
        background(() -> {
            PkgInstaller.GameResult result=null; String failure=null; boolean cancelled=false;
            try {
                if (holdsGame()) { stopped=true; host.stopGame(); }
                PkgProgress track=new PkgProgress();
                PkgInstaller installer=host.pkgInstaller();
                result=installer.installGame(pkg,new File(dest),false,(done,total,label) -> {
                    long now=SystemClock.elapsedRealtime();
                    track.update(done,total,label,now);
                    String what=track.label, text=track.detail(now); int permille=track.permille();
                    main.post(() -> { if (live() && !cancel.get()) progress.pkg(what,"Installing the game…",permille,text); });
                },cancel::get,check);
                if (cancel.get()) throw new CancellationException();
            } catch (CancellationException e) { cancelled=true;
            } catch (Exception e) {
                failure=e.getMessage()!=null ? e.getMessage() : e.toString();
                android.util.Log.e("Bloodborne","game install: "+e);
            } finally { if (temporary && !(result!=null && result.damaged!=null)) pkg.delete(); } // a damaged one is kept for "Install anyway"
            PkgInstaller.GameResult o=result; String problem=failure; boolean stop=cancelled;
            main.post(() -> {
                progress.dismiss(); busy=false;
                if (!live()) { if (temporary && o!=null && o.damaged!=null) pkg.delete(); return; }
                if (stop) { host.toast("Install cancelled."); end(); }
                else if (problem==null && o!=null && o.damaged!=null) damaged(pkg,temporary,o.damaged);
                else if (problem!=null) alert("Not installed",problem,false);
                else if (!o.ok) alert("Not installed",o.problem==null ? "The PKG could not be installed." : o.problem,false);
                else finished(o);
            });
        },"Game-install");
    }

    private void finished(PkgInstaller.GameResult o) {
        String where=o.gameDir==null ? "" : o.gameDir;
        if (o.ready && !where.isEmpty()) {
            // Without native code that fits, the game start shows the "Native code needed" dialog (NativeNeededDialog): it explains the
            // Rebuild Tool and links to it, so the install says only where the game went.
            if (!host.nativeReady()) host.toast("Game installed in "+where+".");
            closed=true; host.useGame(new File(where));
            return;
        }
        // Not ready: the install found an older version (the base game, or an update before 1.09).
        String title=o.titleId==null ? "" : o.titleId;
        String version=o.appVer==null ? "" : o.appVer.replaceFirst("^0(?=\\d)","");
        String what="Installed: Bloodborne"+(title.isEmpty() ? "" : " "+title)
            +", version "+(version.isEmpty() ? "unknown" : version)+(where.isEmpty() ? "" : "\nin "+where)+".";
        new AlertDialog.Builder(activity).setTitle("Update 1.09 needed")
            .setMessage(what+"\n\nThe game runs only with version 1.09: install the 1.09 update PKG to play.")
            .setCancelable(false)
            .setNegativeButton("Later",(d,w) -> end())
            .setPositiveButton("Pick update PKG…",(d,w) -> pickPkg()).show();
    }
}
