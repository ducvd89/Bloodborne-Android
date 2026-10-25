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
import android.widget.TextView;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Menu -> Native CPU: the status and import page of the game's native code, built from the player's own game: on the phone (Rebuild on this
 *  phone: RebuildDialog) or on a PC with the Rebuild Tool (NativeCpu.TOOL_URL) and imported here as libbbaot.so + libbbaot.json (NativeCpu).
 *  The game cannot run without it, so there is no switch: the page shows what is installed and whether it fits this game, imports a picked
 *  pair (the files are checked: ABI version, game, checksum), removes it, rebuilds it, and leads to the tool and to the game folder choice.
 *  It cannot be downloaded: it is derived from the player's own game. Rows take the gamepad (D-pad, A, B) and touch. */
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
        void chooseGameFolder();                      // the activity's own folder choice (it imports the folder's aot/ by itself)
        void rebuildOnPhone();                        // the rebuild dialog, from its confirmations
        void openUrl(String url);
        void changed();                               // the installed library changed (imported, removed): the activity checks again
        void closed();                                // the dialog went away (the activity returns to immersive mode)
    }

    private final Activity activity;
    private final Host host;
    private final Handler main = new Handler(Looper.getMainLooper());
    private AlertDialog dialog;
    private LinearLayout box;
    private Button importButton, removeButton, folderButton;
    private boolean busy, restartNote;
    /** The first file of a pair, kept while the player picks the other one. */
    private Uri pendingSo, pendingJson;
    private String pendingJsonText;
    private String progressText;
    private volatile boolean closed;

    NativeCpuDialog(Activity activity, Host host) {
        this.activity = activity; this.host = host;
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
        return NativeCpu.identity(dir == null ? null : new File(dir));
    }

    /** The state for the drawer row and the activity (cached image hash only: cheap). */
    NativeCpu.State quickState() {
        String[] id = gameIdentity();
        return NativeCpu.state(host.base(), NativeCpu.deviceImageHash(host.base(), false), id[0], id[1]);
    }

    /** The state the game's start is decided by: the image hash computed when it is not cached yet, and the library's checksum checked. Blocking
     *  (about a second on a phone, once per file): not for the UI thread. */
    NativeCpu.State startState() {
        String[] id = gameIdentity();
        NativeCpu.State s = NativeCpu.state(host.base(), NativeCpu.deviceImageHash(host.base(), true), id[0], id[1]);
        if (s.installed && s.info != null && s.problem == null && !s.manualOverride) {
            String bad = NativeCpu.checksumProblem(host.base(), s.info);
            if (bad != null) s.problem = bad;
        }
        return s;
    }

    void show() {
        closed = false; pendingSo = pendingJson = null; pendingJsonText = null; busy = false; restartNote = false; progressText = null;
        box = new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(4), dp(24), dp(8));
        ScrollView scroll = new ScrollView(activity); scroll.addView(box);
        dialog = new AlertDialog.Builder(activity).setTitle("Native CPU (required)").setView(scroll)
            .setPositiveButton("Choose game folder", null).setNeutralButton("Import libbbaot.so…", null)
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
        folderButton = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(v -> { if (!busy) dialog.dismiss(); });
        importButton.setOnClickListener(v -> pickFiles());
        folderButton.setOnClickListener(v -> { if (!busy) { dialog.dismiss(); host.chooseGameFolder(); } });
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
            NativeCpu.State s = NativeCpu.state(host.base(), hash, id[0], id[1]);
            main.post(() -> { if (!closed) render(s); });
        }, "NativeCpu-state");
        t.setDaemon(true); t.start();
    }

    private void render(NativeCpu.State s) {
        if (box == null) return;
        box.removeAllViews();
        box.addView(line("Bloodborne runs as native ARM64 code that is built from your own game files. It cannot be downloaded. Build it on this phone "
            + "(a few minutes, the phone gets hot), or on a PC with the Rebuild Tool and copy its aot folder into the game folder.", 13, TEXT, false));
        Button rebuildButton = new Button(activity); rebuildButton.setText("Rebuild on this phone"); rebuildButton.setAllCaps(false);
        rebuildButton.setEnabled(!busy);
        rebuildButton.setOnClickListener(v -> { if (!busy) { dialog.dismiss(); host.rebuildOnPhone(); } });
        LinearLayout.LayoutParams rlp = new LinearLayout.LayoutParams(-2, -2); rlp.topMargin = dp(8);
        box.addView(rebuildButton, rlp);
        box.addView(linkButton("Rebuild Tool for PC", NativeCpu.DOWNLOAD_URL, 4));
        box.addView(linkButton("How to use it", NativeCpu.GUIDE_URL, 4));

        TextView heading = line("STATUS", 11, GOLD, true); heading.setLetterSpacing(0.15f); heading.setPadding(0, dp(14), 0, dp(2));
        box.addView(heading);
        if (s == null) {
            box.addView(line(progressText != null ? progressText : "Checking…", 15, progressText != null ? GOLD : DIM, false));
            setButtons(false);
            return;
        }
        if (!s.installed) {
            box.addView(line("Not installed: the game cannot start", 16, WARN, true));
            box.addView(line("Rebuild it on this phone, choose a game folder that has an aot folder, or import libbbaot.so and libbbaot.json from a PC.", 13, DIM, false));
        } else if (s.problem != null || s.info == null) {
            box.addView(line("Installed, but it cannot be used", 16, WARN, true));
            box.addView(line(s.problem, 13, WARN, false));
        } else {
            NativeCpu.Info i = s.info;
            String head;
            int color;
            if (s.match == NativeCpu.Match.MISMATCH) { head = "Installed: not for this game"; color = WARN; }
            else if (s.manualOverride) { head = "Set by hand in bionic-local.sh"; color = WARN; }
            else { head = "Installed and ready"; color = GOOD; }
            box.addView(line(head, 16, color, true));
            if (s.match == NativeCpu.Match.MISMATCH) box.addView(line(s.matchNote, 13, WARN, false));
            else if (s.match == NativeCpu.Match.MATCH) box.addView(line("Matches this game (image " + i.imageHash + ").", 13, GOOD, false));
            else box.addView(line(s.matchNote, 13, DIM, false));
            if (s.manualOverride) box.addView(line("bionic-local.sh sets BB_AOT: that library runs, not this one.", 13, WARN, false));
            String built = (i.gameTitle != null ? i.gameTitle + " · " : "") + (i.gameId != null ? i.gameId : "?")
                + " version " + (i.gameVersion != null ? i.gameVersion : "?");
            box.addView(line("Built from: " + built, 13, DIM, false));
            box.addView(line(String.format(Locale.ROOT, "%,d units in %,d functions · %s", i.unitCount, i.functionCount, NativeCpu.megabytes(i.soSize)), 13, DIM, false));
            box.addView(line("Made by " + (i.tool != null ? i.tool : "bb-recomp") + (i.created != null ? " on " + i.created.replace('T', ' ').replace("Z", " UTC") : ""), 13, DIM, false));
        }

        if (restartNote) box.addView(line("Restart the game to apply.", 14, WARN, true));
        setButtons(!busy);
        if (s.installed) {
            removeButton = new Button(activity); removeButton.setText("Remove it"); removeButton.setAllCaps(false);
            removeButton.setOnClickListener(v -> new AlertDialog.Builder(activity).setTitle("Remove the native CPU?")
                .setMessage("Deletes libbbaot.so and libbbaot.json from the app. The game cannot start without them: you can import them again, or choose the tool's game folder.")
                .setNegativeButton("Cancel", null).setPositiveButton("Remove", (d, w) -> {
                    NativeCpu.remove(host.base()); restartNote = host.gameRunning(); host.toast("Native CPU removed"); refresh(); host.changed();
                }).show());
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.topMargin = dp(12);
            box.addView(removeButton, lp);
        }
    }

    private Button linkButton(String label, String url, int topDp) {
        Button b = new Button(activity); b.setText(label); b.setAllCaps(false);
        b.setOnClickListener(v -> host.openUrl(url));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2); lp.topMargin = dp(topDp);
        b.setLayoutParams(lp);
        return b;
    }

    private void setButtons(boolean on) {
        if (importButton != null) importButton.setEnabled(on);
        if (folderButton != null) folderButton.setEnabled(on);
    }

    // ---------------------------------------------------------------- import

    /** Opens the file picker for the pair (the full-screen message's Import button). */
    void importNow() { pickFiles(); }

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
                if (failure == null) host.changed();
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
