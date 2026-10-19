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
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/** Frame generation: Lossless Scaling (libbloodframe.so in the game process, on the player's own Lossless.dll).
 *  Unlike the graphics settings it applies at once, while the game runs: Apply writes bbport.ini (the next
 *  start) and bbport/lsfg.state (run-bionic.sh writes it at start; libbbgpu follows it, see
 *  gpu/shim/bb_lsfg_state.h), then the display mode follows (MainActivity.pickRefreshRate). FSR 3.1 frame
 *  generation is a setting of the upscaler (Graphics settings, needs a restart) and exclusive with this. */
final class FrameGenDialog {
    private static final String[] VALUES={"0","2","3","4"};
    private static final String[] NAMES={"Off","2× frames","3× frames","4× frames"};

    /** The Lossless.dll side, done by the activity (a file picker, the Steam dialog, the app's storage).
     *  `done` runs on the main thread when the player is back from it. */
    interface LosslessHost {
        /** The Lossless.dll libbbgpu will use, or null. */
        File dll();
        void signIn(Runnable done);
        void choose(Runnable done);
    }

    private final Activity activity;
    private final SettingsDialog settings;
    private final File state;
    private final LosslessHost lossless;
    private final Runnable applied;

    /** @param settings bbport.ini; @param state bbport/lsfg.state; @param applied runs after Apply saved
     *  (the display mode follows the new multiplier). */
    FrameGenDialog(Activity activity,SettingsDialog settings,File state,LosslessHost lossless,Runnable applied) {
        this.activity=activity; this.settings=settings; this.state=state; this.lossless=lossless; this.applied=applied;
    }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private static String get(Map<String,String> ini,String key,String fallback) {
        String v=ini.get(key); return v==null || v.isEmpty() ? fallback : v;
    }
    /** FSR 3.1 frame generation is on at the next start (and, unless Lossless Scaling was on at start, now). */
    private static boolean fsrOn(String frameGeneration,String upscaler,String lsfg) {
        return "1".equals(frameGeneration) && !"off".equals(upscaler) && !"gsr2".equals(upscaler) && (lsfg==null || !lsfg.matches("[234]"));
    }
    private TextView note(LinearLayout box,String text,float alpha) {
        TextView t=new TextView(activity); t.setTextSize(13); t.setAlpha(alpha); t.setText(text);
        t.setPadding(0,dp(4),0,dp(4)); box.addView(t); return t;
    }

    /** The drawer row's second line. */
    static String summary(String frameGeneration,String upscaler,String lsfg,boolean dllFound) {
        if (fsrOn(frameGeneration,upscaler,lsfg)) return "FSR 3.1 (Graphics settings)";
        if (lsfg==null || !lsfg.matches("[234]")) return "Frame generation off";
        return dllFound ? "Frame generation "+lsfg+"×" : "Frame generation: needs Lossless.dll";
    }

