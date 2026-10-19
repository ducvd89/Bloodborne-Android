package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Typeface;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.util.LinkedHashMap;
import java.util.Map;

/** Debug options: switches for testing, kept in bbport.ini (run-bionic.sh turns them into BB_* environment
 *  variables at start). Written only on Apply, which restarts the game when something changed. A missing
 *  key is its default. */
final class DebugDialog {
    /** {key, title, note, default}. */
    private static final String[][] FAULT={
        {"debug_fault_prepare","Announce copies before writing","The game tells the GPU side before it writes, so fewer writes are caught late.","1"},
        {"debug_fault_smart_drain","Wait for the GPU only when needed","Off: wait for the GPU on every caught write.","1"},
        {"debug_fault_sleep_lock","Sleeping lock in the fault handler","Off: a spinning lock instead.","1"},
    };
    private static final String[][] EXPERIMENTAL={
        {"unified_memory","Unified memory","The game's buffers (vertex and index data) are shared with the GPU in place instead of copied: less RAM; textures stay copies. Adreno only; other GPUs and drivers that fail the start-up checks keep copies (see the log). Turn off if you see crashes or glitches.","1"},
        {"debug_vram_copy_queue","Free old GPU memory (FIFO)","Keeps GPU copies of game memory under a cap: once it is reached, throws away the oldest copies not used for 20 s, so memory stops growing on long sessions. Turn off if you see missing geometry or stutter.","1"},
    };
    /** The cap of the FIFO above, in MB: a single choice, used only while that switch is on. */
    private static final String CAP_KEY="debug_vram_copy_cap_mb", CAP_SWITCH="debug_vram_copy_queue", CAP_DEFAULT="512";
    private static final String[] CAP_VALUES={"512","768","1024","1536","2048","3072"};
    private static final String CAP_TITLE="GPU memory cap (FIFO)";
    private static final String CAP_NOTE="Only with Free old GPU memory on. Lower = less RAM but more re-copying (stutter).";
    private static final String[][] DIAGNOSTICS={
        {"debug_frame_stats","Frame statistics in the log","Timing and counters per second.","0"},
        {"debug_gpu_profile","GPU time per pass in the log (costs FPS)","Each pass is timed: the game runs slower.","0"},
    };

    private final Activity activity;
    private final SettingsDialog settings;
    private final Runnable restart;

