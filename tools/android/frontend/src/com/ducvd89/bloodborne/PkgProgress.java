package com.ducvd89.bloodborne;

import java.util.Locale;

/** The progress of a PKG command (no Android classes, so it is tested on a desktop JDK): parses its
 *  "progress <bytesDone> <bytesTotal> <label>" lines (the older "progress <done> <total>" too) and
 *  works out a smoothed rate for a rough time left. */
final class PkgProgress {
    long done, total;
    String label="";
    private double rate; // bytes per second, smoothed
    private long sampleAt=-1, sampleDone, firstAt=-1;
    private static final long WINDOW=1000; // ms between rate samples
    private static final double SMOOTHING=0.25;

    /** Takes a line; false when it is not a progress line. `now` in ms (any clock). */
    boolean feed(String line,long now) {
        line=line.trim();
        if (!line.startsWith("progress ")) return false;
        String[] f=line.split("\\s+",4);
        long d, t;
        try { d=Long.parseLong(f[1]); t=Long.parseLong(f[2]); }
        catch (RuntimeException e) { return false; }
        if (d<0 || t<0) return false;
        update(d,t,f.length>3 ? f[3].trim() : null,now);
        return true;
    }

    /** Takes the progress itself (what feed parses from a line); label null keeps the last one. */
    void update(long d,long t,String name,long now) {
        done=Math.min(d,t>0 ? t : d); total=t;
        if (name!=null && !name.equals(label)) { // a new phase (checking, extracting, ...): its own rate and estimate
            label=name; rate=0; firstAt=-1; sampleAt=-1;
        }
        if (firstAt<0) { firstAt=now; sampleAt=now; sampleDone=done; }
        else if (now-sampleAt>=WINDOW) {
            double instant=(done-sampleDone)*1000.0/(now-sampleAt);
            rate=rate==0 ? instant : rate+(instant-rate)*SMOOTHING;
            sampleAt=now; sampleDone=done;
        }
    }

    /** 0..1000, or -1 when the total is unknown. */
    int permille() { return total>0 ? (int)Math.min(1000,done*1000/total) : -1; }
    int percent() { return total>0 ? (int)Math.min(100,done*100/total) : 0; }

    /** "12.3 / 41.0 GB" (both in the unit that suits the total). */
    String amount() {
        if (total<=0) return bytes(done);
        double unit=total>=1L<<30 ? 1073741824.0 : 1048576.0;
        String name=total>=1L<<30 ? "GB" : "MB";
        return String.format(Locale.ROOT,"%.1f / %.1f %s",done/unit,total/unit,name);
    }
    static String bytes(long n) {
        if (n>=1L<<30) return String.format(Locale.ROOT,"%.1f GB",n/1073741824.0);
        return String.format(Locale.ROOT,"%.1f MB",n/1048576.0);
    }

    /** Seconds left at the smoothed rate, or -1 until there is a fair estimate (some seconds in). */
    long secondsLeft(long now) {
        if (total<=0 || rate<=0 || firstAt<0 || now-firstAt<3000 || done>=total) return -1;
        return Math.round((total-done)/rate);
    }
    static String left(long seconds) {
        if (seconds<0) return "";
        if (seconds<60) return "less than a minute left";
        long minutes=(seconds+30)/60;
        if (minutes<90) return "about "+minutes+" min left";
        return "about "+minutes/60+" h "+minutes%60+" min left";
    }
    /** "12.3 / 41.0 GB · 30% · about 4 min left". */
    String detail(long now) {
        StringBuilder s=new StringBuilder(amount());
        if (total>0) s.append(" · ").append(percent()).append('%');
        String l=left(secondsLeft(now));
        if (!l.isEmpty()) s.append(" · ").append(l);
        return s.toString();
    }
}
