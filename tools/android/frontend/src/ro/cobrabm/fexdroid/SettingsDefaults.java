package ro.cobrabm.fexdroid;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** The Android default settings (bbport.default.ini, from tools/android/thor.ini) applied to bbport.ini
 *  by revision: on a first install (no bbport.ini), and once when updating from before the revision
 *  (a bbport.ini and no settings_defaults marker). Later updates find the marker and leave the player's
 *  settings alone. The marker holds the revision number; a revision above the stored one applies the
 *  defaults once more (raise REVISION only for that). Runs after RuntimeInstaller extracted the runtime
 *  (that is what brings the defaults file) and before run-bionic.sh reads bbport.ini.
 *  When updating, every key of the defaults file takes its default value, except the player's own: the
 *  controller mapping (pad.*), the GPU driver, the mods menu position and language, and the mods switch.
 *  Keys that are not in the defaults file stay as they are, as do the comments. */
final class SettingsDefaults {
    static final int REVISION=1;
    private static final String MARKER="settings_defaults";
    private static final String[] KEPT={"gpu_driver","menu_pos","menu_language","mods_enabled"};

    private SettingsDefaults() {}

    private static boolean kept(String key) {
        if (key.startsWith("pad.")) return true;
        for (String k:KEPT) if (k.equals(key)) return true;
        return false;
    }
    /** The key of a "key=value" line (not a comment), else null; as SettingsDialog reads the file. */
    private static String keyOf(String line) {
        int eq=line.indexOf('=');
        return eq>0 && !line.startsWith("#") ? line.substring(0,eq).trim() : null;
    }
    /** The lines of the file; none when it is NUL bytes only (see SafeWrite). */
    private static List<String> lines(File file) throws IOException { return SafeWrite.readLines(file); }
    /** key=value pairs, the last of a key winning (as run-bionic.sh's setting()). */
    private static Map<String,String> values(List<String> lines) {
        Map<String,String> out=new LinkedHashMap<>();
        for (String line:lines) {
            String key=keyOf(line);
            if (key!=null) out.put(key,line.substring(line.indexOf('=')+1).trim());
        }
        return out;
    }
    /** Written whole beside the file and synced, then renamed over it. */
    private static void write(File file,List<String> lines) throws IOException { SafeWrite.writeLines(file,lines); }
    private static int revision(File marker) {
        try {
            List<String> lines=lines(marker);
            return lines.isEmpty() ? 0 : Integer.parseInt(lines.get(0).trim());
        } catch (Exception e) { return 0; }
    }

    /** 0.9.4: the write-fault fixes (Debug options) are on for everyone. Test builds before it could save them
     *  off (debug_fault_* = 0); once, those lines are removed so the defaults (on) apply. The marker holds 1. */
    private static final String FAULT_MARKER="debug_fault_defaults";
    static void resetFaultSwitches(File base) throws IOException {
        File marker=new File(base,FAULT_MARKER), ini=new File(base,"bbport.ini");
        if (revision(marker)>=1) return;
        if (ini.isFile()) {
            List<String> out=new ArrayList<>();
            int removed=0;
            for (String line:lines(ini)) {
                String key=keyOf(line);
                if (key!=null && key.startsWith("debug_fault_")) { ++removed; continue; }
                out.add(line);
            }
            if (removed>0) {
                write(ini,out);
                android.util.Log.i("Bloodborne","write-fault fixes back on by default ("+removed+" debug_fault_* settings removed)");
            }
        }
        List<String> mark=new ArrayList<>(); mark.add("1");
        write(marker,mark);
    }

    /** 0.11.0: Snapdragon GSR 2 is the default upscaler. Once, every bbport.ini from before it switches
     *  to upscaler=gsr2; FSR 3.1 frame generation (frame_generation=1) goes off with it, as it works with
     *  FSR 3.1 only. Bloodframe (lsfg_*) stays as it is. The marker holds 1. */
    private static final String GSR2_MARKER="upscaler_gsr2_default";
    static void switchToGsr2(File base) throws IOException {
        File marker=new File(base,GSR2_MARKER), ini=new File(base,"bbport.ini");
        if (revision(marker)>=1) return;
        if (ini.isFile()) {
            List<String> out=new ArrayList<>();
            boolean upscaler=false;
            for (String line:lines(ini)) {
                String key=keyOf(line);
                if ("upscaler".equals(key)) { if (!upscaler) out.add("upscaler=gsr2"); upscaler=true; continue; }
                if ("frame_generation".equals(key)) { out.add("frame_generation=0"); continue; }
                out.add(line);
            }
            if (!upscaler) out.add("upscaler=gsr2");
            write(ini,out);
            android.util.Log.i("Bloodborne","upscaler set to Snapdragon GSR 2 (0.11.0 default)");
        }
        List<String> mark=new ArrayList<>(); mark.add("1");
        write(marker,mark);
    }

