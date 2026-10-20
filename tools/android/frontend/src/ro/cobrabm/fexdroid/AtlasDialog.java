package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.hardware.display.DisplayManager;
import android.view.Display;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;
import java.util.concurrent.CancellationException;

/** The Hunter's Atlas menu: the map (a switch), where it is shown (second screen, floating mini map or both), the mini
 *  map's opacity and position, what it shows, and its map data. Turning
 *  the map on downloads the map pack the first time (AtlasPack: progress, cancel; a failure or no network turns
 *  the switch back off). Map by Hunter's Atlas, https://github.com/MikeTat1988/HuntersAtlas (GPL-2.0-or-later). */
final class AtlasDialog {
    /** What the dialog needs from the activity. */
    interface Host {
        boolean destroyed();
        void closed();
        /** The game runs, but was started without the map's files: it needs a restart for the map. */
        boolean needsRestart();
        void restart();
        /** The game process is running (a new pack's flags reach it only after a restart). */
        default boolean gameRunning() { return false; }
        /** Closes this dialog and opens the layout editor on the mini map's place. */
        void editMiniMap();
        /** The floating map's side in percent of the game screen's shorter side (the layout editor's own size), and its setter. */
        int miniPercent();
        void setMiniPercent(int percent);
    }

    private final Activity activity;
    private final AtlasScreen screen;
    private final Host host;
    private final Handler main=new Handler(Looper.getMainLooper());
    private AlertDialog dialog, progress;
    private Switch enable;
    private TextView status;
    private Button delete, editMini;
    private LinearLayout floatBox, whereBox, updateBox;
    private DisplayManager displayManager;
    private boolean watching;
    private TextView editNote;
    private boolean setting; // the switch is being set by the code
    private AtlasScreen.UpdateListener following;

    AtlasDialog(Activity activity,AtlasScreen screen,Host host) { this.activity=activity; this.screen=screen; this.host=host; }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private boolean live() { return !host.destroyed() && !activity.isFinishing(); }
    private TextView note(LinearLayout box,String text,float alpha) {
        TextView t=new TextView(activity); t.setTextSize(13); t.setAlpha(alpha); t.setText(text);
        t.setPadding(0,dp(4),0,dp(4)); box.addView(t); return t;
    }

    /** The drawer row's second line. */
    static String summary(AtlasScreen screen) {
        if (!screen.enabled()) return "Off";
        if (!screen.installed()) return "On · map data not downloaded";
        String mode=screen.mode();
        if (mode.equals(AtlasScreen.MODE_FLOAT)) return "On · mini map";
        String second=screen.secondDisplay()==null ? "no second display" : "second screen";
        return mode.equals(AtlasScreen.MODE_BOTH) ? "On · "+second+" + mini map" : "On · "+second;
    }

