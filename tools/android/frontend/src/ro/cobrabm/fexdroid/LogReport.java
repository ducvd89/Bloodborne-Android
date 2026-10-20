package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.util.DisplayMetrics;
import android.view.Display;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.TreeSet;
import java.util.concurrent.TimeUnit;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** The "Report a problem" zip: what a maintainer needs to read a GitHub issue (device, settings, the game's last two
 *  logs, this app's logcat, the shaders the driver rejected). Never saves, game files, Lossless.dll or Steam data.
 *  Blocks: runs off the UI thread. The zip is built in the app's own reports folder (shared through
 *  {@link ReportProvider}) and copied to Download when that can be written. */
final class LogReport {
    /** What MainActivity knows and this class does not. */
    static final class Facts {
        String gameFolder;      // last component of the game folder, null when none is chosen
        boolean ebootFound;
        String modsLine;        // "off" / "2 of 3 enabled"
        String driverLine;      // "system" or the driver's name and version
        boolean losslessDll;
    }
    static final class Result {
        File zip;               // in the app's reports folder (always); shared from here
        File saved;             // the copy in Download, or null
        String saveProblem;     // why there is no copy in Download
        String summary;         // report.txt's device lines, for the issue text
    }

    static final long LOG_CAP=8L<<20, LOGCAT_CAP=4L<<20, SHADER_CAP=5L<<20;
    /** A line carrying a secret: never copied to the report. */
    private static final Pattern SECRET_LINE=Pattern.compile("(?i)(token|passw(or)?d|cookie|secret|steam_?guard|refresh_?key)\\s*[=:]");
    private static final Pattern SECRET_KEY=Pattern.compile("(?i)token|password|key|cookie|steam");
    static final String PREFIX="Bloodborne-logs-";
    private LogReport() {}

    /** The folder of the shared zip (files/reports). */
    static File reportsDir(Context c) { return new File(c.getFilesDir(),"reports"); }

