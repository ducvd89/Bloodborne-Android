package ro.cobrabm.fexdroid;

/** ABI of fexdroid's unmodified MIT-licensed libfxio.so. */
public final class XInput {
    static { System.loadLibrary("fxio"); }
    public static native String connect(int display);
    public static native void key(int keycode, boolean down);
    public static native void button(int button, boolean down);
    public static native void moveTo(int x, int y);
}