    void show() {
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));
        note(box,"Hunter's Atlas shows a live map: on the second screen (the bottom one of a dual-screen handheld), as a small "
            +"floating map over the game, or both. It shows where you are and which way you face, the floor you are on, lamps you have lit, "
            +"and, if you choose, remaining pickups and conversations. Touch the second-screen map to pan, zoom and browse the areas; "
            +"tap a lamp, pickup, conversation or any spot and choose Navigate for a route. "
            +"It needs the map data (a download) and a restart of the game the first time.",0.8f);
        enable=new Switch(activity); enable.setText("Map"); enable.setTextSize(16);
        enable.setPadding(0,dp(8),0,dp(8));
        setting=true; enable.setChecked(screen.enabled() && screen.installed()); setting=false;
        enable.setOnCheckedChangeListener((b,on) -> { if (!setting) toggled(on); });
        box.addView(enable);
        status=note(box,"",0.9f); status.setTextColor(0xffc6ad76);
        updateBox=new LinearLayout(activity); updateBox.setOrientation(LinearLayout.VERTICAL);
        box.addView(updateBox);

        whereBox=new LinearLayout(activity); whereBox.setOrientation(LinearLayout.VERTICAL);
        box.addView(whereBox);
        refreshWhere();
        // The floating map's options: opacity and position.
        floatBox=new LinearLayout(activity); floatBox.setOrientation(LinearLayout.VERTICAL);
        final TextView opacity=new TextView(activity); opacity.setTextSize(15); opacity.setPadding(0,dp(6),0,0);
        opacity.setText("Mini map opacity: "+screen.floatAlpha()+" %");
        floatBox.addView(opacity);
        SeekBar alpha=new SeekBar(activity);
        alpha.setMax((AtlasScreen.ALPHA_MAX-AtlasScreen.ALPHA_MIN)/AtlasScreen.ALPHA_STEP);
        alpha.setProgress((screen.floatAlpha()-AtlasScreen.ALPHA_MIN)/AtlasScreen.ALPHA_STEP);
        alpha.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar,int progress,boolean user) {
                int percent=AtlasScreen.ALPHA_MIN+progress*AtlasScreen.ALPHA_STEP;
                opacity.setText("Mini map opacity: "+percent+" %");
                if (user) screen.setFloatAlpha(percent);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        floatBox.addView(alpha,new LinearLayout.LayoutParams(-1,-2));
        final TextView sizeLabel=new TextView(activity); sizeLabel.setTextSize(15); sizeLabel.setPadding(0,dp(8),0,0);
        final int minPercent=18, maxPercent=70;
        int now=Math.max(minPercent,Math.min(maxPercent,host.miniPercent()));
        sizeLabel.setText("Mini map size: "+now+" %");
        floatBox.addView(sizeLabel);
        SeekBar sizeBar=new SeekBar(activity); sizeBar.setMax(maxPercent-minPercent); sizeBar.setProgress(now-minPercent);
        sizeBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar,int progress,boolean user) {
                int percent=minPercent+progress;
                sizeLabel.setText("Mini map size: "+percent+" %");
                if (user) host.setMiniPercent(percent);
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        floatBox.addView(sizeBar,new LinearLayout.LayoutParams(-1,-2));
        editMini=new Button(activity); editMini.setAllCaps(false); editMini.setText("Edit mini map position");
        editMini.setOnClickListener(v -> host.editMiniMap());
        floatBox.addView(editMini,new LinearLayout.LayoutParams(-2,-2));
        editNote=note(floatBox,"Drag the map to move it, pinch (or − / +) to resize it. It is top right by default; the on-screen controller and the camera work through it. "
            +"Touch and hold it for 2 seconds to open the full map (set a destination there; Close returns here). It appears once the game is in a mapped place and hides while the game is paused.",0.65f);
        box.addView(floatBox);

        TextView heading=new TextView(activity); heading.setText("Show on the map"); heading.setTextSize(15);
        heading.setTypeface(Typeface.DEFAULT,Typeface.BOLD); heading.setPadding(0,dp(10),0,dp(2)); box.addView(heading);
        Switch pickups=new Switch(activity); pickups.setText("Remaining pickups"); pickups.setTextSize(15);
        pickups.setPadding(0,dp(6),0,dp(6)); pickups.setChecked(screen.pickups());
        pickups.setOnCheckedChangeListener((b,on) -> screen.setOption(AtlasScreen.PREF_PICKUPS,on));
        box.addView(pickups);
        note(box,"Blood vials red, Insight purple, equipment blue, Coldblood gold. A pickup disappears once you have it.",0.65f);
        TextView npc=new TextView(activity); npc.setText("Conversations"); npc.setTextSize(15); npc.setPadding(0,dp(6),0,dp(2)); box.addView(npc);
        final String[] modes={"all","new","hidden"}; String[] names={"All conversation sites","New dialogue only","Hidden"};
        RadioGroup group=new RadioGroup(activity);
        for (int i=0;i<modes.length;++i) {
            RadioButton r=new RadioButton(activity); r.setId(View.generateViewId()); r.setText(names[i]); r.setTextSize(15); group.addView(r);
            if (modes[i].equals(screen.npcMode())) group.check(r.getId());
        }
        group.setOnCheckedChangeListener((g,id) -> { for (int i=0;i<g.getChildCount();++i) if (g.getChildAt(i).getId()==id) screen.setNpcMode(modes[i]); });
        box.addView(group);
        Switch fog=new Switch(activity); fog.setText("Boss fog"); fog.setTextSize(15); fog.setPadding(0,dp(6),0,dp(6));
        fog.setChecked(screen.fog());
        fog.setOnCheckedChangeListener((b,on) -> screen.setOption(AtlasScreen.PREF_FOG,on));
        box.addView(fog);
        note(box,"Hides an area's map until its boss is beaten (as in Hunter's Atlas), to avoid spoilers. Off by default.",0.65f);
        TextView printHead=new TextView(activity); printHead.setText("Footprint size"); printHead.setTextSize(15); printHead.setPadding(0,dp(8),0,dp(2)); box.addView(printHead);
        String[] printNames={"Small","Medium","Large"};
        RadioGroup printGroup=new RadioGroup(activity); printGroup.setOrientation(LinearLayout.HORIZONTAL);
        for (int i=0;i<printNames.length;++i) {
            RadioButton r=new RadioButton(activity); r.setId(View.generateViewId()); r.setText(printNames[i]); r.setTextSize(15); r.setPadding(0,0,dp(14),0); printGroup.addView(r);
            if (i==screen.printSize()) printGroup.check(r.getId());
        }
        printGroup.setOnCheckedChangeListener((g,id) -> { for (int i=0;i<g.getChildCount();++i) if (g.getChildAt(i).getId()==id) screen.setPrintSize(i); });
        box.addView(printGroup);
        note(box,"The size of the route's footprints on the maps.",0.65f);

        TextView debug=new TextView(activity); debug.setText("Debug"); debug.setTextSize(15);
        debug.setTypeface(Typeface.DEFAULT,Typeface.BOLD); debug.setPadding(0,dp(12),0,dp(2)); box.addView(debug);
        Switch doors=new Switch(activity); doors.setText("Log door flags"); doors.setTextSize(15); doors.setPadding(0,dp(6),0,dp(6));
        doors.setChecked(screen.doorLog());
        doors.setOnCheckedChangeListener((b,on) -> screen.setDoorLog(on));
        box.addView(doors);
        note(box,"While on, every change of a door, lever or lift state flag is written to atlas-doors.log (time, area, door, flag, old and new "
            +"value, your position) and included in Report a problem. Open a door in the game to see which flag flips. Needs the map data with route finding.",0.65f);

        delete=new Button(activity); delete.setAllCaps(false);
        delete.setOnClickListener(v -> askDelete());
        box.addView(delete,new LinearLayout.LayoutParams(-2,-2));
        note(box,"Map and art are generated from Bloodborne's own data. Map renderer ported from Hunter's Atlas by MikeTat1988 "
            +"(github.com/MikeTat1988/HuntersAtlas), GPL-2.0-or-later.",0.6f);

        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        dialog=new AlertDialog.Builder(activity).setTitle("Hunter's Atlas").setView(scroll).setPositiveButton("Close",null).create();
        dialog.setOnDismissListener(d -> { unwatchDisplays(); host.closed(); });
        refreshUi();
        watchDisplays();
        dialog.show();
        if (screen.updating()) attachProgress(screen.enabled() || screen.installed());
    }

    /** While the dialog is open a display that comes or goes updates the choices at once (and AtlasScreen, which has its own
     *  listener, shows or hides its views). */
    private final DisplayManager.DisplayListener displayListener=new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) { main.post(AtlasDialog.this::displaysChanged); }
        @Override public void onDisplayRemoved(int id) { main.post(AtlasDialog.this::displaysChanged); }
        @Override public void onDisplayChanged(int id) { main.post(AtlasDialog.this::displaysChanged); }
    };
    private void displaysChanged() {
        if (!live() || dialog==null || !dialog.isShowing()) return;
        screen.displaysChanged();
        refreshWhere(); refreshUi();
    }
    private void watchDisplays() {
        displayManager=(DisplayManager)activity.getSystemService(android.content.Context.DISPLAY_SERVICE);
        if (displayManager!=null && !watching) { displayManager.registerDisplayListener(displayListener,main); watching=true; }
    }
    private void unwatchDisplays() {
        if (displayManager!=null && watching) { displayManager.unregisterDisplayListener(displayListener); watching=false; }
    }

    /** "Where" (second screen / floating mini map / both) and, with several other displays, the "Display" choice. With no
     *  second display the first and last options are grayed out with a note and floating is what shows (the saved choice
     *  is not overwritten: it applies again when a display appears). */
    private void refreshWhere() {
        if (whereBox==null) return;
        whereBox.removeAllViews();
        final java.util.List<Display> others=screen.secondDisplays();
        final boolean have=!others.isEmpty();
        TextView where=new TextView(activity); where.setText("Where"); where.setTextSize(15);
        where.setTypeface(Typeface.DEFAULT,Typeface.BOLD); where.setPadding(0,dp(10),0,dp(2)); whereBox.addView(where);
        final String[] places={AtlasScreen.MODE_SCREEN,AtlasScreen.MODE_FLOAT,AtlasScreen.MODE_BOTH};
        String[] placeNames={"Second screen","Floating mini map","Both"};
        RadioGroup group=new RadioGroup(activity);
        String effective=screen.mode();
        setting=true;
        for (int i=0;i<places.length;++i) {
            RadioButton r=new RadioButton(activity); r.setId(View.generateViewId()); r.setText(placeNames[i]); r.setTextSize(15);
            group.addView(r);
            if (!places[i].equals(AtlasScreen.MODE_FLOAT)) r.setEnabled(have);
            if (places[i].equals(effective)) group.check(r.getId());
        }
        setting=false;
        group.setOnCheckedChangeListener((g,id) -> {
            if (setting) return;
            for (int i=0;i<g.getChildCount();++i)
                if (g.getChildAt(i).getId()==id && g.getChildAt(i).isEnabled() && !places[i].equals(screen.mode())) screen.setMode(places[i]);
            refreshUi();
        });
        whereBox.addView(group);
        if (!have) {
            TextView none=note(whereBox,"No second display connected",0.8f); none.setTextColor(0xffc6ad76);
        } else if (others.size()>1) {
            TextView head=new TextView(activity); head.setText("Display"); head.setTextSize(15);
            head.setTypeface(Typeface.DEFAULT,Typeface.BOLD); head.setPadding(0,dp(10),0,dp(2)); whereBox.addView(head);
            RadioGroup list=new RadioGroup(activity);
            Display chosen=screen.secondDisplay();
            setting=true;
            for (Display d:others) {
                RadioButton r=new RadioButton(activity); r.setId(View.generateViewId()); r.setTextSize(15);
                android.graphics.Point size=new android.graphics.Point(); try { d.getRealSize(size); } catch (RuntimeException ignored) {}
                r.setText(d.getName()+" ("+size.x+"×"+size.y+", id "+d.getDisplayId()+")");
                r.setTag(d); list.addView(r);
                if (chosen!=null && chosen.getDisplayId()==d.getDisplayId()) list.check(r.getId());
            }
            setting=false;
            list.setOnCheckedChangeListener((g,id) -> {
                if (setting) return;
                View v=g.findViewById(id);
                if (v!=null && v.getTag() instanceof Display) screen.setDisplay((Display)v.getTag());
                refreshUi();
            });
            whereBox.addView(list);
            note(whereBox,"The map is shown on this display at its own resolution.",0.65f);
        }
    }

    private void refreshUi() {
        if (status==null) return;
        status.setText(screen.describe()+(screen.enabled() && !screen.installed() ? "\nMap data is not downloaded yet." : ""));
        boolean installed=screen.installed();
        if (floatBox!=null) {
            String mode=screen.mode();
            boolean floating=mode.equals(AtlasScreen.MODE_FLOAT) || mode.equals(AtlasScreen.MODE_BOTH);
            floatBox.setVisibility(floating ? View.VISIBLE : View.GONE);
            editMini.setEnabled(screen.floatOn());
            editNote.setText(screen.floatOn() ? "Drag the map to move it, pinch (or − / +) to resize it. It is top right by default; the on-screen controller and the camera work through it. "
                +"Touch and hold it for 2 seconds to open the full map (set a destination there; Close returns here). It appears once the game is in a mapped place and hides while the game is paused."
                : "Turn the map on to place the mini map.");
        }
        refreshUpdate();
        delete.setVisibility(installed ? View.VISIBLE : View.GONE);
        if (installed) delete.setText("Delete map data ("+AtlasDialog.megabytes(AtlasPack.size(screen.base()))+")");
    }
    /** An installed pack without route data (version 1): a note and an Update button; the map works as it is without them. */
    private void refreshUpdate() {
        if (updateBox==null) return;
        updateBox.removeAllViews();
        if (!AtlasPack.updateAvailable(screen.base())) return;
        TextView t=note(updateBox,"Map update available (route finding)",1f); t.setTextColor(0xffc6ad76); t.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        note(updateBox,"Adds routes to lamps, pickups and any spot you tap, and shows doors and whether you can open them from where you are. "
            +"Until you update, the map works as before.",0.7f);
        Button update=new Button(activity); update.setAllCaps(false); update.setText("Update");
        update.setOnClickListener(v -> download(true));
        updateBox.addView(update,new LinearLayout.LayoutParams(-2,-2));
    }
    private static String megabytes(long bytes) { return String.format(java.util.Locale.ROOT,"%.1f MB",bytes/1048576.0); }

    private void switchTo(boolean on) { setting=true; enable.setChecked(on); setting=false; }

    private void toggled(boolean on) {
        if (!on) { screen.setEnabled(false); screen.refresh(); refreshUi(); return; }
        if (screen.installed()) {
            screen.setEnabled(true); screen.refresh(); refreshUi();
            if (screen.updating()) attachProgress(true); // a download of the update runs already
            else if (AtlasPack.needsUpdate(screen.base())) screen.autoUpdate(); // an older pack: updated in the background (Wi-Fi), the same way as at the game's start
            else offerRestart();
            return;
        }
        download(false);
    }

    private void offerRestart() {
        if (!host.needsRestart() && !(host.gameRunning() && screen.flagsStale())) return;
        new AlertDialog.Builder(activity).setTitle("Restart Bloodborne?")
            .setMessage("The map reads the game's state: the game starts sharing it after a restart. Any unsaved progress will be lost.")
            .setNegativeButton("Later",null).setPositiveButton("Restart",(d,w) -> host.restart()).show();
    }

    /** update: an installed pack is being replaced; the map stays on and the old pack stays if this fails or is cancelled. The
     *  download is AtlasScreen's (one at a time): this starts it, or follows the one that runs. */
    private void download(final boolean update) {
        if (!screen.updating() && !screen.startUpdate()) return;
        attachProgress(update);
    }

    /** The progress dialog of the download AtlasScreen runs (started here or at the game's start). */
    private void attachProgress(final boolean update) {
        if (following!=null || !screen.updating()) return;
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(12),dp(24),dp(8));
        final TextView message=new TextView(activity); message.setTextSize(15); message.setText("Starting the download…");
        final ProgressBar bar=new ProgressBar(activity,null,android.R.attr.progressBarStyleHorizontal); bar.setMax(1000);
        box.addView(message); box.addView(bar,new LinearLayout.LayoutParams(-1,-2));
        progress=new AlertDialog.Builder(activity).setTitle("Map data").setView(box).setCancelable(false)
            .setNegativeButton("Cancel",(d,w) -> screen.cancelUpdate()).create();
        final AtlasScreen.UpdateListener listener=new AtlasScreen.UpdateListener() {
            @Override public void progress(long done,long total,boolean unpacking) {
                if (unpacking) { message.setText("Unpacking…"); bar.setIndeterminate(true); }
                else if (total>0) { message.setText((done*100/total)+"% · "+megabytes(done)+" of "+megabytes(total)); bar.setIndeterminate(false); bar.setProgress((int)(done*1000/total)); }
                else { message.setText(megabytes(done)); bar.setIndeterminate(true); }
            }
            @Override public void finished(String problem,boolean cancelled) {
                screen.removeUpdateListener(this); following=null;
                try { progress.dismiss(); } catch (Exception ignored) {}
                if (host.destroyed()) return;
                if (problem!=null || cancelled) {
                    if (!update) { switchTo(false); screen.setEnabled(false); screen.refresh(); }
                    refreshUi();
                    if (problem!=null) new AlertDialog.Builder(activity).setTitle(update ? "Map update failed" : "Map data not installed").setMessage(problem)
                        .setPositiveButton("OK",null).show();
                    return;
                }
                if (!update) screen.setEnabled(true);
                screen.refresh(); refreshUi(); // AtlasScreen swapped the new data in already (and renewed the running game's flags)
                offerRestart();
            }
        };
        following=listener; screen.addUpdateListener(listener);
        listener.progress(screen.updateDone(),screen.updateTotal(),screen.updateUnpacking());
        progress.show();
    }

    private void askDelete() {
        new AlertDialog.Builder(activity).setTitle("Delete the map data?")
            .setMessage("The map is turned off. It is downloaded again when you turn it on.")
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Delete",(d,w) -> {
                screen.cancelUpdate();
                screen.setEnabled(false); switchTo(false);
                screen.packChanged(); AtlasPack.delete(screen.base());
                screen.refresh(); refreshUi();
            }).show();
    }

    /** Closes the dialogs (also when the activity is destroyed); a download goes on (AtlasScreen's, it ends with the activity). */
    void close() {
        if (following!=null) { screen.removeUpdateListener(following); following=null; }
        unwatchDisplays();
        for (AlertDialog d:new AlertDialog[]{progress,dialog}) { try { if (d!=null && d.isShowing()) d.dismiss(); } catch (Exception ignored) {} }
    }
}
