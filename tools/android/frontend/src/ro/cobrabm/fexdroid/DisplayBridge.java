package ro.cobrabm.fexdroid;

import android.view.Surface;

/** ABI of fexdroid's unmodified MIT-licensed libfxdisplay.so. */
public final class DisplayBridge {
    static { System.loadLibrary("fxdisplay"); }
    public static native String start(Surface surface, String socket, int shmid, int fps, String present);
    public static native void stop();
    public static native long frames();
    public static native long directFrames();
}
