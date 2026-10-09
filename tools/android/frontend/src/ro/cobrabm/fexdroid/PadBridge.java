package ro.cobrabm.fexdroid;

import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

/** Android gamepad events -> bbport's atomic pad-state file, without /dev/input permissions. */
final class PadBridge {
    private final File state;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Set<String> keys = new LinkedHashSet<>();
    private int lx=128, ly=128, rx=128, ry=128, l2, r2, hatX, hatY;
    private boolean queued;

    PadBridge(File state) { this.state=state; reset(); }

    private String name(KeyEvent e) {
        // Thor's Linux scancodes were verified against its printed Nintendo positions.
        switch (e.getScanCode()) {
            case 304: return "cross"; case 305: return "circle";
            case 307: return "square"; case 308: return "triangle";
            case 310: return "l1"; case 311: return "r1";
            case 312: return "l2"; case 313: return "r2";
            case 314: return "touchpad"; case 315: return "options";
            case 317: return "l3"; case 318: return "r3";
        }
        switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_BUTTON_A: return "cross";
            case KeyEvent.KEYCODE_BUTTON_B: return "circle";
            case KeyEvent.KEYCODE_BUTTON_X: return "square";
            case KeyEvent.KEYCODE_BUTTON_Y: return "triangle";
            case KeyEvent.KEYCODE_BUTTON_L1: return "l1";
            case KeyEvent.KEYCODE_BUTTON_R1: return "r1";
            case KeyEvent.KEYCODE_BUTTON_L2: return "l2";
            case KeyEvent.KEYCODE_BUTTON_R2: return "r2";
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return "l3";
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return "r3";
            case KeyEvent.KEYCODE_BUTTON_START: return "options";
            case KeyEvent.KEYCODE_BUTTON_SELECT: return "touchpad";
            case KeyEvent.KEYCODE_DPAD_UP: return "up";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "down";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "left";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "right";
            default: return null;
        }
    }

    boolean key(KeyEvent e) {
        String name=name(e);
        if (name==null) return false;
        if (e.getAction()==KeyEvent.ACTION_DOWN) keys.add(name);
        else if (e.getAction()==KeyEvent.ACTION_UP) keys.remove(name);
        // Publish button transitions immediately; the runtime samples at 20 ms intervals.
        write();
        return true;
    }

    boolean motion(MotionEvent e) {
        if (!e.isFromSource(InputDevice.SOURCE_JOYSTICK)) return false;
        lx=stick(e.getAxisValue(MotionEvent.AXIS_X));
        ly=stick(e.getAxisValue(MotionEvent.AXIS_Y));
        rx=stick(e.getAxisValue(MotionEvent.AXIS_Z));
        ry=stick(e.getAxisValue(MotionEvent.AXIS_RZ));
        l2=trigger(Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER),e.getAxisValue(MotionEvent.AXIS_BRAKE)));
        r2=trigger(Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER),e.getAxisValue(MotionEvent.AXIS_GAS)));
        hatX=Math.round(e.getAxisValue(MotionEvent.AXIS_HAT_X));
        hatY=Math.round(e.getAxisValue(MotionEvent.AXIS_HAT_Y));
        if (!queued) { queued=true; handler.postDelayed(() -> { queued=false; write(); },8); }
        return true;
    }

    private static int stick(float v) { return Math.abs(v)<0.06f ? 128 : Math.max(0,Math.min(255,Math.round((v+1)*127.5f))); }
    private static int trigger(float v) { return Math.max(0,Math.min(255,Math.round(v*255))); }

    void reset() {
        keys.clear(); lx=ly=rx=ry=128; l2=r2=hatX=hatY=0;
        write();
    }

    private void write() {
        StringBuilder text=new StringBuilder();
        for (String key:keys) {
            if ((key.equals("l2") && l2>0) || (key.equals("r2") && r2>0)) continue;
            text.append(key).append(' ');
        }
        if (hatX<0) text.append("left "); if (hatX>0) text.append("right ");
        if (hatY<0) text.append("up "); if (hatY>0) text.append("down ");
        text.append("lx=").append(lx).append(" ly=").append(ly)
            .append(" rx=").append(rx).append(" ry=").append(ry)
            .append(" l2=").append(l2).append(" r2=").append(r2).append('\n');
        File temp=new File(state.getPath()+".tmp");
        try {
            state.getParentFile().mkdirs();
            try (FileOutputStream out=new FileOutputStream(temp)) {
                out.write(text.toString().getBytes(StandardCharsets.US_ASCII));
            }
            if (!temp.renameTo(state)) throw new java.io.IOException("Cannot publish controller state");
        } catch (java.io.IOException ex) { android.util.Log.e("Bloodborne", "Controller bridge",ex); }
    }
}
