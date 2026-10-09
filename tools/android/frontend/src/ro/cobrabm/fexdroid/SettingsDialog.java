package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Typeface;
import android.view.View;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.BufferedReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Graphics settings for the Thor, kept in bbport.ini (run-thor.sh reads them at start: scene
 *  size, upscaler, the game's effect patches). Other lines and comments of the file are kept. */
final class SettingsDialog {
    /** Scene sizes of the presets at the Thor's 1280x720 output (run-thor.sh). */
    private static final String[] PRESETS={"Native (1280×720)","Quality (854×480)","Balanced (752×424)",
        "Performance (640×360)","Ultra performance (426×240)"};
    private static final String[][] EFFECTS={
        {"effect_chromatic_aberration","Chromatic aberration","1"},
        {"effect_dof","Depth of field","1"},
        {"effect_motion_blur","Motion blur","1"},
        {"effect_ssao","Ambient occlusion (SSAO)","1"},
        {"effect_game_aa","Game's own anti-aliasing","1"},
        {"effect_dynamic_shadows","Shadows from dynamic lights","1"},
        {"effect_ssr","Screen-space reflections (not in the original)","0"},
        {"skip_intro","Skip startup intros","0"},
    };
    // FSR 4 is off on the Thor for now (run-thor.sh treats a stored fsr4 as FSR 3.1).
    private static final String[] UPSCALER_VALUES={"off","fsr3"};
    private static final String[] UPSCALER_NAMES={"Off (render at 1280×720)","FSR 3.1"};
    private static final String[] FPS_VALUES={"30","40","45","60","0"};
    private static final String[] FPS_NAMES={"30 (the game's own)","40","45","60","Unlimited"};
    /** Snapdragon 8 Gen 2: three Cortex-A510, four Cortex-A715, one Cortex-X3. */
    private static final String[] CORE_NAMES={"Core 0 · efficiency A510 2.0 GHz","Core 1 · efficiency A510 2.0 GHz",
        "Core 2 · efficiency A510 2.0 GHz","Core 3 · performance A715 2.8 GHz","Core 4 · performance A715 2.8 GHz",
        "Core 5 · performance A715 2.8 GHz","Core 6 · performance A715 2.8 GHz","Core 7 · prime X3 3.2 GHz"};
    private static final String[] LOD_VALUES={"-2","0","1","2"};
    private static final String[] LOD_NAMES={"Highest","Game default","Lower","Lowest"};

    private final Activity activity;
    private final File file;
    private final Runnable restart;
    private final Map<String,String> values=new LinkedHashMap<>();
    private final List<String> lines=new ArrayList<>();

    SettingsDialog(Activity activity,File file,Runnable restart) {
        this.activity=activity; this.file=file; this.restart=restart;
    }

    private int dp(float n) { return Math.round(n*activity.getResources().getDisplayMetrics().density); }
    private String get(String key,String fallback) { String v=values.get(key); return v==null || v.isEmpty() ? fallback : v; }

    private void load() {
        values.clear(); lines.clear();
        if (!file.isFile()) return;
        try (BufferedReader r=new BufferedReader(new InputStreamReader(new FileInputStream(file),"UTF-8"))) {
            for (String line; (line=r.readLine())!=null;) {
                lines.add(line);
                int eq=line.indexOf('=');
                if (eq>0 && !line.startsWith("#")) values.put(line.substring(0,eq).trim(),line.substring(eq+1).trim());
            }
        } catch (Exception ex) { android.util.Log.e("Bloodborne","settings: "+ex); }
    }
    /** Changed keys replace their lines; new keys are appended. Written whole, then renamed. */
    private boolean save(Map<String,String> changed) {
        List<String> out=new ArrayList<>();
        Map<String,String> pending=new LinkedHashMap<>(changed);
        for (String line:lines) {
            int eq=line.indexOf('=');
            String key=eq>0 && !line.startsWith("#") ? line.substring(0,eq).trim() : null;
            if (key!=null && pending.containsKey(key)) out.add(key+"="+pending.remove(key));
            else out.add(line);
        }
        for (Map.Entry<String,String> e:pending.entrySet()) out.add(e.getKey()+"="+e.getValue());
        File temporary=new File(file.getPath()+".tmp");
        try (Writer w=new OutputStreamWriter(new FileOutputStream(temporary),"UTF-8")) {
            for (String line:out) w.write(line+"\n");
        } catch (Exception ex) { android.util.Log.e("Bloodborne","settings: "+ex); return false; }
        return temporary.renameTo(file);
    }

    private TextView heading(LinearLayout box,String title) {
        TextView t=new TextView(activity); t.setText(title); t.setTextSize(15);
        t.setTypeface(Typeface.DEFAULT,Typeface.BOLD); t.setPadding(0,dp(14),0,dp(4));
        box.addView(t); return t;
    }
    private Switch toggle(LinearLayout box,String title,boolean on) {
        Switch s=new Switch(activity); s.setText(title); s.setChecked(on); s.setTextSize(15);
        s.setPadding(0,dp(6),0,dp(6)); box.addView(s); return s;
    }
    private RadioGroup choices(LinearLayout box,String[] names,int selected) {
        RadioGroup g=new RadioGroup(activity);
        for (int i=0;i<names.length;++i) {
            RadioButton b=new RadioButton(activity); b.setId(View.generateViewId()); b.setText(names[i]); b.setTextSize(15);
            g.addView(b); if (i==selected) g.check(b.getId());
        }
        box.addView(g); return g;
    }
    private static int indexOf(String[] values,String value,int fallback) {
        for (int i=0;i<values.length;++i) if (values[i].equals(value)) return i;
        return fallback;
    }
    /** A taskset list ("0-2,5", "3-7"); empty: every core. */
    private static boolean[] parseCpus(String list,int count) {
        boolean[] on=new boolean[count];
        if (list.trim().isEmpty()) { java.util.Arrays.fill(on,true); return on; }
        for (String part:list.split(",")) {
            try {
                String[] range=part.trim().split("-");
                int from=Integer.parseInt(range[0].trim()), to=range.length>1 ? Integer.parseInt(range[1].trim()) : from;
                for (int i=Math.max(0,from);i<=Math.min(count-1,to);++i) on[i]=true;
            } catch (NumberFormatException ignored) {}
        }
        boolean any=false; for (boolean b:on) any|=b;
        if (!any) java.util.Arrays.fill(on,true);
        return on;
    }
    private static String cpuList(Switch[] cores) {
        StringBuilder list=new StringBuilder(); boolean all=true;
        for (int i=0;i<cores.length;++i) {
            if (!cores[i].isChecked()) { all=false; continue; }
            if (list.length()>0) list.append(',');
            list.append(i);
        }
        return all ? "" : list.toString();
    }
    private static int checkedIndex(RadioGroup g) {
        for (int i=0;i<g.getChildCount();++i) if (((RadioButton)g.getChildAt(i)).isChecked()) return i;
        return -1;
    }

    void show() {
        load();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));

        heading(box,"Upscaling (to 1280×720)");
        final RadioGroup upscalers=choices(box,UPSCALER_NAMES,indexOf(UPSCALER_VALUES,get("upscaler","fsr3"),1));
        heading(box,"Render resolution");
        int preset=0;
        try { preset=Math.max(0,Math.min(4,Integer.parseInt(get("preset","3")))); } catch (NumberFormatException ignored) { preset=3; }
        final RadioGroup presets=choices(box,PRESETS,preset);
        final Switch frameGen=toggle(box,"FSR 3.1 frame generation","1".equals(get("frame_generation","0")));
        TextView frameGenNote=new TextView(activity); frameGenNote.setTextSize(13); frameGenNote.setAlpha(0.7f);
        frameGenNote.setText("Shows a generated frame between each two (about twice the frames on screen); "
            +"adds about half a frame of input delay. Best with a steady frame rate.");
        box.addView(frameGenNote);
        final TextView presetNote=new TextView(activity); presetNote.setTextSize(13); presetNote.setAlpha(0.7f);
        box.addView(presetNote);
        RadioGroup.OnCheckedChangeListener upscaling=(g,id) -> {
            boolean on=checkedIndex(upscalers)!=0;
            for (int i=0;i<presets.getChildCount();++i) presets.getChildAt(i).setEnabled(on);
            frameGen.setEnabled(on);
            presetNote.setText(on ? "Lower is faster; the upscaler fills the screen." : "Without upscaling the game renders at 1280×720.");
        };
        upscalers.setOnCheckedChangeListener(upscaling); upscaling.onCheckedChanged(upscalers,0);

        heading(box,"Image");
        final Switch sharpen=toggle(box,"Sharpening","1".equals(get("sharpen","0")));
        final TextView sharpLabel=new TextView(activity); sharpLabel.setTextSize(14); box.addView(sharpLabel);
        final SeekBar sharpness=new SeekBar(activity); sharpness.setMax(100);
        float initial=0.5f;
        try { initial=Float.parseFloat(get("sharpness","0.5")); } catch (NumberFormatException ignored) {}
        sharpness.setProgress(Math.round(Math.max(0,Math.min(1,initial))*100));
        SeekBar.OnSeekBarChangeListener sharpText=new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s,int p,boolean user) { sharpLabel.setText("Sharpness: "+p+"%"); }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        };
        sharpness.setOnSeekBarChangeListener(sharpText); sharpText.onProgressChanged(sharpness,sharpness.getProgress(),false);
        box.addView(sharpness);
        sharpen.setOnCheckedChangeListener((b,on) -> sharpness.setEnabled(on)); sharpness.setEnabled(sharpen.isChecked());
        final Switch fps=toggle(box,"Show FPS","1".equals(get("show_fps","0")));

        heading(box,"FPS limit");
        final RadioGroup fpsLimit=choices(box,FPS_NAMES,indexOf(FPS_VALUES,get("fps_limit","30"),0));
        TextView fpsNote=new TextView(activity); fpsNote.setTextSize(13); fpsNote.setAlpha(0.7f);
        fpsNote.setText("Above 30 uses the community frame-rate patch (game timing follows the real frame time).");
        box.addView(fpsNote);

        heading(box,"CPU cores the game may use");
        final Switch[] cores=new Switch[CORE_NAMES.length];
        boolean[] allowed=parseCpus(get("thor_cpus",""),CORE_NAMES.length);
        for (int i=0;i<cores.length;++i) cores[i]=toggle(box,CORE_NAMES[i],allowed[i]);
        CompoundButton.OnCheckedChangeListener keepOne=(b,on) -> {
            if (on) return;
            for (Switch c:cores) if (c.isChecked()) return;
            b.setChecked(true); // at least one core
        };
        for (Switch c:cores) c.setOnCheckedChangeListener(keepOne);

        heading(box,"Game effects");
        final Switch[] effects=new Switch[EFFECTS.length];
        for (int i=0;i<EFFECTS.length;++i) effects[i]=toggle(box,EFFECTS[i][1],"1".equals(get(EFFECTS[i][0],EFFECTS[i][2])));

        heading(box,"Model detail");
        int lod=1;
        for (int i=0;i<LOD_VALUES.length;++i) if (LOD_VALUES[i].equals(get("model_lod","0"))) lod=i;
        final RadioGroup lods=choices(box,LOD_NAMES,lod);

        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        new AlertDialog.Builder(activity).setTitle("Graphics settings").setView(scroll)
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Apply & restart",(d,w) -> {
                Map<String,String> changed=new LinkedHashMap<>();
                changed.put("upscaler",UPSCALER_VALUES[Math.max(0,checkedIndex(upscalers))]);
                changed.put("fps_limit",FPS_VALUES[Math.max(0,checkedIndex(fpsLimit))]);
                changed.put("frame_generation",frameGen.isChecked() && checkedIndex(upscalers)!=0 ? "1" : "0");
                changed.put("thor_cpus",cpuList(cores));
                changed.put("preset",String.valueOf(Math.max(0,checkedIndex(presets))));
                changed.put("sharpen",sharpen.isChecked() ? "1" : "0");
                changed.put("sharpness",String.format(Locale.ROOT,"%.2f",sharpness.getProgress()/100f));
                changed.put("show_fps",fps.isChecked() ? "1" : "0");
                for (int i=0;i<EFFECTS.length;++i) changed.put(EFFECTS[i][0],effects[i].isChecked() ? "1" : "0");
                changed.put("model_lod",LOD_VALUES[Math.max(0,checkedIndex(lods))]);
                boolean same=true;
                for (Map.Entry<String,String> e:changed.entrySet()) if (!e.getValue().equals(values.get(e.getKey()))) same=false;
                if (same) return;
                if (save(changed)) restart.run();
                else new AlertDialog.Builder(activity).setMessage("The settings could not be saved.").setPositiveButton("OK",null).show();
            }).show();
    }
}
