package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.StateListDrawable;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.Map;

/** Remap buttons: which button each button of the physical gamepad sends (PadBridge.remap). Saved at once as
 *  "pad_remap" ("cross=circle,circle=cross": only the changed ones). The on-screen controller is not remapped. */
final class ButtonRemapDialog {
    private static final String KEY="pad_remap";
    private static final String[] LABELS={"Cross","Circle","Square","Triangle","L1","R1","L2","R2","L3","R3","Options","Touchpad",
        "D-pad Up","D-pad Down","D-pad Left","D-pad Right"};
    private static final int GOLD=0xffc6ad76, TEXT=0xffe8dfc9;

    private final Activity activity;
    private final PadBridge pad;
    private final TextView[] rows=new TextView[PadBridge.BUTTONS.length];

    ButtonRemapDialog(Activity activity,PadBridge pad) { this.activity=activity; this.pad=pad; }

    private SharedPreferences prefs() { return activity.getSharedPreferences("bloodborne",Activity.MODE_PRIVATE); }
    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }

    /** The saved table, for PadBridge at start. */
    static Map<String,String> load(SharedPreferences prefs) { return PadBridge.parse(prefs.getString(KEY,"")); }
    /** The drawer row's second line. */
    static String summary(SharedPreferences prefs) {
        int n=load(prefs).size();
        return n==0 ? "Default" : n+(n==1 ? " button changed" : " buttons changed");
    }
    private static String label(String name) {
        for (int i=0;i<PadBridge.BUTTONS.length;++i) if (PadBridge.BUTTONS[i].equals(name)) return LABELS[i];
        return "None";
    }

    private void save(Map<String,String> map) {
        prefs().edit().putString(KEY,PadBridge.format(map)).apply();
        pad.remap(map);
    }
    private void refresh() {
        Map<String,String> map=load(prefs());
        for (int i=0;i<rows.length;++i) {
            String to=map.get(PadBridge.BUTTONS[i]);
            boolean changed=to!=null;
            rows[i].setText(LABELS[i]+"  →  "+(changed ? label(to) : LABELS[i]));
            rows[i].setTextColor(changed ? GOLD : TEXT);
        }
    }

    /** The targets of one physical button: the 16 buttons and None; picking one saves. */
    private void pick(final int index) {
        final String from=PadBridge.BUTTONS[index];
        Map<String,String> map=load(prefs());
        String now=map.containsKey(from) ? map.get(from) : from;
        final String[] names=new String[PadBridge.BUTTONS.length+1], labels=new String[names.length];
        int checked=0;
        for (int i=0;i<names.length;++i) {
            names[i]=i<PadBridge.BUTTONS.length ? PadBridge.BUTTONS[i] : "none";
            labels[i]=label(names[i]);
            if (names[i].equals(now)) checked=i;
        }
        AlertDialog picker=new AlertDialog.Builder(activity).setTitle(LABELS[index]+" sends")
            .setSingleChoiceItems(labels,checked,(d,which) -> {
                Map<String,String> m=load(prefs());
                if (names[which].equals(from)) m.remove(from); else m.put(from,names[which]);
                save(m); refresh(); d.dismiss();
            }).setNegativeButton("Cancel",null).create();
        padKeys(picker); picker.show();
    }

    /** B closes it, A taps the focused control, like the drawer. */
    private static void padKeys(AlertDialog dialog) {
        dialog.setOnKeyListener((d,code,e) -> {
            if (code==KeyEvent.KEYCODE_BUTTON_B || e.getScanCode()==305) {
                if (e.getAction()==KeyEvent.ACTION_UP) dialog.dismiss();
                return true;
            }
            if (code==KeyEvent.KEYCODE_BUTTON_A || e.getScanCode()==304) {
                View f=dialog.getCurrentFocus();
                if (e.getAction()==KeyEvent.ACTION_UP && f!=null) f.performClick();
                return true;
            }
            return false;
        });
    }

    private TextView row(final int index) {
        TextView t=new TextView(activity); t.setTextSize(16); t.setPadding(dp(12),dp(10),dp(12),dp(10));
        t.setMinHeight(dp(48)); t.setGravity(Gravity.CENTER_VERTICAL);
        t.setFocusable(true); t.setClickable(true); t.setOnClickListener(v -> pick(index));
        StateListDrawable bg=new StateListDrawable();
        bg.addState(new int[]{android.R.attr.state_focused},new ColorDrawable(0x33c6ad76));
        bg.addState(new int[]{android.R.attr.state_pressed},new ColorDrawable(0x33c6ad76));
        bg.addState(new int[]{},new ColorDrawable(0));
        t.setBackground(bg);
        return t;
    }

    void show() {
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16),dp(4),dp(16),dp(8));
        TextView note=new TextView(activity); note.setTextSize(13); note.setAlpha(0.7f); note.setPadding(dp(12),0,dp(12),dp(8));
        note.setText("What each button of your gamepad sends to the game. Changes apply at once. The on-screen controller and the sticks are not changed.");
        box.addView(note);
        for (int i=0;i<rows.length;++i) { rows[i]=row(i); box.addView(rows[i],new LinearLayout.LayoutParams(-1,-2)); }
        refresh();
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        AlertDialog dialog=new AlertDialog.Builder(activity).setTitle("Remap buttons").setView(scroll)
            .setNeutralButton("Reset to default",null).setPositiveButton("Close",null).create();
        padKeys(dialog); dialog.show();
        Button reset=dialog.getButton(AlertDialog.BUTTON_NEUTRAL);
        reset.setAllCaps(false);
        reset.setOnClickListener(v -> { save(new java.util.HashMap<>()); refresh(); }); // the dialog stays open
        rows[0].requestFocus();
    }
}
