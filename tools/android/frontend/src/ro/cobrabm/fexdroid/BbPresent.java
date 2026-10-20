package ro.cobrabm.fexdroid;

import android.view.Surface;
import android.view.SurfaceHolder;

/**
 * The Bionic game process's frame path (tools/android/present/bb_present_proto.h): libbbpresent.so
 * listens on a unix socket, receives AHardwareBuffers from the game and shows them on the
 * SurfaceView with an ASurfaceControl. The game stays connected while the surface comes and goes:
 * with no surface its frames are dropped, with a new one they show again.
 */
public final class BbPresent {
    static { System.loadLibrary("bbpresent"); }
    private static native String nativeStart(String socketPath);
    private static native void nativeSetSurface(Surface surface);
    private static native long nativeFrames();
    private static native void nativeSetHold(boolean hold);
    private static native void nativeShutdown();

    private static SurfaceHolder.Callback callback;
    private static SurfaceHolder attached;

    /** Listens on socketPath (once) and shows the game on this surface until it is destroyed. */
    public static synchronized String start(SurfaceHolder holder, String socketPath) {
        String result = nativeStart(socketPath);
        used = true;
        if (!result.equals("present running")) return result;
        if (attached != null && callback != null) attached.removeCallback(callback);
        attached = holder;
        callback = new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder h) { setSurface(h.getSurface()); }
            @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int height) {}
            // The game keeps running: its frames are dropped until a surface is back.
            @Override public void surfaceDestroyed(SurfaceHolder h) { setSurface(null); }
        };
        holder.addCallback(callback);
        setSurface(holder.getSurface().isValid() ? holder.getSurface() : null);
        return "bridge running: bionic present on " + socketPath;
    }

    private static synchronized void setSurface(Surface surface) { nativeSetSurface(surface); }

    /** An online game keeps running while the app is in the background or the screen is off: with hold on, its frames
     *  are released unseen (a few per refresh at most) so it never waits for a display that is not there. */
    public static synchronized void setHold(boolean hold) { if (used) nativeSetHold(hold); }

    /** Frames the game has presented to the app so far. */
    public static long frames() { return nativeFrames(); }

    private static boolean used;
    /** frames() for the status poll: 0 until the Bionic path was started. */
    public static long framesIfLoaded() { return used ? nativeFrames() : 0; }

    /** Ends the listener and the display (the app is closing). */
    public static synchronized void stop() { nativeShutdown(); }
}
