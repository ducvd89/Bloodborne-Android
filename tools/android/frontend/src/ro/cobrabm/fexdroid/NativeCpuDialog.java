// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.text.method.LinkMovementMethod;
import android.text.util.Linkify;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Menu -> Native CPU (recompiled): the game's code compiled ahead of time on the player's PC (bb-recomp, docs/NATIVE_CPU.md) and
 *  imported here as libbbaot.so + libbbaot.json (NativeCpu). Shows what is installed and whether it fits this game, imports a picked
 *  pair (the files are checked: ABI version, game, checksum), removes it, and holds the On/Off switch (bbport.ini native_cpu, read by
 *  run-bionic.sh at start). It cannot be downloaded: it is derived from the player's own game. Rows take the gamepad (D-pad, A, B)
 *  and touch. */
final class NativeCpuDialog {
    static final int REQUEST_FILES = 11;
    private static final int TEXT = 0xffe8dfc9, GOLD = 0xffc6ad76, WARN = 0xffe5a04b, GOOD = 0xff8fbf7f, DIM = 0xff8e9097;

    /** What the dialog needs from the activity. */
    interface Host {
        File base();                                  // files/bbport
        String gameDir();                             // the chosen game folder, or null
        void pick(Intent intent, int request);        // startActivityForResult (shows its own error)
        InputStream open(Uri document) throws IOException;
        void toast(String text);
        boolean gameRunning();
        /** Whether the running game was started with the native CPU (valid while it runs). */
        boolean startedNative();
        void restart();
        void closed();                                // the dialog went away (the activity returns to immersive mode)
    }

    private final Activity activity;
    private final SettingsDialog settings;
    private final Host host;
    private final Handler main = new Handler(Looper.getMainLooper());
    private AlertDialog dialog;
    private LinearLayout box;
    private Switch toggle;
    private Button importButton, removeButton, applyButton;
    private boolean busy, restartNote;
    /** The first file of a pair, kept while the player picks the other one. */
    private Uri pendingSo, pendingJson;
    private String pendingJsonText;
    private String progressText;
    private volatile boolean closed;

    NativeCpuDialog(Activity activity, SettingsDialog settings, Host host) {
        this.activity = activity; this.settings = settings; this.host = host;
    }

    private int dp(float n) { return Math.round(n * activity.getResources().getDisplayMetrics().density); }
    private TextView line(String text, int size, int color, boolean bold) {
        TextView t = new TextView(activity); t.setText(text); t.setTextSize(size); t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        t.setPadding(0, dp(2), 0, dp(2)); return t;
    }

    /** The game's id and version from the chosen folder's param.sfo (null each when unknown). */
    private String[] gameIdentity() {
        String dir = host.gameDir();
        if (dir == null) return new String[]{null, null};
        try {
            Map<String, Object> sfo = Sfo.parse(java.nio.file.Files.readAllBytes(new File(dir, "sce_sys/param.sfo").toPath()));
            return new String[]{Sfo.text(sfo, "TITLE_ID"), Sfo.text(sfo, "APP_VER")};
        } catch (IOException | Sfo.Invalid e) { return new String[]{null, null}; }
    }

    /** The state for the drawer row and the activity (cached image hash only: cheap). */
    NativeCpu.State quickState() {
        String[] id = gameIdentity();
        return NativeCpu.state(host.base(), settings.read(), NativeCpu.deviceImageHash(host.base(), false), id[0], id[1]);
    }