    static Result build(Activity activity,File base,Facts facts,File downloads) throws IOException {
        Date now=new Date();
        String stamp=new SimpleDateFormat("yyyyMMdd-HHmmss",Locale.ROOT).format(now);
        File dir=reportsDir(activity);
        FsUtil.deleteTree(dir); // only the latest report is kept
        if (!dir.mkdirs() && !dir.isDirectory()) throw new IOException("Cannot create "+dir);
        Result result=new Result();
        result.zip=new File(dir,PREFIX+stamp+".zip");
        int[] removed={0};
        GpuFamily chip=GpuFamily.detect(base.getPath());
        StringBuilder summary=new StringBuilder();
        try (ZipOutputStream zip=new ZipOutputStream(new java.io.BufferedOutputStream(Files.newOutputStream(result.zip.toPath())))) {
            zip.setLevel(6);
            // The logs first (they are the part that can fail), the report last (it counts what was left out).
            String logcatNote=logcat(zip,removed);
            tail(zip,new File(base,"logs/android-game.log"),"android-game.log",LOG_CAP,removed);
            tail(zip,new File(base,"logs/android-game.prev.log"),"android-game.prev.log",LOG_CAP,removed);
            tail(zip,AtlasDoorLog.file(base),"atlas-doors.log",LOG_CAP,removed); // the map's door-flag calibration log (Atlas, Debug), when it was on
            String settings=settings(zip,new File(base,"bbport.ini"));
            String shaders=shaders(zip,new File(base,"logs/failed-shaders"));
            summary.append(deviceLines(activity,facts,chip,base));
            StringBuilder text=new StringBuilder(summary);
            text.append("Settings: ").append(settings).append('\n');
            text.append("Logcat: ").append(logcatNote).append('\n');
            text.append("Failed shaders: ").append(shaders).append('\n');
            if (removed[0]>0) text.append("Log lines left out as possible secrets: ").append(removed[0]).append('\n');
            text.append("Report time: ").append(new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z",Locale.ROOT).format(now)).append('\n');
            put(zip,"report.txt",text.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException | RuntimeException e) {
            result.zip.delete(); throw e;
        }
        result.summary=summary.toString();
        // Download copy, when storage can be written (the share button works without it).
        try {
            if (downloads==null) throw new IOException("no Download folder");
            if (!downloads.isDirectory() && !downloads.mkdirs()) throw new IOException("Cannot create "+downloads);
            File dest=new File(downloads,result.zip.getName());
            for (int n=2; dest.exists(); n++) dest=new File(downloads,PREFIX+stamp+"-"+n+".zip");
            Files.copy(result.zip.toPath(),dest.toPath());
            result.saved=dest;
        } catch (IOException | RuntimeException e) {
            android.util.Log.w("Bloodborne","report: Download copy: "+e);
            result.saveProblem=e.getMessage()==null ? e.toString() : e.getMessage();
        }
        return result;
    }

    // ---- report.txt ----
    private static String deviceLines(Activity a,Facts f,GpuFamily chip,File base) {
        StringBuilder s=new StringBuilder();
        try {
            PackageInfo p=a.getPackageManager().getPackageInfo(a.getPackageName(),0);
            line(s,"App",p.versionName+" ("+p.getLongVersionCode()+")");
        } catch (Exception e) { line(s,"App","unknown ("+e+")"); }
        line(s,"Device",Build.MANUFACTURER+" "+Build.MODEL+(Build.VERSION.SDK_INT>=31 && !Build.SOC_MODEL.isEmpty() && !"unknown".equals(Build.SOC_MODEL)
            ? ", SoC "+Build.SOC_MANUFACTURER+" "+Build.SOC_MODEL : ""));
        line(s,"Android","Android "+Build.VERSION.RELEASE+" (API "+Build.VERSION.SDK_INT+"), build "+Build.DISPLAY);
        line(s,"ABI",String.join(", ",Build.SUPPORTED_ABIS));
        try {
            ActivityManager.MemoryInfo m=new ActivityManager.MemoryInfo();
            ((ActivityManager)a.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(m);
            line(s,"RAM",String.format(Locale.ROOT,"%.1f GB total",m.totalMem/1073741824.0));
        } catch (Exception e) { line(s,"RAM","unknown"); }
        try {
            Display d=a.getWindowManager().getDefaultDisplay();
            DisplayMetrics metrics=new DisplayMetrics(); d.getRealMetrics(metrics);
            TreeSet<String> rates=new TreeSet<>();
            for (Display.Mode m:d.getSupportedModes()) rates.add(String.format(Locale.ROOT,"%.0f",m.getRefreshRate()));
            List<String> sorted=new ArrayList<>(rates);
            java.util.Collections.sort(sorted,(x,y) -> Integer.compare(Integer.parseInt(x),Integer.parseInt(y)));
            line(s,"Display",metrics.widthPixels+"x"+metrics.heightPixels+" at "+String.format(Locale.ROOT,"%.1f",d.getRefreshRate())
                +" Hz, supported "+String.join("/",sorted)+" Hz");
        } catch (Exception e) { line(s,"Display","unknown"); }
        line(s,"GPU",chip.describe());
        line(s,"GPU driver",f.driverLine);
        line(s,"Game folder",f.gameFolder==null ? "not chosen" : f.gameFolder+(f.ebootFound ? " (eboot.bin found)" : " (no eboot.bin)"));
        line(s,"Mods",f.modsLine);
        line(s,"Lossless.dll",f.losslessDll ? "present" : "not present");
        try { line(s,"Online",new OnlineSettings(a).summary()); } // the pool only: never the account, password or token
        catch (Exception e) { line(s,"Online","unknown"); }
        // The online session's last state (net-status.txt of the last online game), without the account name.
        try {
            OnlineSettings online=new OnlineSettings(a);
            File status=OnlineStatus.file(base);
            line(s,"Online status",OnlineStatus.forReport(status.isFile() ? OnlineStatus.read(status) : null,online.npid(),online.thdOnlineId()));
        } catch (Exception e) { line(s,"Online status","unknown"); }
        return s.toString();
    }
    private static void line(StringBuilder s,String key,String value) { s.append(key).append(": ").append(value).append('\n'); }

    /** kgsl-chip-id's first line (e.g. "0x44050001 ..."), or null when the GPU cannot be read. Blocks up to 5 s. */
    static String chipId(String base) {
        try {
            ProcessBuilder b=new ProcessBuilder(base+"/bionic/bin/kgsl-chip-id").redirectErrorStream(true);
            b.environment().remove("LD_LIBRARY_PATH"); b.environment().remove("LD_PRELOAD");
            Process p=b.start();
            try {
                if (!p.waitFor(5,TimeUnit.SECONDS)) return null;
                String line=new BufferedReader(new InputStreamReader(p.getInputStream())).readLine();
                if (line==null || !line.trim().startsWith("0x")) return null;
                return line.trim();
            } finally { p.destroyForcibly(); }
        } catch (Exception ex) {
            android.util.Log.w("Bloodborne","GPU detection: "+ex);
            return null;
        }
    }

    // ---- bbport.ini ----
    /** Copies the settings without secret-looking keys. Returns the line for report.txt. */
    private static String settings(ZipOutputStream zip,File ini) throws IOException {
        if (!ini.isFile()) return "no bbport.ini";
        StringBuilder out=new StringBuilder(); int skipped=0;
        try (BufferedReader r=new BufferedReader(new InputStreamReader(new FileInputStream(ini),StandardCharsets.UTF_8))) {
            for (String line; (line=r.readLine())!=null;) {
                int eq=line.indexOf('=');
                if (eq>=0 && SECRET_KEY.matcher(line.substring(0,eq)).find()) { ++skipped; continue; }
                out.append(line).append('\n');
            }
        }
        put(zip,"bbport.ini",out.toString().getBytes(StandardCharsets.UTF_8));
        return "bbport.ini"+(skipped>0 ? " ("+skipped+" lines with secret-looking keys left out)" : "");
    }

    // ---- the game logs ----
    /** Adds the last `cap` bytes of a log (whole lines), a note first when the start was cut. A missing log is skipped. */
    private static void tail(ZipOutputStream zip,File file,String name,long cap,int[] removed) throws IOException {
        if (!file.isFile()) return;
        long size=file.length();
        zip.putNextEntry(new ZipEntry(name));
        Writer w=new OutputStreamWriter(zip,StandardCharsets.UTF_8);
        try (FileInputStream in=new FileInputStream(file)) {
            if (size>cap) {
                long skip=size-cap;
                while (skip>0) { long n=in.skip(skip); if (n<=0) break; skip-=n; }
                for (int c; (c=in.read())>=0 && c!='\n';) {} // the cut line
                w.write("[Cut: only the last "+(cap>>20)+" MB of this "+(size>>20)+" MB log is kept]\n");
            }
            copyLines(new BufferedReader(new InputStreamReader(in,StandardCharsets.UTF_8)),w,removed);
        }
        w.flush(); zip.closeEntry();
    }
    private static void copyLines(BufferedReader r,Writer w,int[] removed) throws IOException {
        for (String line; (line=r.readLine())!=null;) {
            if (SECRET_LINE.matcher(line).find()) { ++removed[0]; w.write("[line left out: looks like a secret]\n"); }
            else { w.write(OnlineSettings.scrub(line)); w.write('\n'); } // the online account's password and token, if one got in
        }
    }

    // ---- logcat ----
    /** This app's logcat (the game process runs under the same uid, so it is in it). Returns the line for report.txt. */
    private static String logcat(ZipOutputStream zip,int[] removed) throws IOException {
        String note; ArrayDeque<String> lines=new ArrayDeque<>(); long bytes=0; boolean cut=false;
        Process p=null;
        try {
            // Without READ_LOGS Android shows an app only its own uid's lines: this app and the game process.
            p=new ProcessBuilder("logcat","-d","-v","threadtime").redirectErrorStream(true).start();
            try (BufferedReader r=new BufferedReader(new InputStreamReader(p.getInputStream(),StandardCharsets.UTF_8))) {
                for (String line; (line=r.readLine())!=null;) {
                    if (SECRET_LINE.matcher(line).find()) { ++removed[0]; continue; }
                    line=OnlineSettings.scrub(line);
                    lines.addLast(line); bytes+=line.length()+1;
                    while (bytes>LOGCAT_CAP && lines.size()>1) { bytes-=lines.pollFirst().length()+1; cut=true; }
                }
            }
            if (!p.waitFor(5,TimeUnit.SECONDS)) throw new IOException("logcat did not finish");
            if (p.exitValue()!=0 && lines.isEmpty()) throw new IOException("logcat exited with "+p.exitValue());
            note="logcat.txt, "+lines.size()+" lines"+(cut ? " (cut to the last 4 MB)" : "");
        } catch (Exception e) {
            android.util.Log.w("Bloodborne","report: logcat: "+e);
            note="could not be read ("+e+")";
            lines.clear(); lines.add("logcat could not be read: "+e); cut=false;
        } finally { if (p!=null) p.destroyForcibly(); }
        zip.putNextEntry(new ZipEntry("logcat.txt"));
        Writer w=new OutputStreamWriter(zip,StandardCharsets.UTF_8);
        if (cut) w.write("[Cut: only the last 4 MB of logcat is kept]\n");
        for (String line:lines) { w.write(line); w.write('\n'); }
        w.flush(); zip.closeEntry();
        return note;
    }

    // ---- the shaders the driver rejected ----
    private static String shaders(ZipOutputStream zip,File dir) throws IOException {
        if (!dir.isDirectory()) return "none";
        List<File> files=new ArrayList<>(); long[] total={0};
        walk(dir,files,total);
        if (files.isEmpty()) return "none";
        if (total[0]<=SHADER_CAP) {
            for (File f:files) {
                zip.putNextEntry(new ZipEntry("failed-shaders/"+rel(dir,f)));
                try (InputStream in=new FileInputStream(f)) { copy(in,zip); } catch (IOException e) { /* a file vanished: keep the entry */ }
                zip.closeEntry();
            }
            return files.size()+" files, "+(total[0]/1024)+" KB (included)";
        }
        StringBuilder list=new StringBuilder("Too large to include ("+(total[0]>>20)+" MB); names and sizes:\n");
        int n=0;
        for (File f:files) { if (++n>2000) { list.append("...\n"); break; } list.append(rel(dir,f)).append(' ').append(f.length()).append('\n'); }
        put(zip,"failed-shaders/listing.txt",list.toString().getBytes(StandardCharsets.UTF_8));
        return files.size()+" files, "+(total[0]>>20)+" MB (listing only)";
    }
    private static void walk(File dir,List<File> out,long[] total) {
        File[] children=dir.listFiles();
        if (children==null) return;
        java.util.Arrays.sort(children);
        for (File c:children) {
            if (Files.isSymbolicLink(c.toPath())) continue;
            if (c.isDirectory()) walk(c,out,total); else if (c.isFile()) { out.add(c); total[0]+=c.length(); }
        }
    }
    private static String rel(File root,File f) { return f.getPath().substring(root.getPath().length()+1); }

    private static void put(ZipOutputStream zip,String name,byte[] data) throws IOException {
        zip.putNextEntry(new ZipEntry(name)); zip.write(data); zip.closeEntry();
    }
    private static void copy(InputStream in,OutputStream out) throws IOException {
        byte[] buf=new byte[64*1024];
        for (int n; (n=in.read(buf))>=0;) out.write(buf,0,n);
    }
}
