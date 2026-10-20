package ro.cobrabm.fexdroid;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.system.Os;
import android.system.OsConstants;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileInputStream;
import java.io.RandomAccessFile;

/** Game audio: the game's mixer writes its output (s16 stereo 48 kHz, BB_AUDIO_FIFO, run-bionic.sh) into a
 *  FIFO; this plays it. The FIFO is held open read-write, so the game's open never blocks and a
 *  restarted game writes into the same pipe. The playback rate paces the game's audio thread. */
final class AudioBridge {
    private static final int RATE=48000, FRAME=4;
    private static final int F_SETPIPE_SZ=1031;
    private final File fifo;
    private volatile boolean running;
    private volatile AudioTrack track;
    private volatile boolean muted;

    AudioBridge(File fifo) { this.fifo=fifo; }

    void start() {
        if (running) return;
        running=true;
        Thread thread=new Thread(this::run,"Bloodborne-audio");
        thread.setPriority(Thread.MAX_PRIORITY); thread.setDaemon(true); thread.start();
    }
    void stop() { running=false; AudioTrack t=track; if (t!=null) t.pause(); }
    void pause() { AudioTrack t=track; if (t!=null) t.pause(); }
    void resume() { AudioTrack t=track; if (t!=null) t.play(); }
    /** Silence without stopping the flow: the track keeps playing at volume 0, so the game's audio thread (paced by the
     *  FIFO this drains) never blocks. Pausing the track would fill the FIFO and stall the game. */
    void mute(boolean muted) {
        this.muted=muted;
        AudioTrack t=track;
        if (t!=null) t.setVolume(muted ? 0f : 1f);
    }

    private void run() {
        try {
            if (fifo.exists() && !isFifo(fifo)) fifo.delete();
            if (!fifo.exists()) Os.mkfifo(fifo.getPath(),0600);
            try (RandomAccessFile pipe=new RandomAccessFile(fifo,"rw");
                 FileInputStream in=new FileInputStream(pipe.getFD())) {
                FileDescriptor fd=pipe.getFD();
                // A small pipe keeps the delay short (~40 ms); the default 64 KiB is ~340 ms.
                try { Os.fcntlInt(fd,F_SETPIPE_SZ,8192); } catch (Exception ignored) {}
                int min=AudioTrack.getMinBufferSize(RATE,AudioFormat.CHANNEL_OUT_STEREO,AudioFormat.ENCODING_PCM_16BIT);
                AudioTrack t=new AudioTrack.Builder()
                    .setAudioAttributes(captureAll(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)).build())
                    .setAudioFormat(new AudioFormat.Builder().setSampleRate(RATE)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
                    .setBufferSizeInBytes(Math.max(min,RATE*FRAME/20))
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY).build();
                t.setVolume(muted ? 0f : 1f); track=t; t.play();
                byte[] buffer=new byte[4096];
                int carry=0;
                while (running) {
                    int n=in.read(buffer,carry,buffer.length-carry);
                    if (n<=0) { Thread.sleep(5); continue; }
                    int whole=(carry+n)/FRAME*FRAME;
                    t.write(buffer,0,whole);
                    carry=carry+n-whole;
                    if (carry>0) System.arraycopy(buffer,whole,buffer,0,carry);
                }
                t.stop(); t.release(); track=null;
            }
        } catch (Exception ex) { android.util.Log.e("Bloodborne","audio: "+ex); }
    }
    private static boolean isFifo(File file) {
        try { return OsConstants.S_ISFIFO(Os.stat(file.getPath()).st_mode); } catch (Exception e) { return false; }
    }

    /** Screen recorders may capture the game's sound (Android 10+; the manifest allows it for the app). */
    private static AudioAttributes.Builder captureAll(AudioAttributes.Builder b) {
        if (android.os.Build.VERSION.SDK_INT>=29) b.setAllowedCapturePolicy(AudioAttributes.ALLOW_CAPTURE_BY_ALL);
        return b;
    }
}