    void show() {
        closed = false; pendingSo = pendingJson = null; pendingJsonText = null; busy = false; restartNote = false; progressText = null;
        box = new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(4), dp(24), dp(8));
        ScrollView scroll = new ScrollView(activity); scroll.addView(box);
        dialog = new AlertDialog.Builder(activity).setTitle("Native CPU (recompiled)").setView(scroll)
            .setPositiveButton("Apply & restart", null).setNeutralButton("Import libbbaot.so…", null)
            .setNegativeButton("Close", null).create();
        dialog.setOnKeyListener((d, code, e) -> { // B closes it, A taps the focused control, like the drawer
            if (code == KeyEvent.KEYCODE_BUTTON_B || e.getScanCode() == 305) {
                if (e.getAction() == KeyEvent.ACTION_UP && !busy) dialog.dismiss();
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_A || e.getScanCode() == 304) {
                View f = dialog.getCurrentFocus();
                if (e.getAction() == KeyEvent.ACTION_UP && f != null) f.performClick();
                return true;
            }
            return false;
        });
        dialog.setOnDismissListener(d -> { closed = true; host.closed(); });
        dialog.show();
        // Set after show(): these must not close the dialog by themselves.
        importButton = dialog.getButton(AlertDialog.BUTTON_NEUTRAL);
        applyButton = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> { if (!busy) dialog.dismiss(); });
        importButton.setOnClickListener(v -> pickFiles());
        applyButton.setOnClickListener(v -> apply());
        render(null);
        refresh();
    }

    /** Ends everything (the activity is destroyed). */
    void close() {
        closed = true;
        try { if (dialog != null && dialog.isShowing()) dialog.dismiss(); } catch (Exception ignored) {}
    }

    /** Computes the state (the image hash can take a second) and shows it. */
    private void refresh() {
        final String[] id = gameIdentity();
        Thread t = new Thread(() -> {
            String hash = NativeCpu.deviceImageHash(host.base(), true);
            NativeCpu.State s = NativeCpu.state(host.base(), settings.read(), hash, id[0], id[1]);
            main.post(() -> { if (!closed) render(s); });
        }, "NativeCpu-state");
        t.setDaemon(true); t.start();
    }

    private void render(NativeCpu.State s) {
        if (box == null) return;
        box.removeAllViews();
        box.addView(line("Runs the game's code as ARM64 that was compiled ahead of time, instead of translating it while you play: "
            + "no translation stutter in new areas, and less CPU work.", 13, TEXT, false));
        TextView help = line("Build it on a PC from your own game dump with the Rebuild Tool (bb-recomp). It cannot be downloaded.\n" + NativeCpu.DOCS
            + "\nThe tool's output folder is a game folder with aot/ inside: choose it as the game folder and this is imported by itself. "
            + "Or import libbbaot.so and libbbaot.json by hand below.", 13, GOLD, false);
        help.setAutoLinkMask(Linkify.WEB_URLS); help.setLinksClickable(true); help.setMovementMethod(LinkMovementMethod.getInstance());
        help.setLinkTextColor(GOLD);
        box.addView(help);

        TextView heading = line("STATUS", 11, GOLD, true); heading.setLetterSpacing(0.15f); heading.setPadding(0, dp(14), 0, dp(2));
        box.addView(heading);
        if (s == null) {
            box.addView(line(progressText != null ? progressText : "Checking…", 15, progressText != null ? GOLD : DIM, false));
            setButtons(false, false);
            return;
        }
        if (!s.installed) {
            box.addView(line("Not installed", 16, TEXT, true));
            box.addView(line("Import libbbaot.so and libbbaot.json from the PC.", 13, DIM, false));
        } else if (s.problem != null || s.info == null) {
            box.addView(line("Installed, but it cannot be used", 16, WARN, true));
            box.addView(line(s.problem, 13, WARN, false));
        } else {
            NativeCpu.Info i = s.info;
            String head;
            int color;
            if (s.match == NativeCpu.Match.MISMATCH) { head = "Installed: not for this game"; color = WARN; }
            else if (s.manualOverride) { head = "Installed: set by hand in bionic-local.sh"; color = WARN; }
            else if (s.active()) { head = "Installed and on"; color = GOOD; }
            else { head = "Installed, off"; color = TEXT; }
            box.addView(line(head, 16, color, true));
            if (s.match == NativeCpu.Match.MISMATCH) box.addView(line(s.matchNote, 13, WARN, false));
            else if (s.match == NativeCpu.Match.MATCH) box.addView(line("Matches this game (image " + i.imageHash + ").", 13, GOOD, false));
            else box.addView(line(s.matchNote, 13, DIM, false));
            if (s.manualOverride) box.addView(line("bionic-local.sh sets BB_AOT, which wins over the switch below.", 13, WARN, false));
            String built = (i.gameTitle != null ? i.gameTitle + " · " : "") + (i.gameId != null ? i.gameId : "?")
                + " version " + (i.gameVersion != null ? i.gameVersion : "?");
            box.addView(line("Built from: " + built, 13, DIM, false));
            box.addView(line(String.format(Locale.ROOT, "%,d units in %,d functions · %s", i.unitCount, i.functionCount, NativeCpu.megabytes(i.soSize)), 13, DIM, false));
            box.addView(line("Made by " + (i.tool != null ? i.tool : "bb-recomp") + (i.created != null ? " on " + i.created.replace('T', ' ').replace("Z", " UTC") : ""), 13, DIM, false));
        }

        boolean usable = s.installed && s.info != null && s.problem == null;
        toggle = new Switch(activity); toggle.setText("Use the native CPU"); toggle.setTextSize(15); toggle.setTextColor(TEXT);
        toggle.setPadding(0, dp(14), 0, dp(2)); toggle.setChecked(s.enabled); toggle.setEnabled(usable);
        toggle.setOnCheckedChangeListener((b, c) -> updateApply(s));
        box.addView(toggle);
        TextView note = line("Off: the game's code is translated while you play, as before. The switch is read when the game starts.", 13, DIM, false);
        box.addView(note);
        if (restartNote || (host.gameRunning() && s.active() != host.startedNative() && s.installed))
            box.addView(line("Restart the game to apply.", 14, WARN, true));
        box.addView(line("Code that was not compiled, or changed since (patches, mods), still runs on the translator.", 12, DIM, false));
        setButtons(!busy, usable);
        updateApply(s);
        if (s.installed) {
            removeButton = new Button(activity); removeButton.setText("Remove it"); removeButton.setAllCaps(false);
            removeButton.setOnClickListener(v -> new AlertDialog.Builder(activity).setTitle("Remove the native CPU?")
                .setMessage("Deletes libbbaot.so and libbbaot.json from the app. You can import them again. Restart the game to apply.")
                .setNegativeButton("Cancel", null).setPositiveButton("Remove", (d, w) -> {
                    NativeCpu.remove(host.base()); restartNote = host.gameRunning(); host.toast("Native CPU removed"); refresh();
                }).show());
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.topMargin = dp(12);
            box.addView(removeButton, lp);
        }
    }

    private void setButtons(boolean importOn, boolean applyOn) {
        if (importButton != null) importButton.setEnabled(importOn);
        if (applyButton != null) applyButton.setEnabled(applyOn && !busy);
    }

    /** Apply is for a change of the switch only. */
    private void updateApply(NativeCpu.State s) {
        if (applyButton == null || toggle == null) return;
        applyButton.setEnabled(!busy && toggle.isEnabled() && toggle.isChecked() != s.enabled);
    }

    private void apply() {
        if (busy || toggle == null) return;
        Map<String, String> changed = new LinkedHashMap<>();
        changed.put(NativeCpu.SETTING, toggle.isChecked() ? "1" : "0");
        if (!settings.setAll(changed)) { host.toast("The setting could not be saved."); return; }
        dialog.dismiss();
        if (host.gameRunning()) host.restart(); // the game reads it at start
        else host.toast(toggle.isChecked() ? "Native CPU on from the next start" : "Native CPU off from the next start");
    }

    // ---------------------------------------------------------------- import

    private void pickFiles() {
        if (busy) return;
        pickFiles(true, "Choose libbbaot.so and libbbaot.json (select both)");
    }
    private void pickFiles(boolean multiple, String hint) {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
            .putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
        host.toast(hint);
        host.pick(pick, REQUEST_FILES);
    }

    /** The picker's answer: one or two documents. */
    void onResult(int result, Intent data) {
        if (closed || data == null || result != Activity.RESULT_OK) return;
        List<Uri> picked = new ArrayList<>();
        ClipData clip = data.getClipData();
        if (clip != null) for (int i = 0; i < clip.getItemCount(); ++i) picked.add(clip.getItemAt(i).getUri());
        else if (data.getData() != null) picked.add(data.getData());
        for (Uri u : picked) {
            String kind = kindOf(u);
            if (kind == null) { show("That is neither libbbaot.so nor libbbaot.json.", "Choose the two files bb-recomp wrote: libbbaot.so and libbbaot.json."); return; }
            if (kind.equals("so")) { if (pendingSo != null && !pendingSo.equals(u)) { show("Two libraries were chosen.", "Choose one libbbaot.so and its libbbaot.json."); return; } pendingSo = u; }
            else { if (pendingJson != null && !pendingJson.equals(u)) { show("Two description files were chosen.", "Choose one libbbaot.so and its libbbaot.json."); return; } pendingJson = u; }
        }
        if (pendingSo == null) { askFor("libbbaot.so", "libbbaot.json is chosen; now choose libbbaot.so from the same folder."); return; }
        if (pendingJson == null) { askFor("libbbaot.json", "libbbaot.so is chosen; now choose libbbaot.json from the same folder."); return; }
        startImport();
    }

    /** "so" or "json" by the name, else by the first bytes; null when it is neither. */
    private String kindOf(Uri u) {
        String name = nameOf(u);
        if (name != null) {
            String n = name.toLowerCase(Locale.ROOT);
            if (n.endsWith(".json")) return "json";
            if (n.endsWith(".so")) return "so";
        }
        try (InputStream in = host.open(u)) {
            byte[] h = new byte[4]; int n = in.read(h);
            if (n == 4 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') return "so";
            if (n >= 1 && h[0] == '{') return "json";
        } catch (IOException ignored) {}
        return null;
    }

    private String nameOf(Uri u) {
        try (Cursor c = activity.getContentResolver().query(u, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {}
        String p = u.getLastPathSegment();
        return p == null ? null : p.substring(p.lastIndexOf('/') + 1);
    }

    private void askFor(String missing, String message) {
        new AlertDialog.Builder(activity).setTitle("Now choose " + missing).setMessage(message)
            .setNegativeButton("Cancel", (d, w) -> { pendingSo = pendingJson = null; })
            .setPositiveButton("Choose " + missing, (d, w) -> pickFiles(false, "Choose " + missing)).show();
    }

    private void show(String title, String message) {
        pendingSo = pendingJson = null;
        new AlertDialog.Builder(activity).setTitle(title).setMessage(message).setPositiveButton("OK", null).show();
    }

    private void startImport() {
        final Uri soUri = pendingSo, jsonUri = pendingJson;
        pendingSo = pendingJson = null;
        busy = true; progressText = "Checking…"; render(null);
        final String[] id = gameIdentity();
        Thread t = new Thread(() -> {
            String problem = null; NativeCpu.Info info = null;
            try {
                String jsonText;
                try (InputStream in = host.open(jsonUri)) { jsonText = new String(readAll(in, 1 << 20), StandardCharsets.UTF_8); }
                try (InputStream in = host.open(soUri)) {
                    info = NativeCpu.install(host.base(), jsonText, in, 0, id[0], id[1], (done, total) -> {
                        final String p = String.format(Locale.ROOT, "Copying libbbaot.so… %d of %d MB", done >> 20, total >> 20);
                        main.post(() -> { if (!closed && busy) { progressText = p; updateProgress(); } });
                    });
                }
                // On after a successful import (an earlier Off would otherwise hide it).
                Map<String, String> on = new LinkedHashMap<>(); on.put(NativeCpu.SETTING, "1"); settings.setAll(on);
            } catch (NativeCpu.Problem p) { problem = p.getMessage(); }
            catch (IOException | RuntimeException e) {
                problem = "The files could not be read: " + (e.getMessage() != null ? e.getMessage() : e.toString());
                android.util.Log.w("Bloodborne", "native cpu import: " + e);
            }
            final String failure = problem; final NativeCpu.Info done = info;
            main.post(() -> {
                busy = false; progressText = null;
                if (closed) return;
                if (failure != null) {
                    new AlertDialog.Builder(activity).setTitle("Not imported").setMessage(failure).setPositiveButton("OK", null).show();
                } else {
                    restartNote = host.gameRunning();
                    host.toast("Native CPU imported: " + (done.unitCount) + " units");
                }
                refresh();
            });
        }, "NativeCpu-import");
        t.setDaemon(true); t.start();
    }

    private void updateProgress() { render(null); }

    private static byte[] readAll(InputStream in, int limit) throws IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192]; int n;
        while ((n = in.read(buf)) > 0) { out.write(buf, 0, n); if (out.size() > limit) throw new IOException("the file is too large"); }
        return out.toByteArray();
    }
}
