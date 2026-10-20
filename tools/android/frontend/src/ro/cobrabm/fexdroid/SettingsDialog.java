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

/** Graphics settings for the Thor, kept in bbport.ini (run-bionic.sh reads them at start: scene
 *  size, upscaler, the game's effect patches). Other lines and comments of the file are kept. */
final class SettingsDialog {
    /** The output sizes depend on the aspect ratio (aspect_ratio) and the display; see outputs(). Legacy
     *  output_res values (1920x1080, 1280x720, 854x480) load as they are. 1080p (1080 lines at the chosen aspect ratio) is the default. */
    private static final String[] ASPECT_VALUES={"auto","4:3","16:10","16:9","21:9","32:9"};
    private static final double[] ASPECT_RATIOS={0,4/3.0,16/10.0,16/9.0,21/9.0,32/9.0};
    private static final int ASPECT_DEFAULT=0, OUTPUT_DEFAULT_HEIGHT=1080;
    private static final int[] OUTPUT_HEIGHTS={360,480,540,720,1080,1440};
    private static final String[] PRESET_NAMES={"Native","Quality","Balanced","Performance","Ultra performance","Potato"};
    private static final double[] PRESET_SCALES={1.0,1.5,1.7,2.0,3.0,4.0};
    private static int scene(int v,int preset) { return Math.max(2,(int)Math.rint(v/PRESET_SCALES[preset]/2)*2); }
    private static String presetName(int[] output,int preset) {
        return PRESET_NAMES[preset]+" ("+scene(output[0],preset)+"×"+scene(output[1],preset)+")";
    }
    /** The device's physical display in landscape, long side first (the game surface covers all of it,
     *  cutout included: MainActivity lays the window out in the short-edge cutout area, immersive). */
    static int[] displaySize(Activity activity) {
        int w, h;
        android.view.WindowManager wm=activity.getWindowManager();
        if (android.os.Build.VERSION.SDK_INT>=30) {
            android.graphics.Rect r=wm.getMaximumWindowMetrics().getBounds(); w=r.width(); h=r.height();
        } else {
            android.graphics.Point p=new android.graphics.Point(); wm.getDefaultDisplay().getRealSize(p); w=p.x; h=p.y;
        }
        return new int[]{Math.max(w,h),Math.min(w,h)};
    }
    /** The aspect (width/height) of an aspect_ratio value on a display; missing: auto; unknown values: 16:9. */
    static double aspectOf(String value,int[] display) {
        if (value==null || value.isEmpty()) value="auto";
        for (int i=0;i<ASPECT_VALUES.length;++i) if (ASPECT_VALUES[i].equals(value))
            return i==0 ? display[0]/(double)display[1] : ASPECT_RATIOS[i];
        return 16/9.0;
    }
    /** "20:9" for a display's own ratio: the nearest simple ratio, else "2.13:1". */
    private static String ratioName(int[] d) {
        double r=d[0]/(double)d[1];
        String[] names={"4:3","16:10","16:9","19.5:9","20:9","21:9","32:9"};
        double[] values={4/3.0,1.6,16/9.0,19.5/9,20/9.0,21/9.0,32/9.0};
        for (int i=0;i<names.length;++i) if (Math.abs(r/values[i]-1)<0.012) return names[i];
        return String.format(Locale.ROOT,"%.2f:1",r);
    }
    /** The output sizes {w,h} for an aspect: index 0 is the display native one; then the heights no larger than
     *  the display in either dimension (and the legacy 1080p at 16:9), widths = height x aspect, even. */
    static List<int[]> outputs(double aspect,int[] display) {
        List<int[]> list=new ArrayList<>();
        list.add(new int[]{display[0]&~1,display[1]&~1});
        List<Integer> heights=new ArrayList<>();
        for (int h:OUTPUT_HEIGHTS) heights.add(h);
        if (!heights.contains(display[1]&~1)) heights.add(display[1]&~1);
        java.util.Collections.sort(heights);
        for (int h:heights) {
            int w=(int)Math.round(h*aspect/2)*2;
            boolean legacy=h==1080 && w==1920;
            if (!legacy && (w>display[0] || h>display[1])) continue;
            boolean dup=false;
            for (int[] o:list) if (o[0]==w && o[1]==h) dup=true;
            if (!dup) list.add(new int[]{w,h});
        }
        return list;
    }
    private static final String[][] EFFECTS={
        {"effect_chromatic_aberration","Chromatic aberration","1"},
        {"effect_dof","Depth of field","1"},
        {"effect_motion_blur","Motion blur","1"},
        {"effect_ssao","Ambient occlusion (SSAO)","1"},
        {"effect_game_aa","Game's own anti-aliasing","1"},
        {"effect_dynamic_shadows","Shadows from dynamic lights","1"},
        {"effect_ssr","Screen-space reflections (not in the original)","0"},
    };
    // FSR 4 (INT8) is not offered: ~470 ms a frame and a black scene on the Adreno 740 (run-bionic.sh).
    /** Game settings: the game patches run-bionic.sh applies at start (patch-parts), as upstream's
     *  System menu pages and launcher offer them: {key, title, note, default}. */
    private static final String[][] GAME_PATCHES={
        {"skip_network_choice","Skip the online/offline screen","Start straight at the main menu, offline. Leave off to choose online play at the title screen.","0"},
        {"skip_intro","Skip startup intros","The logos and intro movie at start","0"},
        {"debug_camera","Free camera","Toggle it with Cross + L3","0"},
        {"debug_menu","Debug menu","The game's own debug menu; needs its font files","0"},
    };
    /** Cheats: code hooks of the game applied at start (build_patch_parts.py, run-bionic.sh). */
    private static final String[][] CHEATS={
        {"cheat_health","Infinite health","Your HP refills to full every frame","0"},
        {"cheat_items","Infinite blood vials and items","Using a blood vial, bullet or other item leaves its count as it was "
            +"(items moved to storage or given away stay too)","0"},
    };
    private static final String[] UPSCALER_VALUES={"off","fsr3","gsr2"};
    private static final String[] UPSCALER_NAMES={"Off","FSR 3.1",
        "Snapdragon GSR 2\nQualcomm's upscaler made for Adreno; lighter than FSR 3.1"};
    private static final int FSR3=1;
    private static final String[] FPS_VALUES={"30","40","45","60","0"};
    private static final String[] FPS_NAMES={"30","40","45","60","Unlimited"};
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
        try { // a file of NUL bytes (a hard reset before it was synced) reads as an empty one
            for (String line:SafeWrite.readLines(file)) {
                lines.add(line);
                int eq=line.indexOf('=');
                if (eq>0 && !line.startsWith("#")) values.put(line.substring(0,eq).trim(),line.substring(eq+1).trim());
            }
        } catch (Exception ex) { android.util.Log.e("Bloodborne","settings: "+ex); }
    }
    /** Changed keys replace their lines; new keys are appended. Written whole and synced, then renamed. */
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
        try { SafeWrite.writeLines(file,out); }
        catch (Exception ex) { android.util.Log.e("Bloodborne","settings: "+ex); return false; }
        return true;
    }
    /** One setting, written at once (the GPU driver menu). Before the first start the file is
     *  the defaults (bbport.default.ini, as run-bionic.sh would copy it) plus this line. */
    boolean set(String key,String value) {
        Map<String,String> changed=new LinkedHashMap<>(); changed.put(key,value); return setAll(changed);
    }
    /** Several settings, written together (the Frame generation menu). */
    boolean setAll(Map<String,String> changed) {
        File defaults=new File(file.getParentFile(),"bbport.default.ini");
        SafeWrite.repair(file); // all NUL bytes: missing, so the defaults below
        if (!file.isFile() && defaults.isFile())
            try { SafeWrite.write(file,java.nio.file.Files.readAllBytes(defaults.toPath())); }
            catch (Exception ex) { android.util.Log.e("Bloodborne","settings: "+ex); }
        load(); return save(changed);
    }
    /** The settings as they are in the file now. */
    Map<String,String> read() { load(); return new LinkedHashMap<>(values); }

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
    private static int[] parseSize(String v) {
        java.util.regex.Matcher m=java.util.regex.Pattern.compile("(\\d+)x(\\d+)").matcher(v==null ? "" : v.trim());
        return m.matches() ? new int[]{Integer.parseInt(m.group(1)),Integer.parseInt(m.group(2))} : null;
    }
    private long totalMemory() {
        try {
            android.app.ActivityManager.MemoryInfo info=new android.app.ActivityManager.MemoryInfo();
            ((android.app.ActivityManager)activity.getSystemService(Activity.ACTIVITY_SERVICE)).getMemoryInfo(info);
            return info.totalMem;
        } catch (Exception ex) { return 0; }
    }
    private static int checkedIndex(RadioGroup g) {
        for (int i=0;i<g.getChildCount();++i) if (((RadioButton)g.getChildAt(i)).isChecked()) return i;
        return -1;
    }

    void show() {
        load();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));

        final int[] display=displaySize(activity);
        heading(box,"Aspect ratio");
        String[] aspectNames={"Auto (display, "+ratioName(display)+")","4:3","16:10","16:9","21:9","32:9"};
        final RadioGroup aspects=choices(box,aspectNames,indexOf(ASPECT_VALUES,get("aspect_ratio",""),ASPECT_DEFAULT));
        TextView aspectNote=new TextView(activity); aspectNote.setTextSize(13); aspectNote.setAlpha(0.7f);
        aspectNote.setText("16:9 is the game's own. Wider ratios show more of the scene at the sides; Auto fills the whole screen "
            +"(the ratio of your display). Applies at the next start.");
        box.addView(aspectNote);
        heading(box,"Output resolution");
        final RadioGroup outputs=new RadioGroup(activity);
        box.addView(outputs);
        final List<int[]> sizes=new ArrayList<>();
        final String[] wanted={get("output_res","1920x1080")}; // the saved value, then whatever is picked
        final TextView outputNote=new TextView(activity); outputNote.setTextSize(13); outputNote.setAlpha(0.7f);
        final long totalMem=totalMemory();
        final boolean lowMemory=totalMem>0 && totalMem<10L*1024*1024*1024; // a "12 GB" phone reports about 10.5-11 GiB
        final Runnable fillOutputs=() -> {
            int[] prev=sizes.isEmpty() || checkedIndex(outputs)<0 ? null : sizes.get(checkedIndex(outputs));
            boolean wasNative=prev!=null && checkedIndex(outputs)==0;
            int keepHeight=prev!=null ? prev[1] : -1;
            if (prev!=null) wanted[0]=wasNative ? "native" : prev[0]+"x"+prev[1];
            List<int[]> list=outputs(aspectOf(ASPECT_VALUES[Math.max(0,checkedIndex(aspects))],display),display);
            sizes.clear(); sizes.addAll(list);
            outputs.removeAllViews();
            int pick=-1;
            if ("native".equals(wanted[0]) && prev==null) pick=0;
            else if (prev==null || !wasNative) {
                // Saved value (or last pick) still offered: keep it; else the same height, nearest.
                int[] w=parseSize(wanted[0]);
                if (w!=null) { keepHeight=w[1]; for (int i=1;i<list.size();++i) if (list.get(i)[0]==w[0] && list.get(i)[1]==w[1]) pick=i; }
                if (pick<0 && keepHeight>0) { int best=Integer.MAX_VALUE; for (int i=1;i<list.size();++i) { int dd=Math.abs(list.get(i)[1]-keepHeight); if (dd<best) { best=dd; pick=i; } } }
            } else pick=0;
            if (pick<0) { for (int i=1;i<list.size();++i) if (list.get(i)[1]==OUTPUT_DEFAULT_HEIGHT) pick=i; }
            if (pick<0) pick=0;
            for (int i=0;i<list.size();++i) {
                int[] o=list.get(i);
                RadioButton b=new RadioButton(activity); b.setId(View.generateViewId()); b.setTextSize(15);
                b.setText(i==0 ? "Display native ("+o[0]+"×"+o[1]+")" : o[1]+"p ("+o[0]+"×"+o[1]+")");
                outputs.addView(b); if (i==pick) outputs.check(b.getId());
            }
        };
        aspects.setOnCheckedChangeListener((g,id) -> fillOutputs.run());
        fillOutputs.run();
        final Runnable outputHint=() -> {
            int[] o=sizes.get(Math.max(0,checkedIndex(outputs)));
            String text="Higher output looks sharper but costs more GPU time; the upscaler renders the scene smaller and fills it. "
                +"Outputs above 1080p use more memory. Applies at the next start.";
            if ((o[1]>1080 || o[0]>1920) && lowMemory)
                text+="\n\nThis device has under 12 GB of RAM: this output may run out of memory.";
            outputNote.setText(text);
        };
        box.addView(outputNote);
        heading(box,"Upscaling");
        final RadioGroup upscalers=choices(box,UPSCALER_NAMES,indexOf(UPSCALER_VALUES,get("upscaler","gsr2"),1));
        heading(box,"Render resolution");
        int preset=0;
        try { preset=Math.max(0,Math.min(PRESET_NAMES.length-1,Integer.parseInt(get("preset","3")))); } catch (NumberFormatException ignored) { preset=3; }
        final RadioGroup presets=choices(box,PRESET_NAMES,preset);
        // FSR 3.1 and Lossless Scaling frame generation: one at a time. Lossless Scaling is set in the
        // Frame generation menu and applies at once; while it is on this switch is off and locked.
        final boolean lossless=get("lsfg_multiplier","0").matches("[234]");
        final Switch frameGen=toggle(box,"FSR 3.1 frame generation",!lossless && "1".equals(get("frame_generation","0")));
        TextView frameGenNote=new TextView(activity); frameGenNote.setTextSize(13); frameGenNote.setAlpha(0.7f);
        frameGenNote.setText("Shows a generated frame between each two (about twice the frames on screen); "
            +"adds about half a frame of input delay. Best with a steady frame rate."
            +(lossless ? "\n\nLossless Scaling frame generation is on (Frame generation in the menu): turn it off there to use this."
                : ""));
        box.addView(frameGenNote);
        final TextView presetNote=new TextView(activity); presetNote.setTextSize(13); presetNote.setAlpha(0.7f);
        box.addView(presetNote);
        RadioGroup.OnCheckedChangeListener upscaling=(g,id) -> {
            int upscaler=checkedIndex(upscalers);
            int[] output=sizes.get(Math.max(0,checkedIndex(outputs)));
            boolean on=upscaler!=0;
            outputHint.run();
            ((RadioButton)upscalers.getChildAt(0)).setText("Off (render at "+output[0]+"×"+output[1]+")");
            for (int i=0;i<presets.getChildCount();++i) ((RadioButton)presets.getChildAt(i)).setText(presetName(output,i));
            for (int i=0;i<presets.getChildCount();++i) presets.getChildAt(i).setEnabled(on);
            frameGen.setEnabled(upscaler==FSR3 && !lossless);
            presetNote.setText(on ? "Lower is faster; the upscaler fills the screen." : "Without upscaling the game renders at the output resolution.");
        };
        upscalers.setOnCheckedChangeListener(upscaling); outputs.setOnCheckedChangeListener(upscaling);
        aspects.setOnCheckedChangeListener((g,id) -> { fillOutputs.run(); upscaling.onCheckedChanged(upscalers,0); });
        upscaling.onCheckedChanged(upscalers,0);

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
        fpsNote.setText("Hooks in the game's code give it each frame's real time, so it keeps its speed at any limit.");
        box.addView(fpsNote);
        final Switch speedFloor=toggle(box,"Keep game speed when FPS drops (down to 20 FPS)",!"30".equals(get("speed_floor","20")));
        TextView speedNote=new TextView(activity); speedNote.setTextSize(13); speedNote.setAlpha(0.7f);
        speedNote.setText("Below 30 FPS the game normally runs in slow motion; with this on, it keeps real time down to 20 FPS. "
            +"Turn off if physics or animations misbehave.");
        box.addView(speedNote);
        heading(box,"VSync");
        final Switch vsync=toggle(box,"VSync","1".equals(get("vsync","0")));
        TextView vsyncNote=new TextView(activity); vsyncNote.setTextSize(13); vsyncNote.setAlpha(0.7f);
        vsyncNote.setText("Every frame is shown for at least one screen refresh: smoother pacing, a little more latency. "
            +"Off: the newest frame is shown, frames finished within the same refresh are skipped.");
        box.addView(vsyncNote);

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
                int outIndex=Math.max(0,checkedIndex(outputs)); int[] out=sizes.get(outIndex);
                changed.put("aspect_ratio",ASPECT_VALUES[Math.max(0,checkedIndex(aspects))]);
                changed.put("output_res",outIndex==0 ? "native" : out[0]+"x"+out[1]);
                changed.put("upscaler",UPSCALER_VALUES[Math.max(0,checkedIndex(upscalers))]);
                changed.put("fps_limit",FPS_VALUES[Math.max(0,checkedIndex(fpsLimit))]);
                changed.put("frame_generation",frameGen.isChecked() && checkedIndex(upscalers)==FSR3 && !lossless ? "1" : "0");
                changed.put("speed_floor",speedFloor.isChecked() ? "20" : "30");
                changed.put("vsync",vsync.isChecked() ? "1" : "0");
                changed.put("thor_cpus",cpuList(cores));
                changed.put("preset",String.valueOf(Math.max(0,checkedIndex(presets))));
                changed.put("sharpen",sharpen.isChecked() ? "1" : "0");
                changed.put("sharpness",String.format(Locale.ROOT,"%.2f",sharpness.getProgress()/100f));
                changed.put("show_fps",fps.isChecked() ? "1" : "0");
                for (int i=0;i<EFFECTS.length;++i) changed.put(EFFECTS[i][0],effects[i].isChecked() ? "1" : "0");
                changed.put("model_lod",LOD_VALUES[Math.max(0,checkedIndex(lods))]);
                apply(changed);
            }).show();
    }

    /** Saves the changed keys and restarts the game when any differs from the file. */
    private void apply(Map<String,String> changed) {
        boolean same=true;
        // A key an older bbport.ini lacks counts as its default (speed_floor: 20, as run-bionic.sh reads it).
        for (Map.Entry<String,String> e:changed.entrySet())
            if (!e.getValue().equals(values.containsKey(e.getKey()) || !e.getKey().equals("speed_floor") ? values.get(e.getKey()) : "20")) same=false;
        if (same) return;
        if (save(changed)) restart.run();
        else new AlertDialog.Builder(activity).setMessage("The settings could not be saved.").setPositiveButton("OK",null).show();
    }

    /** Game settings: the game's patches (applied when the game starts: Apply restarts it). */
    void showGame(boolean online) {
        load();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));
        final Switch[] toggles=new Switch[GAME_PATCHES.length];
        for (int i=0;i<GAME_PATCHES.length;++i) {
            if (online && "skip_network_choice".equals(GAME_PATCHES[i][0])) {
                // Online play needs the title screen's online choice: the patch never applies (run-bionic.sh); the saved
                // choice stays in bbport.ini and applies again once online play is off.
                TextView title=new TextView(activity); title.setTextSize(16); title.setText(GAME_PATCHES[i][1]);
                box.addView(title);
                TextView off=new TextView(activity); off.setTextSize(16); off.setText("Off while playing online");
                off.setTextColor(0xffc6ad76); off.setPadding(0,dp(2),0,dp(6)); box.addView(off);
                continue;
            }
            toggles[i]=toggle(box,GAME_PATCHES[i][1],"1".equals(get(GAME_PATCHES[i][0],GAME_PATCHES[i][3])));
            TextView note=new TextView(activity); note.setTextSize(12); note.setAlpha(0.65f);
            note.setText(GAME_PATCHES[i][2]); note.setPadding(0,0,0,dp(6));
            box.addView(note);
        }
        TextView restartNote=new TextView(activity); restartNote.setTextSize(13); restartNote.setAlpha(0.7f);
        restartNote.setText("These are patches to the game: Apply restarts it (unsaved progress is lost).");
        restartNote.setPadding(0,dp(8),0,0);
        box.addView(restartNote);
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        new AlertDialog.Builder(activity).setTitle("Game settings").setView(scroll)
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Apply",(d,w) -> {
                Map<String,String> changed=new LinkedHashMap<>();
                for (int i=0;i<GAME_PATCHES.length;++i)
                    if (toggles[i]!=null) changed.put(GAME_PATCHES[i][0],toggles[i].isChecked() ? "1" : "0");
                apply(changed);
            }).show();
    }

    /** Cheats (applied when the game starts: Apply restarts it). */
    void showCheats(boolean online) {
        load();
        LinearLayout box=new LinearLayout(activity); box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24),dp(4),dp(24),dp(8));
        if (online) {
            // Online play: the game starts with the cheats off (run-bionic.sh) and they cannot be turned on; the saved choice
            // stays in bbport.ini and applies again once online play is off.
            TextView off=new TextView(activity); off.setTextSize(16); off.setText("Off while playing online");
            off.setTextColor(0xffc6ad76); off.setPadding(0,dp(8),0,dp(8)); box.addView(off);
            TextView why=new TextView(activity); why.setTextSize(13); why.setAlpha(0.75f);
            why.setText("Cheats would change the game for the other hunters too, so they are switched off while Online is on. "
                +"Your choice is kept and comes back when you turn Online off (Menu \u2192 Online).");
            box.addView(why);
            new AlertDialog.Builder(activity).setTitle("Cheats").setView(box).setPositiveButton("Close",null).show();
            return;
        }
        final Switch[] toggles=new Switch[CHEATS.length];
        for (int i=0;i<CHEATS.length;++i) {
            toggles[i]=toggle(box,CHEATS[i][1],"1".equals(get(CHEATS[i][0],CHEATS[i][3])));
            TextView note=new TextView(activity); note.setTextSize(12); note.setAlpha(0.65f);
            note.setText(CHEATS[i][2]); note.setPadding(0,0,0,dp(6));
            box.addView(note);
        }
        TextView restartNote=new TextView(activity); restartNote.setTextSize(13); restartNote.setAlpha(0.7f);
        restartNote.setText("For Bloodborne 1.09 offline play. They switch at once, while you play.");
        restartNote.setPadding(0,dp(8),0,0);
        box.addView(restartNote);
        ScrollView scroll=new ScrollView(activity); scroll.addView(box);
        new AlertDialog.Builder(activity).setTitle("Cheats").setView(scroll)
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Apply",(d,w) -> {
                Map<String,String> changed=new LinkedHashMap<>();
                for (int i=0;i<CHEATS.length;++i) changed.put(CHEATS[i][0],toggles[i].isChecked() ? "1" : "0");
                // The running game follows cheats.state (probe.c reads it): no restart.
                boolean ok=save(changed) && writeCheatState(new File(file.getParentFile(),"cheats.state"),
                    toggles[0].isChecked(),toggles[1].isChecked());
                android.widget.Toast.makeText(activity,ok ? "Cheats applied" : "The cheats could not be saved",
                    android.widget.Toast.LENGTH_SHORT).show();
            }).show();
    }

    /** "10": infinite health on, items off (run-bionic.sh's BB_CHEATS_FILE), written whole and synced, then renamed. */
    private static boolean writeCheatState(File state,boolean health,boolean items) {
        try { SafeWrite.write(state,(health ? "1" : "0")+(items ? "1" : "0")+"\n"); }
        catch (Exception ex) { android.util.Log.e("Bloodborne","cheats: "+ex); return false; }
        return true;
    }
}
