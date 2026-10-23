package ro.cobrabm.fexdroid;

import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

/** Android gamepad events -> bbport's atomic pad-state file, without /dev/input permissions. */
final class PadBridge {
    private final File state;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Set<String> keys = new LinkedHashSet<>();
    private final Set<String> virtualKeys = new LinkedHashSet<>();
    private int lx=128, ly=128, rx=128, ry=128, l2, r2, hatX, hatY;
    private int virtualLx=128, virtualLy=128, virtualRx=128, virtualRy=128, virtualL2, virtualR2;
    private boolean virtualLeftActive, virtualRightActive;
    private boolean queued;
    /** The 16 button names, in the order the remap menu lists them. */
    static final String[] BUTTONS={"cross","circle","square","triangle","l1","r1","l2","r2","l3","r3","options","touchpad","up","down","left","right"};
    private static final int TRIGGER_PRESS=64;
    /** Physical name -> target name (a BUTTONS name or "none"); a missing name is itself. Physical pad only. */
    private Map<String,String> remap=new HashMap<>();

    PadBridge(File state) { this.state=state; reset(); }

    /** "cross=circle,circle=none" -> map; unknown names are dropped, identity entries too. */
    static Map<String,String> parse(String text) {
        Map<String,String> map=new LinkedHashMap<>();
        if (text==null) return map;
        for (String part:text.split(",")) {
            String[] kv=part.split("=");
            if (kv.length==2 && valid(kv[0],false) && valid(kv[1],true) && !kv[0].equals(kv[1])) map.put(kv[0],kv[1]);
        }
        return map;
    }
    static String format(Map<String,String> map) {
        StringBuilder text=new StringBuilder();
        for (String b:BUTTONS) {
            String t=map.get(b);
            if (t!=null && !t.equals(b)) text.append(text.length()>0 ? "," : "").append(b).append('=').append(t);
        }
        return text.toString();
    }
    private static boolean valid(String name,boolean none) {
        if (none && name.equals("none")) return true;
        for (String b:BUTTONS) if (b.equals(name)) return true;
        return false;
    }

    /** Applies at once: the physical state is kept as it is and mapped on every write, so nothing stays pressed. */
    void remap(Map<String,String> map) { remap=new HashMap<>(map); write(); }

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

    void virtual(Set<String> buttons, int leftX, int leftY, int rightX, int rightY,
                 boolean leftActive, boolean rightActive, int leftTrigger, int rightTrigger) {
        virtualKeys.clear(); virtualKeys.addAll(buttons);
        virtualLx=leftX; virtualLy=leftY; virtualRx=rightX; virtualRy=rightY;
        virtualLeftActive=leftActive; virtualRightActive=rightActive;
        virtualL2=leftTrigger; virtualR2=rightTrigger;
        write();
    }

    void reset() {
        keys.clear(); lx=ly=rx=ry=128; l2=r2=hatX=hatY=0;
        virtualKeys.clear(); virtualLx=virtualLy=virtualRx=virtualRy=128;
        virtualL2=virtualR2=0; virtualLeftActive=virtualRightActive=false;
        write();
    }

    private String target(String from) { String to=remap.get(from); return to==null ? from : to; }
    /** A held physical button: its target is pressed; a trigger target (not the button's own) is held at 255. */
    private void button(Set<String> out,int[] trigger,String from) {
        String to=target(from);
        if (to.equals("none")) return;
        if ((to.equals("l2") || to.equals("r2")) && !to.equals(from)) fire(trigger,to,255);
        else out.add(to);
    }
    /** A physical trigger's analog value: kept on a trigger target, else the target button is pressed from TRIGGER_PRESS. */
    private void analog(Set<String> out,int[] trigger,String from,int value) {
        String to=target(from);
        if (value<=0 || to.equals("none")) return;
        if (to.equals("l2") || to.equals("r2")) fire(trigger,to,value);
        else if (value>=TRIGGER_PRESS) out.add(to);
    }
    private static void fire(int[] trigger,String to,int value) { int i=to.equals("l2") ? 0 : 1; trigger[i]=Math.max(trigger[i],value); }

    private void write() {
        StringBuilder text=new StringBuilder();
        Set<String> all=new LinkedHashSet<>();
        int[] trigger={0,0}; // left and right trigger values from the physical pad, after mapping
        for (String key:keys) button(all,trigger,key);
        if (hatX<0) button(all,trigger,"left"); if (hatX>0) button(all,trigger,"right");
        if (hatY<0) button(all,trigger,"up"); if (hatY>0) button(all,trigger,"down");
        analog(all,trigger,"l2",l2); analog(all,trigger,"r2",r2);
        all.addAll(virtualKeys);
        int leftTrigger=Math.max(trigger[0],virtualL2), rightTrigger=Math.max(trigger[1],virtualR2);
        for (String key:all) {
            if ((key.equals("l2") && leftTrigger>0) || (key.equals("r2") && rightTrigger>0)) continue;
            text.append(key).append(' ');
        }
        text.append("lx=").append(virtualLeftActive ? virtualLx : lx)
            .append(" ly=").append(virtualLeftActive ? virtualLy : ly)
            .append(" rx=").append(virtualRightActive ? virtualRx : rx)
            .append(" ry=").append(virtualRightActive ? virtualRy : ry)
            .append(" l2=").append(leftTrigger).append(" r2=").append(rightTrigger).append('\n');
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
