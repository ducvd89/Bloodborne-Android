package com.ducvd89.bloodborne;

import android.app.Activity;
import android.app.AlertDialog;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.util.concurrent.atomic.AtomicBoolean;

/** One progress dialog for installs (mod archives, mod and game PKGs): a line of text, a bar, a detail
 *  line ("12.3 / 41.0 GB · 30% · about 4 min left") and Cancel (sets the flag the work polls; a running
 *  process is destroyed by its runner). All methods on the UI thread. It keeps the screen on, as the
 *  activity does: a PKG takes minutes and the phone must not sleep. */
final class PkgProgressDialog {
    private final AlertDialog dialog;
    private final TextView message, detail;
    private final ProgressBar bar;
    private final Button stop;

    PkgProgressDialog(Activity activity,String title,String first,AtomicBoolean cancel) {
        float density=activity.getResources().getDisplayMetrics().density;
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(Math.round(24*density),Math.round(12*density),Math.round(24*density),Math.round(8*density));
        message=new TextView(activity); message.setTextSize(15); message.setText(first);
        bar=new ProgressBar(activity,null,android.R.attr.progressBarStyleHorizontal); bar.setMax(1000);
        bar.setIndeterminate(true);
        detail=new TextView(activity); detail.setTextSize(13); detail.setAlpha(0.75f);
        box.addView(message); box.addView(bar,new LinearLayout.LayoutParams(-1,-2)); box.addView(detail);
        dialog=new AlertDialog.Builder(activity).setTitle(title).setView(box).setCancelable(false)
            .setNegativeButton("Cancel",null).create();
        dialog.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        dialog.show();
        stop=dialog.getButton(AlertDialog.BUTTON_NEGATIVE);
        stop.setOnClickListener(v -> { cancel.set(true); stop.setEnabled(false); message.setText("Cancelling…"); });
        dialog.setOnKeyListener((d,code,e) -> {
            if (code==KeyEvent.KEYCODE_BUTTON_A || e.getScanCode()==304) {
                if (e.getAction()==KeyEvent.ACTION_UP) stop.performClick();
                return true;
            }
            return false;
        });
    }

    void indeterminate(String text) { bar.setIndeterminate(true); message.setText(text); detail.setText(""); }
    /** Cancelling is no longer possible (the last step). */
    void locked(String text) { stop.setEnabled(false); indeterminate(text); }
    /** A step with a known total: "text 40%". */
    void fraction(String text,long done,long of) {
        if (!stop.isEnabled()) return;
        if (of>0) {
            bar.setIndeterminate(false); bar.setProgress((int)Math.min(1000,done*1000/of));
            message.setText(text+" "+Math.min(100,done*100/of)+"%"); detail.setText("");
        } else message.setText(text+" "+PkgProgress.bytes(done));
    }
    /** A PKG step: its label (or `fallback`), the bar (permille -1: unknown), "12.3 / 41.0 GB · 30% · about 4 min left". */
    void pkg(String label,String fallback,int permille,String text) {
        if (!stop.isEnabled()) return;
        message.setText(label.isEmpty() ? fallback : label);
        if (permille<0) bar.setIndeterminate(true);
        else { bar.setIndeterminate(false); bar.setProgress(permille); }
        detail.setText(text);
    }
    void dismiss() { try { dialog.dismiss(); } catch (Exception ignored) {} }
}
