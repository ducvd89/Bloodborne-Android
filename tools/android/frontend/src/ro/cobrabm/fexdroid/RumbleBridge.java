package ro.cobrabm.fexdroid;

import android.content.Context;
import android.os.Build;
import android.os.FileObserver;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.view.InputDevice;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.List;

/** The game's rumble ("<large> <small>" in bbport's atomic rumble file) -> the pad's motors, else the device's own.
 *  No polling: a FileObserver wakes it when the runtime renames a new file in. */
final class RumbleBridge {
    private static final int ON_THRESHOLD=64; // without amplitude control: on at or above this
    private final Context context;
    private final File file;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final List<Vibrator> active = new ArrayList<>();
    private FileObserver observer;
    private boolean enabled=true, drawer, paused;
    private int large, small;

    RumbleBridge(Context context, File file) { this.context=context; this.file=file; }

    /** Starts watching; the file is not read (an old value must not replay). Main thread. */
    void start() {
        File dir=file.getParentFile();
        if (observer!=null || dir==null) return;
        dir.mkdirs(); file.delete();
        final String name=file.getName();
        observer=new FileObserver(dir.getPath(),FileObserver.MOVED_TO|FileObserver.CLOSE_WRITE) {
            @Override public void onEvent(int event, String path) { if (name.equals(path)) handler.post(RumbleBridge.this::read); }
        };
        observer.startWatching();
    }
    void close() { if (observer!=null) { observer.stopWatching(); observer=null; } reset(); }
    /** The setting: off silences it at once; on picks up what the game has set. */
    void setEnabled(boolean on) { enabled=on; if (on) read(); else silence(); }
    /** The drawer is open or the game is held: nothing vibrates then. The file always holds the game's current
     *  motors (the runtime writes only changes), so it is read again when that ends. Any thread. */
    void setDrawer(boolean open) { handler.post(() -> { drawer=open; if (open) silence(); else read(); }); }
    void setPaused(boolean hold) { handler.post(() -> { if (paused==hold) return; paused=hold; if (hold) silence(); else read(); }); }
    /** The game ended or restarts: also the file goes, so the next game does not replay it. */
    void reset() { file.delete(); silence(); }
    private void silence() {
        large=small=0;
        for (Vibrator v:active) v.cancel();
        active.clear();
    }

    private void read() {
        if (!enabled || drawer || paused) return;
        int l, s;
        try (BufferedReader in=new BufferedReader(new FileReader(file))) {
            String[] part=in.readLine().trim().split("\\s+");
            l=Math.max(0,Math.min(255,Integer.parseInt(part[0]))); s=Math.max(0,Math.min(255,Integer.parseInt(part[1])));
        } catch (Exception e) { l=s=0; } // no file: the game set nothing (or 0 0)
        if (l==large && s==small) return;
        large=l; small=s;
        apply();
    }

    /** The pad's vibrators (two: large, small; one: the stronger), else the device's own at the stronger. */
    private void apply() {
        List<Vibrator> targets=new ArrayList<>(); int[] amps;
        List<Vibrator> pad=padVibrators();
        if (pad.size()>=2) { targets.add(pad.get(0)); targets.add(pad.get(1)); amps=new int[]{large,small}; }
        else {
            if (!pad.isEmpty()) targets.add(pad.get(0));
            else { Vibrator own=deviceVibrator(); if (own!=null) targets.add(own); }
            amps=new int[]{Math.max(large,small)};
        }
        for (Vibrator v:active) if (!targets.contains(v)) v.cancel();
        active.clear();
        for (int i=0;i<targets.size();i++) {
            Vibrator v=targets.get(i); int amp=amps[Math.min(i,amps.length-1)];
            if (amp==0) { v.cancel(); continue; }
            try {
                if (Build.VERSION.SDK_INT>=26) {
                    if (v.hasAmplitudeControl()) v.vibrate(VibrationEffect.createWaveform(new long[]{1000},new int[]{amp},0));
                    else if (amp>=ON_THRESHOLD) v.vibrate(VibrationEffect.createWaveform(new long[]{0,1000},0));
                    else { v.cancel(); continue; }
                } else if (amp>=ON_THRESHOLD) v.vibrate(new long[]{0,1000},0);
                else { v.cancel(); continue; }
                active.add(v);
            } catch (RuntimeException e) { /* no vibration permission or a vanished device */ }
        }
    }

    private static List<Vibrator> padVibrators() {
        List<Vibrator> out=new ArrayList<>();
        for (int id:InputDevice.getDeviceIds()) {
            InputDevice d=InputDevice.getDevice(id);
            if (d==null || (d.getSources()&(InputDevice.SOURCE_GAMEPAD|InputDevice.SOURCE_JOYSTICK))==0) continue;
            if (Build.VERSION.SDK_INT>=31) {
                VibratorManager vm=d.getVibratorManager();
                for (int vid:vm.getVibratorIds()) out.add(vm.getVibrator(vid));
            } else {
                Vibrator v=d.getVibrator();
                if (v!=null && v.hasVibrator()) out.add(v);
            }
            if (!out.isEmpty()) break;
        }
        return out;
    }
    private Vibrator deviceVibrator() {
        Vibrator v;
        if (Build.VERSION.SDK_INT>=31) {
            VibratorManager vm=(VibratorManager)context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            v=vm==null ? null : vm.getDefaultVibrator();
        } else v=(Vibrator)context.getSystemService(Context.VIBRATOR_SERVICE);
        return v!=null && v.hasVibrator() ? v : null;
    }
}
