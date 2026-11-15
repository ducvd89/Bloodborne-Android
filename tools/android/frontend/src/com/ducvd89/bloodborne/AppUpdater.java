package com.ducvd89.bloodborne;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.concurrent.CancellationException;

/** The menu's "Check for updates": looks for a newer release of this app on GitHub (UpdateCatalog),
 *  shows its notes, downloads the APK into the cache with a progress bar and a checked SHA-256, and
 *  installs it through a PackageInstaller session. The APKs of the releases are signed with one key, so
 *  Android replaces the app in place. Installing closes the app (and so the game): the player is told,
 *  and the game is stopped before the session is committed. All methods on the UI thread. */
final class AppUpdater {
    /** What the updater needs from the activity. */
    interface Host {
        /** Stops the game (the install is about to close the app). Blocks: called off the UI thread. */
        void stopGame();
        /** Starts the game again: the install did not happen. */
        void restartGame();
        /** The menu row's second line. */
        void detail(String text);
        /** A dialog went away. */
        void closed();
        boolean destroyed();
    }

    private static final String ACTION="com.ducvd89.bloodborne.UPDATE_RESULT";
    private static final long MARGIN=64L<<20;

    private final Activity activity;
    private final Host host;
    private final String installed;
    private final Handler main=new Handler(Looper.getMainLooper());
    private AlertDialog dialog, progress;
    private BroadcastReceiver receiver;
    private boolean checking;
    /** What to continue with once the player has allowed installs from this app (see resumed()). */
    private Runnable afterPermission;
    private volatile DriverCatalog.Download download;
    private volatile int session=-1;
    private volatile boolean closed;

    AppUpdater(Activity activity,Host host) {
        this.activity=activity; this.host=host;
        String version="0";
        try { version=activity.getPackageManager().getPackageInfo(activity.getPackageName(),0).versionName; }
        catch (Exception ignored) {}
        installed=version==null ? "0" : version;
    }

    /** The installed versionName. */
    String version() { return installed; }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private boolean live() { return !closed && !host.destroyed() && !activity.isFinishing(); }

    /** The folders downloaded APKs go to: the external cache when there is one, else the internal one. */
    private File[] folders() {
        File external=activity.getExternalCacheDir();
        return external!=null ? new File[]{new File(external,"update"),new File(activity.getCacheDir(),"update")}
            : new File[]{new File(activity.getCacheDir(),"update")};
    }
    /** App start: removes downloads an earlier run left (cancelled, killed, or never installed) and
     *  abandons install sessions it left open. Blocks: not for the UI thread. */
    void cleanup() {
        for (File dir:folders()) {
            File[] files=dir.listFiles();
            if (files!=null) for (File f:files) f.delete();
        }
        try {
            PackageInstaller installer=activity.getPackageManager().getPackageInstaller();
            for (PackageInstaller.SessionInfo s:installer.getMySessions()) installer.abandonSession(s.getSessionId());
        } catch (Exception e) { android.util.Log.w("Bloodborne","update cleanup: "+e); }
    }

    /** Ends everything: cancels a download, closes the dialogs, stops listening for the install's result. */
    void close() {
        closed=true;
        DriverCatalog.Download d=download; if (d!=null) d.cancel();
        for (AlertDialog dlg:new AlertDialog[]{progress,dialog}) {
            try { if (dlg!=null && dlg.isShowing()) dlg.dismiss(); } catch (Exception ignored) {}
        }
        if (receiver!=null) { try { activity.unregisterReceiver(receiver); } catch (Exception ignored) {} receiver=null; }
    }

