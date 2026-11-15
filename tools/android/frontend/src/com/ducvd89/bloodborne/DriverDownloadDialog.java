package com.ducvd89.bloodborne;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.util.List;
import java.util.concurrent.CancellationException;

/** Downloads a GPU driver from this app's GitHub releases (DriverCatalog; the Turnip for Android builds for
 *  Adreno and the PanVK builds for Mali): the 10 newest builds of all of them (never hidden by GPU), the ones for
 *  this GPU marked, and a build that needs a newer Android than this phone's marked and not installed. It opens with the last list it saved, looks for a newer one in the background
 *  and updates the list in place. A tap downloads the zip with a progress bar, installs it through
 *  DriverManager (which checks it) and offers to use it. Rows take the gamepad (D-pad, A, B) and touch. */
final class DriverDownloadDialog {
    /** What the dialog needs from the activity. */
    interface Host {
        /** The GPU family (GpuFamily): "main" (Adreno 6xx/7xx), "gen8" (Adreno 8xx), "mali" or null when unknown;
         *  builds are recommended by family (every gen8* variant for "gen8"). Blocks: called off the UI thread. */
        String gpuVariant();
        /** Selects the driver for the next start (and offers the restart). */
        void use(DriverManager.Driver driver);
        /** The dialog went away. */
        void closed();
        boolean destroyed();
    }

    private final Activity activity;
    private final DriverManager drivers;
    private final DriverCatalog catalog;
    private final File zip;
    private final Host host;
    private final Handler main=new Handler(Looper.getMainLooper());
    private AlertDialog dialog, progress;
    private LinearLayout list;
    private TextView note;
    // UI-thread state behind the list:
    private DriverCatalog.Result shown;
    private String error, gpu;
    private boolean refreshing=true;
    private volatile DriverCatalog.Download download;
    private volatile boolean closed;

    /** files: the app's files directory. */
    DriverDownloadDialog(Activity activity,DriverManager drivers,File files,Host host) {
        this.activity=activity; this.drivers=drivers; this.host=host;
        catalog=new DriverCatalog(files); zip=new File(new File(files,"drivers"),".download.zip");
    }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private boolean live() { return !closed && !host.destroyed() && !activity.isFinishing(); }