    DebugDialog(Activity activity,SettingsDialog settings,Runnable restart) {
        this.activity=activity; this.settings=settings; this.restart=restart;
    }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private static boolean on(Map<String,String> ini,String[] item) {
        String v=ini.get(item[0]);
        return (v==null || v.isEmpty() ? item[3] : v).equals("1");
    }
    private static String capValue(Map<String,String> ini) {
        String v=ini.get(CAP_KEY);
        for (String c:CAP_VALUES) if (c.equals(v)) return c;
        return CAP_DEFAULT;
    }
    private static int capIndex(String value) {
        for (int i=0;i<CAP_VALUES.length;++i) if (CAP_VALUES[i].equals(value)) return i;
        return 2;
    }
    /** The cap choice, greyed out while the FIFO switch is off. */
    private RadioGroup addCap(LinearLayout box,Map<String,String> ini,final Switch fifo) {
        TextView title=new TextView(activity); title.setText(CAP_TITLE); title.setTextSize(15);
        title.setPadding(0,dp(6),0,dp(2)); box.addView(title);
        final RadioGroup g=new RadioGroup(activity);
        for (int i=0;i<CAP_VALUES.length;++i) {
            RadioButton b=new RadioButton(activity); b.setId(View.generateViewId());
            b.setText(CAP_VALUES[i]+" MB"); b.setTextSize(15); g.addView(b);
            if (i==capIndex(capValue(ini))) g.check(b.getId());
        }
        box.addView(g);
        final TextView hint=note(box,CAP_NOTE);
        final Runnable grey=() -> {
            boolean enabled=fifo.isChecked();
            for (int i=0;i<g.getChildCount();++i) g.getChildAt(i).setEnabled(enabled);
            g.setAlpha(enabled ? 1f : 0.4f); hint.setAlpha(enabled ? 0.7f : 0.4f);
        };
        fifo.setOnCheckedChangeListener((b,c) -> grey.run());
        grey.run();
        return g;
    }
    private static String capChosen(RadioGroup g) {
        for (int i=0;i<g.getChildCount();++i) if (((RadioButton)g.getChildAt(i)).isChecked()) return CAP_VALUES[i];
        return CAP_DEFAULT;
    }
    private TextView heading(LinearLayout box,String title) {
        TextView t=new TextView(activity); t.setText(title); t.setTextSize(15);
        t.setTypeface(Typeface.DEFAULT,Typeface.BOLD); t.setPadding(0,dp(14),0,dp(2));
        box.addView(t); return t;
    }
    private TextView note(LinearLayout box,String text) {
        TextView t=new TextView(activity); t.setTextSize(13); t.setAlpha(0.7f); t.setText(text);
        t.setPadding(0,0,0,dp(4)); box.addView(t); return t;
    }
    private void add(LinearLayout box,String[][] items,Map<String,String> ini,Map<String,Switch> out) {
        for (String[] item:items) {
            Switch s=new Switch(activity); s.setText(item[1]); s.setTextSize(15); s.setChecked(on(ini,item));
            s.setPadding(0,dp(6),0,dp(2)); box.addView(s); out.put(item[0],s);
            note(box,item[2]);
        }
    }

    void show() {
        final Map<String,String> ini=settings.read();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));
        final Map<String,Switch> switches=new LinkedHashMap<>();

        note(box,"For testing. Changes apply after a restart.");
        heading(box,"Write-fault tracking");
        note(box,"How the game's writes to GPU memory are tracked.");
        add(box,FAULT,ini,switches);
        heading(box,"Experimental");
        add(box,EXPERIMENTAL,ini,switches);
        final RadioGroup cap=addCap(box,ini,switches.get(CAP_SWITCH));
        heading(box,"Diagnostics");
        add(box,DIAGNOSTICS,ini,switches);

        Button reset=new Button(activity); reset.setText("Reset to defaults"); reset.setAllCaps(false);
        reset.setOnClickListener(v -> {
            for (String[][] group:new String[][][]{FAULT,EXPERIMENTAL,DIAGNOSTICS})
                for (String[] item:group) switches.get(item[0]).setChecked(item[3].equals("1"));
            ((RadioButton)cap.getChildAt(capIndex(CAP_DEFAULT))).setChecked(true);
        });
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-2,-2); lp.topMargin=dp(16);
        box.addView(reset,lp);

        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        new AlertDialog.Builder(activity).setTitle("Debug options").setView(scroll)
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Apply & restart",(d,w) -> {
                Map<String,String> changed=new LinkedHashMap<>();
                boolean same=true;
                for (Map.Entry<String,Switch> e:switches.entrySet()) {
                    String value=e.getValue().isChecked() ? "1" : "0";
                    changed.put(e.getKey(),value);
                    String[] item=find(e.getKey());
                    if (!(on(ini,item) ? "1" : "0").equals(value)) same=false;
                }
                String chosen=capChosen(cap);
                changed.put(CAP_KEY,chosen);
                if (!capValue(ini).equals(chosen)) same=false;
                if (same) return;
                if (settings.setAll(changed)) restart.run();
                else new AlertDialog.Builder(activity).setMessage("The settings could not be saved.")
                    .setPositiveButton("OK",null).show();
            }).show();
    }

    private static String[] find(String key) {
        for (String[][] group:new String[][][]{FAULT,EXPERIMENTAL,DIAGNOSTICS})
            for (String[] item:group) if (item[0].equals(key)) return item;
        throw new IllegalArgumentException(key);
    }
}