    // ---- The check ----
    void check() {
        if (checking) return;
        checking=true; host.detail("Checking…");
        Thread t=new Thread(() -> {
            UpdateCatalog.Release found=null; String failure=null;
            try { found=UpdateCatalog.latest(); }
            catch (Exception e) {
                failure=DriverCatalog.message(e);
                android.util.Log.w("Bloodborne","update check: "+e);
            }
            UpdateCatalog.Release release=found; String problem=failure;
            main.post(() -> {
                checking=false;
                if (!live()) return;
                if (problem!=null) { host.detail("Could not check: "+problem); toast("Could not check for updates: "+problem); }
                else if (release==null || UpdateCatalog.compare(release.version,installed)<=0) {
                    host.detail("Up to date"); toast("Bloodborne "+installed+" is up to date");
                } else {
                    host.detail(release.version+" available");
                    offer(release);
                }
            });
        },"Update-check");
        t.setDaemon(true); t.start();
    }

    private void toast(String message) { android.widget.Toast.makeText(activity,message,android.widget.Toast.LENGTH_LONG).show(); }
    private AlertDialog.Builder builder() { return new AlertDialog.Builder(activity); }
    private void notice(String title,String message) {
        builder().setTitle(title).setMessage(message).setPositiveButton("OK",null)
            .setOnDismissListener(d -> host.closed()).show();
    }