    void show() {
        final Map<String,String> ini=settings.read();
        final boolean fsr=fsrOn(get(ini,"frame_generation","0"),get(ini,"upscaler","gsr2"),get(ini,"lsfg_multiplier","0"));
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));

        if (fsr) {
            TextView warning=note(box,"FSR 3.1 frame generation is on (Graphics settings): turn it off to use Lossless Scaling",1f);
            warning.setTypeface(Typeface.DEFAULT,Typeface.BOLD); warning.setTextSize(14);
        }
        note(box,"Bloodframe is this app's frame generation engine. It runs Lossless Scaling's frame generation "
            +"(LSFG 3.1) with the shaders from your own Lossless.dll and adds generated frames between the game's own, "
            +"for smoother motion. It adds a little input lag and works best when the game already runs at 30 FPS or more.",0.8f);
        TextView heading=new TextView(activity); heading.setText("Lossless Scaling"); heading.setTextSize(15);
        heading.setTypeface(Typeface.DEFAULT,Typeface.BOLD); heading.setPadding(0,dp(10),0,dp(4)); box.addView(heading);

        int selected=0;
        for (int i=0;i<VALUES.length;++i) if (VALUES[i].equals(get(ini,"lsfg_multiplier","0"))) selected=i;
        final RadioGroup multiplier=new RadioGroup(activity);
        for (int i=0;i<NAMES.length;++i) {
            RadioButton b=new RadioButton(activity); b.setId(View.generateViewId()); b.setText(NAMES[i]); b.setTextSize(15);
            multiplier.addView(b); if (i==selected) multiplier.check(b.getId());
        }
        box.addView(multiplier);

        final TextView flowLabel=new TextView(activity); flowLabel.setTextSize(14); box.addView(flowLabel);
        final SeekBar flow=new SeekBar(activity); flow.setMax(15); // 0.25 .. 1.00 in steps of 0.05
        float flowInitial=0.75f;
        try { flowInitial=Float.parseFloat(get(ini,"lsfg_flow_scale","0.75")); } catch (NumberFormatException ignored) {}
        flow.setProgress(Math.round((Math.max(0.25f,Math.min(1f,flowInitial))-0.25f)/0.05f));
        SeekBar.OnSeekBarChangeListener flowText=new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s,int p,boolean user) {
                flowLabel.setText(String.format(Locale.ROOT,"Flow scale: %.2f (lower is faster, the motion is coarser)",0.25f+p*0.05f));
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        };
        flow.setOnSeekBarChangeListener(flowText); flowText.onProgressChanged(flow,flow.getProgress(),false);
        box.addView(flow);
        final Switch performance=new Switch(activity); performance.setText("Performance mode (recommended)"); performance.setTextSize(15);
        performance.setChecked("1".equals(get(ini,"lsfg_performance","1"))); performance.setPadding(0,dp(6),0,dp(6));
        box.addView(performance);
        final Switch half=new Switch(activity); half.setText("Half precision, FP16 (recommended)"); half.setTextSize(15);
        half.setChecked("1".equals(get(ini,"lsfg_fp16","1"))); half.setPadding(0,dp(6),0,dp(6));
        box.addView(half);
        final TextView halfNote=note(box,"Recommended: 2× with performance mode and FP16 on. "
            +"FP16 is faster on most phones and falls back to full precision if the GPU or driver can't. "
            +"A change restarts frame generation for a moment, while you play.",0.7f);
        halfNote.setPadding(0,0,0,dp(4));

        final TextView dllStatus=new TextView(activity); dllStatus.setTextSize(13); dllStatus.setPadding(0,dp(6),0,dp(2));
        box.addView(dllStatus);
        final Runnable showDll=() -> {
            File dll=lossless.dll();
            dllStatus.setText(dll==null ? "Lossless.dll: not found" : "Lossless.dll: found at "+dll.getPath());
        };
        showDll.run();
        LinearLayout dllButtons=new LinearLayout(activity); dllButtons.setOrientation(LinearLayout.HORIZONTAL);
        Button steam=new Button(activity); steam.setText("Sign in with Steam"); steam.setAllCaps(false);
        steam.setOnClickListener(v -> lossless.signIn(showDll));
        Button choose=new Button(activity); choose.setText("Choose Lossless.dll…"); choose.setAllCaps(false);
        choose.setOnClickListener(v -> lossless.choose(showDll));
        dllButtons.addView(steam,new LinearLayout.LayoutParams(0,-2,1)); dllButtons.addView(choose,new LinearLayout.LayoutParams(0,-2,1));
        box.addView(dllButtons);

        note(box,"Needs Lossless.dll from your own Lossless Scaling on Steam: sign in to download just that file, "
            +"or choose a copy. Shows 2-4 frames per game frame and raises the screen's refresh rate to the FPS limit times "
            +"that (every frame is shown for one refresh, as with VSync). Replaces FSR 3.1 frame generation. "
            +"Performance mode is the lighter model. The engine is Bloodframe, based on lsfg-vk 1.0 by PancakeTAS (MIT).",0.7f);
        note(box,"Changes apply at once while you play. At the 30 FPS limit, the smoother frame timing for Lossless "
            +"Scaling applies after the next start of the game; until then the game keeps its own timing.",0.7f);

        if (fsr) {
            multiplier.setEnabled(false);
            for (int i=0;i<multiplier.getChildCount();++i) multiplier.getChildAt(i).setEnabled(false);
            flow.setEnabled(false); performance.setEnabled(false); half.setEnabled(false);
        } else {
            final Runnable enable=() -> {
                boolean on=checked(multiplier)>0;
                flow.setEnabled(on); performance.setEnabled(on); half.setEnabled(on);
            };
            multiplier.setOnCheckedChangeListener((g,id) -> enable.run());
            enable.run();
        }

        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        AlertDialog.Builder dialog=new AlertDialog.Builder(activity).setTitle("Bloodframe").setView(scroll);
        if (fsr) {
            dialog.setPositiveButton("Close",null);
        } else {
            dialog.setNegativeButton("Cancel",null).setPositiveButton("Apply",(d,w) -> {
                int m=Integer.parseInt(VALUES[Math.max(0,checked(multiplier))]); // 0 (off) or 2-4
                String flowScale=String.format(Locale.ROOT,"%.2f",0.25f+flow.getProgress()*0.05f);
                boolean fast=performance.isChecked();
                boolean fp16=half.isChecked();
                Map<String,String> changed=new LinkedHashMap<>();
                changed.put("lsfg_multiplier",String.valueOf(m));
                changed.put("lsfg_flow_scale",flowScale);
                changed.put("lsfg_performance",fast ? "1" : "0");
                changed.put("lsfg_fp16",fp16 ? "1" : "0");
                // The running game follows lsfg.state (libbbgpu reads it): no restart.
                boolean ok=settings.setAll(changed) && writeState(state,m,flowScale,fast,fp16);
                Toast.makeText(activity,!ok ? "Frame generation could not be saved"
                    : m>0 && lossless.dll()==null ? "Lossless Scaling stays off until Lossless.dll is found"
                    : m>0 ? "Lossless Scaling "+m+"× applied" : "Frame generation off",Toast.LENGTH_SHORT).show();
                if (ok && applied!=null) applied.run();
            });
        }
        dialog.show();
    }

    private static int checked(RadioGroup g) {
        for (int i=0;i<g.getChildCount();++i) if (((RadioButton)g.getChildAt(i)).isChecked()) return i;
        return -1;
    }

    /** "multiplier=2\nflow=0.75\nperformance=1\nfp16=1\n" (multiplier 0: off), written whole and synced, then renamed so
     *  the game never reads half of it. */
    private static boolean writeState(File state,int multiplier,String flow,boolean performance,boolean fp16) {
        try {
            SafeWrite.write(state,"multiplier="+multiplier+"\nflow="+flow+"\nperformance="+(performance ? 1 : 0)
                +"\nfp16="+(fp16 ? 1 : 0)+"\n");
        } catch (Exception ex) { android.util.Log.e("Bloodborne","lsfg state: "+ex); return false; }
        return true;
    }
}
