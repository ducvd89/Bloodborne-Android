// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Typeface;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** "Native code needed": shown when the app takes a game folder that has the game but no native code that fits (MainActivity.startGame,
 *  NativeCpu.neededLead decides and words the lead). It leads with the reason, explains the Rebuild Tool and links to its newest release and
 *  its guide; Import goes to the Native CPU page's file picker. The full-screen message that blocks the game start says the same
 *  (NativeRequiredView). Buttons take the gamepad (D-pad, A, B) and touch. */
final class NativeNeededDialog {
    private static final int TEXT = 0xffe8dfc9, WARN = 0xffe5a04b;

    /** What the dialog needs from the activity. */
    interface Host {
        void openUrl(String url);
        void importFiles();     // the Native CPU page's file picker for libbbaot.so + libbbaot.json
        void closed();          // the dialog went away (the activity returns to immersive mode)
    }

    private final Activity activity;
    private final Host host;
    private AlertDialog dialog;

    NativeNeededDialog(Activity activity, Host host) { this.activity = activity; this.host = host; }

    boolean showing() { return dialog != null && dialog.isShowing(); }

    private int dp(float n) { return Math.round(n * activity.getResources().getDisplayMetrics().density); }
    private TextView line(String text, int size, int color, boolean bold) {
        TextView t = new TextView(activity); t.setText(text); t.setTextSize(size); t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        t.setPadding(0, dp(2), 0, dp(6)); return t;
    }
    private Button button(String label, Runnable action) {
        Button b = new Button(activity); b.setText(label); b.setAllCaps(false);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = dp(6);
        b.setLayoutParams(lp);
        return b;
    }

    /** Shows the dialog; `lead` is NativeCpu.neededLead's text (the reason, or that the folder has no native code). */
    void show(String lead) {
        if (showing()) return;
        LinearLayout box = new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(4), dp(24), dp(8));
        box.addView(line(lead, 14, WARN, true));
        box.addView(line(NativeCpu.NEEDED_TEXT, 14, TEXT, false));
        Button first = button("Download the Rebuild Tool", () -> host.openUrl(NativeCpu.DOWNLOAD_URL));
        box.addView(first);
        box.addView(button("How to use it", () -> host.openUrl(NativeCpu.GUIDE_URL)));
        box.addView(button("Import libbbaot.so…", () -> { dialog.dismiss(); host.importFiles(); }));
        ScrollView scroll = new ScrollView(activity); scroll.addView(box);
        dialog = new AlertDialog.Builder(activity).setTitle(NativeCpu.NEEDED_TITLE).setView(scroll)
            .setNegativeButton("Close", null).create();
        dialog.setOnKeyListener((d, code, e) -> { // B closes it, A taps the focused control, like the other dialogs
            if (code == KeyEvent.KEYCODE_BUTTON_B || e.getScanCode() == 305) {
                if (e.getAction() == KeyEvent.ACTION_UP) dialog.dismiss();
                return true;
            }
            if (code == KeyEvent.KEYCODE_BUTTON_A || e.getScanCode() == 304) {
                View f = dialog.getCurrentFocus();
                if (e.getAction() == KeyEvent.ACTION_UP && f != null) f.performClick();
                return true;
            }
            return false;
        });
        dialog.setOnDismissListener(d -> host.closed());
        dialog.show();
        first.requestFocus();
    }

    /** Ends it (the activity is destroyed). */
    void close() {
        try { if (dialog != null && dialog.isShowing()) dialog.dismiss(); } catch (Exception ignored) {}
    }
}
