// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.util.Locale;

/** "Native code needed": the dialog that gets the game's native code onto the phone, in four steps. 1: why it is needed and the choices (build
 *  on the phone, build on a PC, quit); 2: the two confirmations before a build (heat, charger); 3: the build's progress (AotBuildService runs it,
 *  the dialog only shows its state and can be closed while it goes on); 4: done. A failed build shows its message with Retry and the problem
 *  report. It is due when the game starts without native code that fits (NativeCpu.needed) and from Menu > Native CPU > Rebuild on this
 *  phone. Every control takes the gamepad (D-pad, A, B) and touch. */
final class RebuildDialog {
    private static final int TEXT = 0xffe8dfc9, GOLD = 0xffc6ad76, WARN = 0xffe5a04b, GOOD = 0xff8fbf7f, DIM = 0xff8e9097;

    /** What the dialog needs from the activity. */
    interface Host {
        File base();                          // files/bbport
        File gameDir();                       // the game folder the app uses, or null
        void openToolPage(String url);        // the phone's default browser (a plain link when that fails)
        void toast(String text);
        boolean gameRunning();
        void stopGame(Runnable after);        // stops the running game (off the UI thread), then runs `after` on the UI thread
        void startGame();                     // the native code is in: start (or restart) the game
        void recheck();                       // the same check as at the game's start (the folder's aot/ is imported when it fits)
        void quit();                          // closes the app
        void report();                        // the problem report dialog
        void closed();                        // the dialog went away (the activity returns to immersive mode)
    }

    private final Activity activity;
    private final Host host;
    private final Handler main = new Handler(Looper.getMainLooper());
    private AlertDialog dialog;
    private LinearLayout box;
    private enum Step { CHECKING, CHOOSE, CONFIRM, BUILDING, DONE, FAILED }
    private Step step = Step.CHECKING;
    private NativeCpu.Needed needed;
    private AotBuild.Plan plan;
    private boolean full, update, quickKept; // full: delete the kept files at the start; update: opened from the update prompt; quickKept: a finished build's files are there
    private int resumePercent = -1;            // an unfinished work folder of this tool and game: how far it is (-1: none)
    private boolean pcOpened, understood, noCharger, checkedAgain;
    private String recheckNote;
    private volatile boolean closed = true;
    private int generation;
    // step 2 (live charger line) and step 3 (updated in place)
    private TextView chargerLine; private CheckBox noChargerBox; private Button startButton; private CheckBox heatBox;
    private android.content.BroadcastReceiver batteryReceiver;
    private TextView reusedText;
    private TextView stageText, percentText, etaText, elapsedText, tempText, chargerText, memText, noteText;
    private ProgressBar bar;
    private final Runnable statusListener = this::onStatus;

    RebuildDialog(Activity activity, Host host) { this.activity = activity; this.host = host; }

    boolean showing() { return dialog != null && dialog.isShowing(); }

    private int dp(float n) { return Math.round(n * activity.getResources().getDisplayMetrics().density); }
    private TextView line(String text, int size, int color, boolean bold) {
        TextView t = new TextView(activity); t.setText(text); t.setTextSize(size); t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        t.setPadding(0, dp(2), 0, dp(6)); return t;
    }

    // ---------------------------------------------------------------- showing

    /** The game cannot start without native code that fits: step 1 with the reason (while a build runs: its progress). Shown again in place
     *  when it is up already (the reason is refreshed: "Check again"). */
    void show(NativeCpu.Needed why) {
        needed = why; full = false; update = false;
        if (showing()) {
            if (step == Step.CHOOSE) { checkedAgain = pcOpened; recheckNote = pcOpened ? "Checked again: " + why.reason : null; render(); }
            return;
        }
        open();
        AotBuildService.Status st = AotBuildService.status();
        if (st.active() || (st.ended() && !st.acknowledged)) { onStatus(); return; }
        loadPlan(false);
    }

    /** Menu > Native CPU > Rebuild on this phone: from step 2 when the phone can build. */
    void showRebuild() { showRebuild(false, false); }

    /** `full`: the kept build files are deleted when the build starts (a full rebuild); `update`: from the "update available" prompt. */
    void showRebuild(boolean full, boolean update) {
        needed = null;
        if (showing()) return;
        this.full = full; this.update = update;
        open();
        AotBuildService.Status st = AotBuildService.status();
        if (st.active() || (st.ended() && !st.acknowledged)) { onStatus(); return; }
        loadPlan(true);
    }

