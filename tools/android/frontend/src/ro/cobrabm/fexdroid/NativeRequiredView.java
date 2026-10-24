// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/** The full-screen message shown instead of the game when libbbaot.so is missing or does not fit (wrong game, ABI, checksum): this app
 *  runs Bloodborne as native code only, which the player builds once on a PC with the Rebuild Tool. The same text and links as the
 *  "Native code needed" dialog (NativeNeededDialog), and the two ways to bring the result here: the game folder the tool made, or the
 *  library itself. Sits above the game surface and the touch controller, below the menu handle and the drawer. */
final class NativeRequiredView extends ScrollView {
    private static final int TEXT = 0xffe8dfc9, WARN = 0xffe5a04b;

    interface Actions {
        void openDownload();
        void openGuide();
        void chooseGameFolder();
        void importFiles();
    }

    private final LinearLayout box;
    private final TextView reason;

    NativeRequiredView(Activity activity, Actions actions) {
        super(activity);
        setBackgroundColor(0xf0101116);
        setClickable(true); setFocusable(false);
        setFillViewport(true);
        FrameLayout frame = new FrameLayout(activity);
        box = new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(activity, 28);
        box.setPadding(pad, dp(activity, 36), pad, dp(activity, 36));
        TextView title = text(activity, NativeCpu.NEEDED_TITLE, 24, TEXT, true);
        box.addView(title);
        TextView message = text(activity, NativeCpu.NEEDED_TEXT, 16, TEXT, false);
        message.setPadding(0, dp(activity, 12), 0, 0);
        box.addView(message);
        reason = text(activity, "", 14, WARN, false);
        reason.setPadding(0, dp(activity, 10), 0, dp(activity, 6));
        box.addView(reason);
        box.addView(button(activity, "Download the Rebuild Tool", actions::openDownload));
        box.addView(button(activity, "How to use it", actions::openGuide));
        box.addView(button(activity, "Choose game folder", actions::chooseGameFolder));
        box.addView(button(activity, "Import libbbaot.so…", actions::importFiles));
        TextView note = text(activity, "Menu (swipe from the left edge, or the ○ button) has these too, under Native CPU and Game folder.", 12, 0xff8e9097, false);
        note.setPadding(0, dp(activity, 14), 0, 0);
        box.addView(note);
        FrameLayout.LayoutParams p = new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER_VERTICAL);
        frame.addView(box, p);
        addView(frame, new ScrollView.LayoutParams(-1, -1));
        setVisibility(View.GONE);
    }

    /** Shows the message with the reason this library cannot be used (null: just missing). */
    void show(String why) {
        reason.setText(why == null ? "" : why);
        reason.setVisibility(why == null || why.isEmpty() ? View.GONE : View.VISIBLE);
        setVisibility(View.VISIBLE);
        scrollTo(0, 0);
    }
    void hide() { setVisibility(View.GONE); }
    boolean shown() { return getVisibility() == View.VISIBLE; }

    private static int dp(Activity a, float n) { return Math.round(n * a.getResources().getDisplayMetrics().density); }
    private static TextView text(Activity a, String value, int size, int color, boolean bold) {
        TextView t = new TextView(a); t.setText(value); t.setTextSize(size); t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return t;
    }
    private static Button button(Activity a, String label, Runnable action) {
        Button b = new Button(a); b.setText(label); b.setAllCaps(false);
        b.setOnClickListener(v -> action.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2); lp.topMargin = dp(a, 8);
        b.setLayoutParams(lp);
        return b;
    }
}