    /** 1.2.0: online play exists, so "Skip the online/offline screen" (skip_network_choice) is off by default (the game shows
     *  its screen). Once, every bbport.ini from before it gets skip_network_choice=0, even where the player had it on; the
     *  player may turn it back on afterwards. The marker holds 1 and is written last (a crash before it repeats the step). */
    private static final String SKIP_MARKER="skip_network_choice_off";
    static void switchOffSkipNetworkChoice(File base) throws IOException {
        File marker=new File(base,SKIP_MARKER), ini=new File(base,"bbport.ini");
        if (revision(marker)>=1) return;
        if (ini.isFile()) {
            List<String> out=new ArrayList<>();
            boolean changed=false;
            for (String line:lines(ini)) {
                if ("skip_network_choice".equals(keyOf(line))) { // the last line of a key wins: every one is set
                    String fixed="skip_network_choice=0";
                    if (!fixed.equals(line.trim())) changed=true;
                    out.add(fixed);
                    continue;
                }
                out.add(line);
            }
            if (changed) {
                write(ini,out);
                android.util.Log.i("Bloodborne","online/offline screen shown by default (skip_network_choice set to 0, 1.2.0)");
            }
        }
        List<String> mark=new ArrayList<>(); mark.add("1");
        write(marker,mark);
    }

    /** Applies the defaults in base (the bbport directory) when this revision has not been yet. Does
     *  nothing, and writes no marker, while the defaults file is missing. Throws when the files cannot
     *  be written; the marker is last, so the next start tries again. */
    static void apply(File base) throws IOException {
        // A hard reset can leave files of NUL bytes (written, never synced): settings and markers of that kind
        // are taken as missing, so the defaults apply as on a first install.
        for (String name:new String[]{"bbport.ini",MARKER,FAULT_MARKER,GSR2_MARKER,SKIP_MARKER}) SafeWrite.repair(new File(base,name));
        resetFaultSwitches(base);
        switchToGsr2(base);
        switchOffSkipNetworkChoice(base);
        File marker=new File(base,MARKER), ini=new File(base,"bbport.ini"), defaults=new File(base,"bbport.default.ini");
        if (revision(marker)>=REVISION || !defaults.isFile()) return;
        List<String> wantedLines=lines(defaults);
        Map<String,String> wanted=values(wantedLines);
        if (wanted.isEmpty()) return;
        boolean first=!ini.isFile();
        List<String> out;
        int changed=0;
        if (first) out=wantedLines; // what run-bionic.sh would copy
        else {
            List<String> current=lines(ini);
            Map<String,String> before=values(current);
            out=new ArrayList<>(); Set<String> seen=new HashSet<>();
            for (String line:current) {
                String key=keyOf(line);
                if (key != null && wanted.containsKey(key)) {
                    seen.add(key);
                    if (!kept(key)) { // the last line wins in the file: every line of the key gets the default
                        out.add(key+"="+wanted.get(key));
                        if (!wanted.get(key).equals(before.get(key))) ++changed;
                        continue;
                    }
                }
                out.add(line);
            }
            for (Map.Entry<String,String> e:wanted.entrySet())
                if (!seen.contains(e.getKey())) { out.add(e.getKey()+"="+e.getValue()); ++changed; }
        }
        write(ini,out);
        // The cheats follow cheats.state while the game runs (run-bionic.sh writes it at start too).
        Map<String,String> now=values(out);
        List<String> state=new ArrayList<>();
        state.add(("1".equals(now.get("cheat_health")) ? "1" : "0")+("1".equals(now.get("cheat_items")) ? "1" : "0"));
        write(new File(base,"cheats.state"),state);
        List<String> mark=new ArrayList<>(); mark.add(String.valueOf(REVISION));
        write(marker,mark);
        android.util.Log.i("Bloodborne","default settings applied ("+(first ? "first install"
            : "update from before 0.8.0, "+changed+" settings changed, controller mapping and GPU driver kept")+")");
    }
}