    /** The notification was tapped, or the app came back while a build ran or ended: the build's own step. */
    void showProgress() {
        AotBuildService.Status st = AotBuildService.status();
        if (showing() || !(st.active() || (st.ended() && !st.acknowledged))) return;
        open();
        onStatus();
    }

    private void open() {
        closed = false; step = Step.CHECKING; pcOpened = understood = noCharger = checkedAgain = false; recheckNote = null; plan = null; resumePercent = -1; quickKept = false;
        box = new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(4), dp(24), dp(8));
        ScrollView scroll = new ScrollView(activity); scroll.addView(box);
        dialog = new AlertDialog.Builder(activity).setTitle(update ? "Native code update" : NativeCpu.NEEDED_TITLE).setView(scroll).create();
        dialog.setCanceledOnTouchOutside(false);
        dialog.setOnKeyListener((d, code, e) -> { // B goes back or closes, A taps the focused control, like the other dialogs
            if (code == KeyEvent.KEYCODE_BUTTON_B || e.getScanCode() == 305) {
                if (e.getAction() == KeyEvent.ACTION_UP) back();
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_A || e.getScanCode() == 304) {
                View f = dialog.getCurrentFocus();
                if (e.getAction() == KeyEvent.ACTION_UP && f != null) f.performClick();
                return true;
            }
            return false;
        });
        dialog.setOnDismissListener(d -> { closed = true; stopBattery(); AotBuildService.unlisten(statusListener); host.closed(); });
        AotBuildService.listen(statusListener);
        render();
        dialog.show();
    }

    /** The native code fits now ("Check again" found it): the dialog has nothing more to ask. */
    void dismissWhenReady() {
        if (showing() && (step == Step.CHOOSE || step == Step.CHECKING)) dialog.dismiss();
    }

    /** Ends everything (the activity is destroyed). */
    void close() {
        closed = true;
        stopBattery();
        AotBuildService.unlisten(statusListener);
        try { if (dialog != null && dialog.isShowing()) dialog.dismiss(); } catch (Exception ignored) {}
    }

    private void back() {
        switch (step) {
            case CONFIRM: step = Step.CHOOSE; stopBattery(); if (needed == null) { dialog.dismiss(); } else render(); break;
            case BUILDING: dialog.dismiss(); break; // the build goes on
            case CHECKING: dialog.dismiss(); break;
            default: if (step == Step.DONE || step == Step.FAILED) AotBuildService.acknowledge(); dialog.dismiss();
        }
    }

    /** What the phone can do and how far an unfinished build is: off the UI thread (it reads /proc, /sys and the game image's hash). */
    private void loadPlan(boolean toConfirm) {
        final int gen = ++generation;
        step = Step.CHECKING; render();
        Thread t = new Thread(() -> {
            File base = host.base();
            boolean arm64 = false;
            for (String abi : Build.SUPPORTED_64_BIT_ABIS) if (abi.equals("arm64-v8a")) arm64 = true;
            arm64 &= RecompAssets.program(activity) != null;
            String toolVersion = RecompAssets.version(RecompAssets.versionText(activity));
            String hash = NativeCpu.deviceImageHash(base, true);
            int shards = 0; boolean finished = false;
            if (hash != null && !full) {
                File work = new File(AotBuild.workRoot(base), AotBuild.workName(hash));
                shards = AotBuild.shardsDone(work); finished = shards > 0 && AotBuild.built(work);
            }
            final boolean quick = finished;
            AotBuild.Plan p = AotBuild.plan(arm64, AotBuild.memAvailableKb(new File("/proc/meminfo")), activity.getFilesDir().getUsableSpace(),
                AotBuild.maxFreqs(new File("/sys/devices/system/cpu")), shards > 0);
            if (toolVersion == null) p.blocker = "This APK has no build program.";
            final int pct = shards > 0 && !quick ? Math.max(1, AotBuild.percentDone(shards)) : -1; // -1 also for a finished build's files: a quick update, not a continued build
            main.post(() -> {
                if (closed || gen != generation) return;
                plan = p; resumePercent = pct; quickKept = quick;
                step = toConfirm && p.blocker == null ? Step.CONFIRM : Step.CHOOSE;
                if (toConfirm && p.blocker == null) startBattery();
                render();
            });
        }, "RebuildDialog-plan");
        t.setDaemon(true); t.start();
    }

    // ---------------------------------------------------------------- rendering

    private void render() {
        if (box == null) return;
        box.removeAllViews();
        chargerLine = null; noChargerBox = null; startButton = null; heatBox = null; reusedText = stageText = percentText = etaText = elapsedText = tempText = chargerText = memText = noteText = null; bar = null;
        View focus = null;
        switch (step) {
            case CHECKING: box.addView(line("Checking this phone…", 15, DIM, false)); break;
            case CHOOSE: focus = renderChoose(); break;
            case CONFIRM: focus = renderConfirm(); break;
            case BUILDING: focus = renderBuilding(AotBuildService.status()); break;
            case DONE: focus = renderDone(AotBuildService.status()); break;
            case FAILED: focus = renderFailed(AotBuildService.status()); break;
        }
        if (focus != null) focus.requestFocus();
    }

    /** A choice: a title, a line under it, focusable (the focus frame shows for the gamepad) and tappable. */
    private View row(String title, String subtitle, int subtitleColor, boolean enabled, Runnable action) {
        LinearLayout r = new LinearLayout(activity); r.setOrientation(LinearLayout.VERTICAL);
        r.setPadding(dp(14), dp(10), dp(14), dp(10));
        StateListDrawable bg = new StateListDrawable();
        GradientDrawable focused = new GradientDrawable(); focused.setCornerRadius(dp(8)); focused.setColor(0x33c6ad76); focused.setStroke(dp(2), GOLD);
        GradientDrawable plain = new GradientDrawable(); plain.setCornerRadius(dp(8)); plain.setColor(0x16ffffff);
        bg.addState(new int[]{android.R.attr.state_focused}, focused);
        bg.addState(new int[]{android.R.attr.state_pressed}, focused);
        bg.addState(new int[]{}, plain);
        r.setBackground(bg);
        TextView t = new TextView(activity); t.setText(title); t.setTextSize(16); t.setTypeface(Typeface.DEFAULT, Typeface.BOLD); t.setTextColor(enabled ? TEXT : DIM);
        r.addView(t);
        if (subtitle != null && !subtitle.isEmpty()) {
            TextView s = new TextView(activity); s.setText(subtitle); s.setTextSize(13); s.setTextColor(subtitleColor); s.setPadding(0, dp(2), 0, 0);
            r.addView(s);
        }
        r.setEnabled(enabled); r.setFocusable(enabled); r.setClickable(enabled);
        if (enabled) r.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = dp(8);
        box.addView(r, lp);
        return r;
    }
    private Button button(String label, boolean enabled, Runnable action) {
        Button b = new Button(activity); b.setText(label); b.setAllCaps(false); b.setEnabled(enabled);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = dp(8);
        box.addView(b, lp);
        return b;
    }

    // step 1
    private View renderChoose() {
        if (needed != null) box.addView(line(needed.reason, 14, WARN, true));
        box.addView(line("The game's code has to be rebuilt for this phone, once. Your saves are not touched.", 14, TEXT, false));
        View first = null;
        String sub; int subColor = DIM; boolean ok = plan != null && plan.blocker == null;
        String title = resumePercent >= 0 ? "Continue the build (" + resumePercent + " % done)" : quickKept ? "Quick rebuild on this phone" : "Build on this phone";
        if (plan == null) sub = "";
        else if (!ok) { sub = plan.blocker; subColor = WARN; }
        else sub = (resumePercent >= 0 ? "The finished parts are kept. " : quickKept ? "Reuses the files of the last build: only what changed is built again. " : "")
            + (quickKept ? "about a minute" : AotBuild.about(plan.etaSeconds * (resumePercent >= 0 ? 1 - resumePercent / 100.0 : 1))) + " with the charger · needs about "
            + String.format(Locale.ROOT, "%.0f", Math.ceil(AotBuild.ramNeededGb(plan.jobs))) + " GB of free memory and "
            + String.format(Locale.ROOT, "%.1f", plan.storageNeed / 1073741824.0) + " GB of free storage";
        View build = row(title, sub, subColor, ok, () -> { step = Step.CONFIRM; understood = noCharger = false; startBattery(); render(); });
        if (ok) first = build;
        View pc = row("Build on a PC", "Opens the Rebuild Tool's page in your browser. It makes an aot folder that you copy into the game folder.", DIM, true, () -> {
            pcOpened = true; recheckNote = null; host.openToolPage(NativeCpu.TOOL_URL); render();
        });
        if (first == null) first = pc;
        if (pcOpened) {
            box.addView(line("When the aot folder from the PC is in the game folder, choose Check again. You can also import libbbaot.so and libbbaot.json from Menu > Native CPU.", 13, TEXT, false));
            if (recheckNote != null) box.addView(line(recheckNote, 13, WARN, false));
            box.addView(button("Check again", true, () -> host.recheck()));
        }
        if (needed != null) row("Quit", "Closes the app.", DIM, true, host::quit);
        else row("Close", null, DIM, true, () -> dialog.dismiss());
        return first;
    }

    // step 2
    private View renderConfirm() {
        TextView title = line("Before you build", 18, GOLD, true);
        box.addView(title);
        box.addView(line(quickKept ? "The update reuses the files of the last build, so it takes only a few minutes at most. The phone runs its fast cores at full "
            + "speed and gets warm meanwhile."
            : "For several minutes the phone runs its fast cores at full speed and gets hot: the CPU reaches about 95 °C. Take it out of a case and "
            + "do not hold it on your lap."
            + (update ? " There are no kept build files yet, so this is a full rebuild, once: later updates are quick." : ""), 14, WARN, false));
        chargerLine = line("", 14, TEXT, false); box.addView(chargerLine);
        if (host.gameRunning()) box.addView(line("The game is running: it is closed when the build starts.", 13, WARN, false));
        box.addView(line("You can switch to other apps while it builds, but do not close Bloodborne from the recent apps. If the build stops, it goes on "
            + "where it left off.", 13, DIM, false));
        heatBox = check("I understand the phone will get hot", understood, v -> { understood = ((CheckBox) v).isChecked(); updateConfirm(); });
        noChargerBox = check("Build without the charger anyway", noCharger, v -> { noCharger = ((CheckBox) v).isChecked(); updateConfirm(); });
        startButton = button("Start build", false, this::startBuild);
        button("Back", true, () -> { step = Step.CHOOSE; stopBattery(); if (needed == null) dialog.dismiss(); else render(); });
        updateConfirm();
        return heatBox;
    }
    private CheckBox check(String text, boolean value, View.OnClickListener l) {
        CheckBox c = new CheckBox(activity); c.setText(text); c.setTextColor(TEXT); c.setTextSize(15); c.setChecked(value); c.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = dp(8);
        box.addView(c, lp);
        return c;
    }
    private boolean charging; private int batteryPercent = -1;
    private void startBattery() {
        stopBattery();
        batteryReceiver = new android.content.BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent i) { readBattery(i); updateConfirm(); }
        };
        readBattery(activity.registerReceiver(batteryReceiver, new IntentFilter(Intent.ACTION_BATTERY_CHANGED)));
    }
    private void stopBattery() {
        if (batteryReceiver != null) { try { activity.unregisterReceiver(batteryReceiver); } catch (RuntimeException ignored) {} batteryReceiver = null; }
    }
    private void readBattery(Intent i) {
        if (i == null) return;
        charging = i.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
        int level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1), scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
        batteryPercent = level >= 0 && scale > 0 ? level * 100 / scale : -1;
    }
    private static String chargerText(boolean charging, int percent) {
        String battery = percent >= 0 ? " · battery " + percent + " %" : "";
        return charging ? "Charger connected" + battery : "Not charging" + battery + ". On the battery the build is slower and uses a lot of it.";
    }
    private void updateConfirm() {
        if (chargerLine == null || step != Step.CONFIRM) return;
        chargerLine.setText(chargerText(charging, batteryPercent)); chargerLine.setTextColor(charging ? GOOD : WARN);
        noChargerBox.setVisibility(charging ? View.GONE : View.VISIBLE);
        startButton.setEnabled(understood && (charging || noCharger));
    }

    private void startBuild() {
        final File gameDir = host.gameDir();
        if (gameDir == null) { host.toast("Choose the game folder first."); return; }
        stopBattery();
        if (plan == null) return;
        Runnable go = () -> {
            // Read again at this moment: the game that was running has let go of its memory by now.
            AotBuild.Plan q = AotBuild.plan(true, AotBuild.memAvailableKb(new File("/proc/meminfo")), activity.getFilesDir().getUsableSpace(),
                AotBuild.maxFreqs(new File("/sys/devices/system/cpu")), resumePercent >= 0 || quickKept);
            if (q.blocker != null) { plan = q; step = Step.CHOOSE; if (needed == null) needed = new NativeCpu.Needed(NativeCpu.Need.OTHER, q.blocker); render(); return; }
            if (full) { AotBuild.deleteWorkFiles(host.base()); full = false; quickKept = false; resumePercent = -1; } // a full rebuild: nothing is reused
            AotBuildService.start(activity, gameDir.getPath(), q.jobs);
            step = Step.BUILDING; render();
        };
        if (host.gameRunning()) host.stopGame(go); else go.run();
    }

    // step 3
    private View renderBuilding(AotBuildService.Status s) {
        stageText = line(s.label, 17, GOLD, true); box.addView(stageText);
        bar = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(1000);
        box.addView(bar, new LinearLayout.LayoutParams(-1, -2));
        percentText = line("", 15, TEXT, true); box.addView(percentText);
        etaText = line("", 14, TEXT, false); box.addView(etaText);
        reusedText = line("", 13, GOOD, false); box.addView(reusedText);
        elapsedText = line("", 13, DIM, false); box.addView(elapsedText);
        tempText = line("", 13, DIM, false); box.addView(tempText);
        chargerText = line("", 13, DIM, false); box.addView(chargerText);
        memText = line("", 13, DIM, false); box.addView(memText);
        noteText = line("", 13, WARN, false); box.addView(noteText);
        updateBuilding(s);
        Button background = button("Run in background", true, () -> dialog.dismiss());
        button("Cancel", true, () -> new AlertDialog.Builder(activity).setTitle("Cancel build?")
            .setMessage("Finished parts are kept; you can continue later.")
            .setNegativeButton("Keep building", null).setPositiveButton("Cancel build", (d, w) -> AotBuildService.cancel()).show());
        return background;
    }
    private void updateBuilding(AotBuildService.Status s) {
        if (stageText == null) return;
        stageText.setText(s.label);
        if (s.phase == AotBuildService.Phase.PREPARING) { bar.setIndeterminate(true); percentText.setText(""); etaText.setText("This takes a moment."); }
        else {
            bar.setIndeterminate(false); bar.setProgress((int) Math.round(s.fraction * 1000));
            percentText.setText(s.percent() + " %");
            etaText.setText(AotBuild.left(s.etaSeconds));
        }
        if (s.reused >= 0 && s.reusedOf > 0) { reusedText.setVisibility(View.VISIBLE); reusedText.setText("Reused " + s.reused + " of " + s.reusedOf + " parts from the last build"); }
        else reusedText.setVisibility(View.GONE);
        elapsedText.setText(s.phase == AotBuildService.Phase.PREPARING ? "" : "Elapsed " + AotBuild.clock(s.elapsedMs) + (s.jobs > 0 ? " · " + s.jobs + (s.jobs == 1 ? " job" : " jobs") : ""));
        if (Double.isNaN(s.tempC)) tempText.setVisibility(View.GONE);
        else { tempText.setVisibility(View.VISIBLE); tempText.setText(String.format(Locale.ROOT, "CPU temperature %.0f °C", s.tempC)); tempText.setTextColor(s.tempC >= 95 ? WARN : DIM); }
        chargerText.setText(chargerText(s.charging, s.batteryPercent)); chargerText.setTextColor(s.charging ? GOOD : WARN);
        if (s.rssKb < 0) memText.setVisibility(View.GONE);
        else { memText.setVisibility(View.VISIBLE); memText.setText(String.format(Locale.ROOT, "Memory in use %.1f GB", s.rssKb / 1048576.0)); }
        StringBuilder note = new StringBuilder();
        if (s.veryHot) note.append("The CPU has stayed at 95 °C or more. The phone slows itself down to cool off, so the build takes longer. If you can, take it out of a case or put it somewhere cool.\n");
        if (s.notificationsOff) note.append("Notifications are off for this app, so the progress shows only here. You can turn them on in the phone's settings.");
        noteText.setText(note.toString().trim()); noteText.setVisibility(note.length() == 0 ? View.GONE : View.VISIBLE);
    }

    // ---------------------------------------------------------------- the update prompt

    /** A library that works but was built with older catalogs: not blocking. "Later" is remembered per catalog hash (the prompt comes back when the
     *  app ships newer catalogs). `quick`: kept build files from a finished build exist. */
    static final String LATER_KEY = "native_update_later";
    void offerUpdate(String catalogHash, boolean quick) {
        if (showing()) return;
        String msg = "New fixes need a quick rebuild of the native code. "
            + (quick ? "It reuses the files of the last build and takes about a minute."
                     : "This time it is a full rebuild (several minutes, the phone gets hot), once: later updates are quick.")
            + " Until then the game runs as before, without the new fixes. Your saves are not touched.";
        Runnable later = () -> activity.getSharedPreferences("bloodborne", Context.MODE_PRIVATE).edit().putString(LATER_KEY, catalogHash).apply();
        AlertDialog d = new AlertDialog.Builder(activity).setTitle("Native code update available").setMessage(msg)
            .setPositiveButton("Update now", (x, w) -> showRebuild(false, true))
            .setNegativeButton("Later", (x, w) -> later.run())
            .setOnCancelListener(x -> later.run())
            .setOnDismissListener(x -> { if (!showing()) host.closed(); })
            .create();
        d.show();
    }

    // step 4
    private View renderDone(AotBuildService.Status s) {
        box.addView(line("Native code ready", 18, GOOD, true));
        box.addView(line("Built in " + AotBuild.clock(s.builtMs) + " and checked.", 15, TEXT, false));
        return button("Start game", true, () -> { AotBuildService.acknowledge(); dialog.dismiss(); host.startGame(); });
    }

    private View renderFailed(AotBuildService.Status s) {
        box.addView(line("The build stopped", 18, WARN, true));
        box.addView(line(s.error == null ? "It failed." : s.error, 14, TEXT, false));
        box.addView(line("Finished parts are kept: Retry goes on from there.", 13, DIM, false));
        Button retry = button("Retry", true, () -> { AotBuildService.acknowledge(); retry(); });
        button("Report a problem", true, () -> { host.report(); });
        button("Close", true, () -> { AotBuildService.acknowledge(); dialog.dismiss(); });
        return retry;
    }
    /** Retry: the two confirmations were given already; the plan is read again (the memory may have changed). */
    private void retry() {
        final int gen = ++generation;
        step = Step.CHECKING; render();
        Thread t = new Thread(() -> {
            AotBuild.Plan p = AotBuild.plan(true, AotBuild.memAvailableKb(new File("/proc/meminfo")), activity.getFilesDir().getUsableSpace(),
                AotBuild.maxFreqs(new File("/sys/devices/system/cpu")), true);
            main.post(() -> {
                if (closed || gen != generation) return;
                plan = p;
                if (p.blocker != null) { needed = needed == null ? new NativeCpu.Needed(NativeCpu.Need.OTHER, p.blocker) : needed; step = Step.CHOOSE; render(); }
                else startBuild();
            });
        }, "RebuildDialog-retry");
        t.setDaemon(true); t.start();
    }

    // ---------------------------------------------------------------- the build's state

    private void onStatus() {
        if (closed) return;
        AotBuildService.Status s = AotBuildService.status();
        switch (s.phase) {
            case PREPARING: case RUNNING: case INSTALLING:
                if (step != Step.BUILDING) { step = Step.BUILDING; stopBattery(); render(); } else updateBuilding(s);
                break;
            case DONE: if (step != Step.DONE) { step = Step.DONE; render(); } break;
            case FAILED: if (step != Step.FAILED) { step = Step.FAILED; render(); } break;
            case CANCELLED:
                AotBuildService.acknowledge();
                if (step == Step.BUILDING || step == Step.CHECKING) { host.toast("Build cancelled. The finished parts are kept."); loadPlan(false); }
                break;
            default: break;
        }
    }
}