    void show() {
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(8),dp(24),dp(8));
        TextView intro=new TextView(activity); intro.setTextSize(13); intro.setAlpha(0.8f);
        intro.setText("Turnip (Adreno) and PanVK (Mali) builds for Android from this app's GitHub releases. "
            +"A driver you pick here replaces the automatic choice and the phone's own driver; "
            +"choose Automatic or the phone's own driver again to go back.");
        box.addView(intro);
        note=new TextView(activity); note.setTextSize(13); note.setTextColor(0xffc6ad76);
        note.setPadding(0,dp(8),0,dp(4));
        box.addView(note);
        list=new LinearLayout(activity); list.setOrientation(LinearLayout.VERTICAL);
        box.addView(list,new LinearLayout.LayoutParams(-1,-2));
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        dialog=new AlertDialog.Builder(activity).setTitle("Download a GPU driver").setView(scroll)
            .setNegativeButton("Close",null).create();
        dialog.setOnKeyListener((d,code,e) -> { // B closes it, like the drawer
            if (code!=KeyEvent.KEYCODE_BUTTON_B && e.getScanCode()!=305) return false;
            if (e.getAction()==KeyEvent.ACTION_UP) dialog.dismiss();
            return true;
        });
        dialog.setOnDismissListener(d -> { close(); host.closed(); });
        render();
        dialog.show();
        refresh();
        Thread detect=new Thread(() -> {
            String variant=host.gpuVariant();
            main.post(() -> { if (live()) { gpu=variant; render(); } });
        },"Driver-gpu");
        detect.setDaemon(true); detect.start();
    }

    /** Ends everything: cancels a download and closes the dialogs (also when the activity is destroyed). */
    void close() {
        closed=true;
        DriverCatalog.Download d=download; if (d!=null) d.cancel();
        for (AlertDialog dlg:new AlertDialog[]{progress,dialog}) {
            try { if (dlg!=null && dlg.isShowing()) dlg.dismiss(); } catch (Exception ignored) {}
        }
    }

    /** Shows the saved list, then asks GitHub (a 304 keeps the saved one) and shows the answer. */
    private void refresh() {
        refreshing=true; error=null; render();
        Thread t=new Thread(() -> {
            DriverCatalog.Result saved=catalog.cached();
            if (saved!=null) main.post(() -> { if (live() && shown==null) { shown=saved; render(); } });
            DriverCatalog.Result fresh=null; String failure=null;
            try { fresh=catalog.refresh(saved); }
            catch (Exception e) {
                failure=DriverCatalog.message(e);
                android.util.Log.w("Bloodborne","driver list: "+e);
            }
            DriverCatalog.Result result=fresh; String problem=failure;
            main.post(() -> {
                if (!live()) return;
                refreshing=false; error=problem;
                if (result!=null) shown=result;
                render();
            });
        },"Driver-catalog");
        t.setDaemon(true); t.start();
    }

    /** Rebuilds the rows from the current state; the focused row stays focused. */
    private void render() {
        int focus=-1;
        for (int i=0;i<list.getChildCount();++i) if (list.getChildAt(i).hasFocus()) focus=i;
        list.removeAllViews();
        if (error!=null) note.setText(shown!=null ? error+" — showing the list from "+DriverCatalog.day(shown.fetched)+"."
            : error+" — the driver list could not be loaded.");
        else note.setText(refreshing ? shown==null ? "Loading the list…" : "Checking for new drivers…"
            : shown!=null && shown.entries.isEmpty() ? "No driver builds have been published yet." : "");
        note.setVisibility(note.getText().length()==0 ? View.GONE : View.VISIBLE);
        List<DriverManager.Driver> installed=drivers.list();
        if (shown!=null) for (DriverCatalog.Entry e:shown.entries) list.addView(row(e,installed));
        if (error!=null && shown==null) {
            Button retry=new Button(activity); retry.setAllCaps(false); retry.setText("Retry");
            retry.setOnClickListener(v -> refresh());
            list.addView(retry,new LinearLayout.LayoutParams(-2,-2));
        }
        if (focus>=0 && focus<list.getChildCount()) list.getChildAt(focus).requestFocus();
        else if (list.getChildCount()>0 && dialog.getCurrentFocus()==null) list.getChildAt(0).requestFocus();
    }

    private View row(DriverCatalog.Entry e,List<DriverManager.Driver> installed) {
        StringBuilder detail=new StringBuilder(e.date());
        if (detail.length()>0) detail.append(" · ");
        detail.append(DriverCatalog.megabytes(e.size));
        boolean fits=Build.VERSION.SDK_INT>=e.minApi();
        detail.append(" · ").append(e.family().equals("mali") ? "Mali only" : e.gpu());
        if (!fits) detail.append(" · Needs ").append(androidName(e.minApi()));
        else if (gpu!=null && e.family().equals(DriverCatalog.family(gpu))) detail.append(" · Recommended for this GPU");
        if (installedFor(e,installed)!=null) detail.append(" · Installed");
        LinearLayout row=new LinearLayout(activity); row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(dp(12),dp(10),dp(12),dp(10));
        TextView title=new TextView(activity); title.setTextSize(16); title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setTextColor(0xffe8dfc9); title.setText(e.title);
        TextView sub=new TextView(activity); sub.setTextSize(13); sub.setAlpha(0.75f);
        sub.setTextColor(0xffe8dfc9); sub.setText(detail);
        row.addView(title); row.addView(sub);
        // Focus (gamepad) and press (touch) look the same.
        GradientDrawable lit=new GradientDrawable(); lit.setColor(0x33c6ad76); lit.setCornerRadius(dp(6));
        StateListDrawable background=new StateListDrawable();
        background.addState(new int[]{android.R.attr.state_focused},lit);
        background.addState(new int[]{android.R.attr.state_pressed},lit);
        row.setBackground(background);
        row.setFocusable(true); row.setClickable(true);
        row.setOnClickListener(v -> choose(e));
        row.setOnKeyListener((v,code,event) -> { // A taps the row
            if (code!=KeyEvent.KEYCODE_BUTTON_A && event.getScanCode()!=304) return false;
            if (event.getAction()==KeyEvent.ACTION_UP) v.performClick();
            return true;
        });
        return row;
    }

    /** The installed driver made from this build's zip, or null. */
    private static DriverManager.Driver installedFor(DriverCatalog.Entry e,List<DriverManager.Driver> installed) {
        for (DriverManager.Driver d:installed) if (d.zip.equals(e.asset)) return d;
        return null;
    }

    /** "Android 15 or newer" for an API level. */
    private static String androidName(int api) {
        String[] names={"Android 11","Android 12","Android 12L","Android 13","Android 14","Android 15","Android 16"};
        return (api>=30 && api-30<names.length ? names[api-30] : "Android API "+api)+" or newer";
    }

    private void choose(DriverCatalog.Entry e) {
        if (Build.VERSION.SDK_INT<e.minApi()) {
            new AlertDialog.Builder(activity).setTitle("Not for this phone")
                .setMessage(e.title+" needs "+androidName(e.minApi())+". This phone runs Android API "+Build.VERSION.SDK_INT+".")
                .setPositiveButton("OK",null).show();
            return;
        }
        DriverManager.Driver have=installedFor(e,drivers.list());
        if (have==null) { start(e); return; }
        new AlertDialog.Builder(activity).setTitle("Already installed")
            .setMessage(have.title()+" is already installed from this build. Install it again as a second copy?")
            .setPositiveButton("Use it",(d,w) -> { dialog.dismiss(); host.use(have); })
            .setNeutralButton("Install again",(d,w) -> start(e))
            .setNegativeButton("Cancel",null).show();
    }

    /** Downloads the entry's zip and installs it. */
    private void start(DriverCatalog.Entry e) {
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(12),dp(24),dp(8));
        TextView message=new TextView(activity); message.setTextSize(15); message.setText("Starting the download…");
        ProgressBar bar=new ProgressBar(activity,null,android.R.attr.progressBarStyleHorizontal); bar.setMax(1000);
        box.addView(message); box.addView(bar,new LinearLayout.LayoutParams(-1,-2));
        DriverCatalog.Download dl=new DriverCatalog.Download(); download=dl;
        progress=new AlertDialog.Builder(activity).setTitle(e.title).setView(box).setCancelable(false)
            .setNegativeButton("Cancel",(d,w) -> dl.cancel()).create();
        progress.show();
        Thread t=new Thread(() -> {
            DriverManager.Driver installed=null; String failure=null; boolean cancelled=false;
            try {
                File dir=zip.getParentFile();
                if (!dir.isDirectory() && !dir.mkdirs()) throw new java.io.IOException("Cannot create "+dir);
                dl.run(e,zip,(done,total) -> main.post(() -> {
                    message.setText((done*100/total)+"% · "+DriverCatalog.megabytes(done)+" of "+DriverCatalog.megabytes(total));
                    bar.setProgress((int)(done*1000/total));
                }));
                main.post(() -> { message.setText("Installing…"); bar.setIndeterminate(true); });
                try (FileInputStream in=new FileInputStream(zip)) { installed=drivers.install(in,e.asset); }
            } catch (CancellationException ex) {
                cancelled=true;
            } catch (Exception ex) {
                failure=DriverCatalog.message(ex);
                if (failure.equals("Offline")) failure="Offline — the download could not be completed.";
                android.util.Log.e("Bloodborne","driver download: "+ex);
            } finally { zip.delete(); }
            DriverManager.Driver result=installed; String problem=failure; boolean stopped=cancelled;
            main.post(() -> {
                try { progress.dismiss(); } catch (Exception ignored) {}
                download=null;
                if (!live() || stopped) return;
                render(); // "Installed" on its row
                if (result==null) new AlertDialog.Builder(activity).setTitle("Driver not installed").setMessage(problem)
                    .setPositiveButton("OK",null).show();
                else new AlertDialog.Builder(activity).setTitle("Driver installed")
                    .setMessage(result.title()+" is installed. Use it now?")
                    .setNegativeButton("Later",null).setPositiveButton("Use",(d,w) -> { dialog.dismiss(); host.use(result); }).show();
            });
        },"Driver-download");
        t.setDaemon(true); t.start();
    }
}