    /** The new version, its size and notes, and the choice to go on. */
    private void offer(UpdateCatalog.Release release) {
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(8),dp(24),dp(8));
        TextView head=new TextView(activity); head.setTextSize(14); head.setTextColor(0xffc6ad76);
        head.setText("Version "+release.version+" · "+DriverCatalog.megabytes(release.size)+"\nYou have "+installed+".");
        TextView notes=new TextView(activity); notes.setTextSize(13); notes.setPadding(0,dp(10),0,dp(10));
        notes.setText(release.notes.isEmpty() ? "No release notes." : release.notes);
        TextView warning=new TextView(activity); warning.setTextSize(13); warning.setAlpha(0.7f);
        warning.setText("Installing closes the game: any unsaved progress is lost. Your settings and saves are kept.");
        box.addView(head); box.addView(notes); box.addView(warning);
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        dialog=builder().setTitle("Update available").setView(scroll)
            .setNegativeButton("Later",null)
            .setPositiveButton("Download and install",(d,w) -> begin(release)).create();
        dialog.setOnKeyListener((d,code,e) -> { // B closes it, like the drawer
            if (code!=KeyEvent.KEYCODE_BUTTON_B && e.getScanCode()!=305) return false;
            if (e.getAction()==KeyEvent.ACTION_UP) dialog.dismiss();
            return true;
        });
        dialog.setOnDismissListener(d -> host.closed());
        dialog.show();
    }

    // ---- Permission to install ----
    private boolean allowed() { return activity.getPackageManager().canRequestPackageInstalls(); }

    /** Runs next when Android lets this app install packages; asks the player to allow it first if not. */
    private void withPermission(Runnable next) {
        if (allowed()) { next.run(); return; }
        builder().setTitle("Allow installing updates")
            .setMessage("Android needs your permission for Bloodborne to install its own update. "
                +"In the next screen, allow \"Install unknown apps\" for Bloodborne, then come back.")
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Open settings",(d,w) -> {
                afterPermission=next;
                try {
                    activity.startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:"+activity.getPackageName())));
                } catch (Exception e) {
                    afterPermission=null;
                    notice("Cannot open the settings","Allow Bloodborne to install unknown apps in Android's settings, then try again.");
                }
            }).setOnDismissListener(d -> host.closed()).show();
    }
    /** The activity is back in front: goes on with the update when the player allowed installs. */
    void resumed() {
        Runnable next=afterPermission;
        if (next==null) return;
        afterPermission=null;
        if (allowed()) next.run(); else toast("Installing updates is not allowed for Bloodborne");
    }

    // ---- Download ----
    private void begin(UpdateCatalog.Release release) { withPermission(() -> download(release)); }

    /** The folder for the download, or null when the phone lacks the space: the APK twice over
     *  (the download and the installer's copy), on the cache's volume or the app's. */
    private File folder(long size) {
        long data=activity.getFilesDir().getUsableSpace();
        File[] candidates=folders();
        for (int i=0;i<candidates.length;++i) {
            File dir=candidates[i];
            boolean internal=i==candidates.length-1; // the same volume as the installer's copy
            long need=internal ? 2*size+2*MARGIN : size+MARGIN;
            if ((dir.isDirectory() || dir.mkdirs()) && dir.canWrite() && dir.getUsableSpace()>=need && (internal || data>=size+MARGIN))
                return dir;
        }
        return null;
    }

    private void download(UpdateCatalog.Release release) {
        if (!release.url.startsWith(UpdateCatalog.DOWNLOADS) || !release.asset.matches("Bloodborne-[0-9.]+\\.apk")) {
            notice("Update not downloaded","The release does not point to this app's downloads."); return;
        }
        File dir=folder(release.size);
        if (dir==null) {
            notice("Not enough free space","The update needs about "+DriverCatalog.megabytes(2*release.size+MARGIN)
                +" free on the phone: the download and the installer's copy. Free some space and try again.");
            return;
        }
        File part=new File(dir,release.asset+".part"), apk=new File(dir,release.asset);
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(12),dp(24),dp(8));
        TextView message=new TextView(activity); message.setTextSize(15); message.setText("Starting the download…");
        ProgressBar bar=new ProgressBar(activity,null,android.R.attr.progressBarStyleHorizontal); bar.setMax(1000);
        bar.setIndeterminate(true);
        box.addView(message); box.addView(bar,new LinearLayout.LayoutParams(-1,-2));
        DriverCatalog.Download dl=new DriverCatalog.Download(); download=dl;
        progress=builder().setTitle("Bloodborne "+release.version).setView(box).setCancelable(false)
            .setNegativeButton("Cancel",(d,w) -> dl.cancel()).create();
        progress.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON); // minutes on a slow link
        progress.show();
        Thread t=new Thread(() -> {
            String sha=release.sha256, failure=null; boolean cancelled=false, done=false;
            try {
                if (sha.isEmpty()) sha=UpdateCatalog.checksum(release);
                if (sha.isEmpty()) throw new java.io.IOException("This release has no checksum to verify the download with, so it is not installed.");
                DriverCatalog.Entry entry=new DriverCatalog.Entry("android-"+release.version,"Bloodborne "+release.version,
                    "app","",release.asset,release.size,release.url,sha);
                dl.run(entry,part,(have,total) -> main.post(() -> {
                    message.setText((have*100/total)+"% · "+DriverCatalog.megabytes(have)+" of "+DriverCatalog.megabytes(total));
                    bar.setIndeterminate(false); bar.setProgress((int)(have*1000/total));
                }));
                apk.delete();
                if (!part.renameTo(apk)) throw new java.io.IOException("Cannot save the download");
                done=true;
            } catch (CancellationException ex) {
                cancelled=true;
            } catch (Exception ex) {
                failure=DriverCatalog.message(ex);
                if (failure.equals("Offline")) failure="Offline — the download could not be completed.";
                android.util.Log.e("Bloodborne","update download: "+ex);
            } finally { part.delete(); if (!done) apk.delete(); }
            String problem=failure, checked=sha; boolean stopped=cancelled, ok=done;
            main.post(() -> {
                try { progress.dismiss(); } catch (Exception ignored) {}
                download=null;
                if (!live() || stopped) { if (!ok) apk.delete(); return; }
                if (!ok) { notice("Update not downloaded",problem); return; }
                confirm(release,apk,checked);
            });
        },"Update-download");
        t.setDaemon(true); t.start();
    }

    // ---- Install ----
    /** The APK is downloaded and checked: the last question before the game closes. */
    private void confirm(UpdateCatalog.Release release,File apk,String sha) {
        builder().setTitle("Install Bloodborne "+release.version+"?")
            .setMessage("The download is complete and verified. Installing closes the game: any unsaved progress is lost. "
                +"Android asks you to confirm, then Bloodborne starts again as the new version.")
            .setCancelable(false)
            .setNegativeButton("Cancel",(d,w) -> apk.delete())
            .setPositiveButton("Install",(d,w) -> withPermission(() -> install(release,apk,sha)))
            .setOnDismissListener(d -> host.closed()).show();
    }
    /** Writes the APK into an install session (hashing it again: the cache may be on shared storage)
     *  and commits it; the result comes to the receiver. */
    private void install(UpdateCatalog.Release release,File apk,String sha) {
        listen();
        AlertDialog preparing=builder().setTitle("Bloodborne "+release.version)
            .setMessage("Preparing the install…").setCancelable(false).create();
        preparing.show();
        Thread t=new Thread(() -> {
            String failure=null;
            PackageInstaller installer=activity.getPackageManager().getPackageInstaller();
            int id=-1; boolean committed=false;
            try {
                host.stopGame();
                PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                params.setAppPackageName(activity.getPackageName());
                params.setSize(apk.length());
                id=installer.createSession(params);
                try (PackageInstaller.Session s=installer.openSession(id)) {
                    MessageDigest hash=MessageDigest.getInstance("SHA-256");
                    try (InputStream in=new FileInputStream(apk); OutputStream out=s.openWrite("Bloodborne.apk",0,apk.length())) {
                        byte[] buffer=new byte[1<<16];
                        for (int n; (n=in.read(buffer))>0;) { out.write(buffer,0,n); hash.update(buffer,0,n); }
                        s.fsync(out);
                    }
                    StringBuilder hex=new StringBuilder();
                    for (byte b:hash.digest()) hex.append(String.format(Locale.ROOT,"%02x",b&0xff));
                    if (!hex.toString().equalsIgnoreCase(sha)) throw new java.io.IOException("The downloaded file changed before the install. Download it again.");
                    session=id;
                    Intent result=new Intent(ACTION).setPackage(activity.getPackageName());
                    PendingIntent callback=PendingIntent.getBroadcast(activity,id,result,PendingIntent.FLAG_UPDATE_CURRENT);
                    s.commit(callback.getIntentSender());
                    committed=true;
                }
            } catch (Exception ex) {
                failure=ex.getMessage()!=null ? ex.getMessage() : ex.toString();
                android.util.Log.e("Bloodborne","update install: "+ex);
            } finally {
                if (!committed && id>=0) { try { installer.abandonSession(id); } catch (Exception ignored) {} session=-1; }
                apk.delete(); // the session holds its own copy
            }
            String problem=failure;
            main.post(() -> {
                try { preparing.dismiss(); } catch (Exception ignored) {}
                if (problem!=null) {
                    if (live()) notice("Update not installed",problem);
                    host.restartGame();
                }
            });
        },"Update-install");
        t.setDaemon(true); t.start();
    }

    /** Receives the installer's answers (registered once; unregistered by close). */
    private void listen() {
        if (receiver!=null) return;
        receiver=new BroadcastReceiver() {
            @Override public void onReceive(Context context,Intent intent) {
                if (intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID,-2)!=session) return;
                int status=intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE);
                String text=intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
                if (status==PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirmation=intent.getParcelableExtra(Intent.EXTRA_INTENT);
                    try { activity.startActivity(confirmation); }
                    catch (Exception e) { failed("Android's installer could not be opened: "+e.getMessage()); }
                    return;
                }
                session=-1;
                if (status==PackageInstaller.STATUS_SUCCESS) return; // the new version replaces this process
                if (status==PackageInstaller.STATUS_FAILURE_ABORTED) {
                    if (live()) notice("Update cancelled","The update was not installed. Bloodborne "+installed+" stays.");
                    host.restartGame();
                    return;
                }
                failed(text==null || text.isEmpty() ? "Android refused the update (status "+status+")." : text);
            }
        };
        activity.registerReceiver(receiver,new IntentFilter(ACTION));
    }
    private void failed(String problem) {
        session=-1;
        if (live()) notice("Update not installed",problem);
        host.restartGame();
    }
}
