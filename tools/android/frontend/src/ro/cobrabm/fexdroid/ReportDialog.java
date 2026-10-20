package ro.cobrabm.fexdroid;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;

import java.io.File;
import java.io.UnsupportedEncodingException;
import java.net.URLEncoder;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.Executor;

/** Menu -> Report a problem: explains what is collected, builds the zip ({@link LogReport}) off the UI thread and
 *  offers to open a prefilled GitHub issue or to share the zip. */
final class ReportDialog {
    interface Host {
        Executor worker();
        void post(Runnable r);
        boolean destroyed();
        void closed();                      // back to the immersive game screen
        LogReport.Facts facts();            // blocks (lists the mods)
        File base();
        File downloads();
    }
    static final String ISSUES="https://github.com/ducvd89/Bloodborne-Android/issues/new";
    private static final int URL_LIMIT=6000;
    private final Activity activity;
    private final Host host;
    private boolean busy;
    private final Map<String,String> uploaded=new HashMap<>();   // zip path -> link, so a second tap does not upload again

    ReportDialog(Activity activity,Host host) { this.activity=activity; this.host=host; }

    void show() {
        if (busy) return;
        new AlertDialog.Builder(activity).setTitle("Report a problem")
            .setMessage("This saves a zip in your Download folder for a GitHub issue. It holds:\n"
                +"• your device model, Android and app versions, GPU and the GPU driver\n"
                +"• the app settings (bbport.ini, without anything that looks like a password or key)\n"
                +"• whether online play is on and with which pool, and the online connection's last state (never the online account, password or token)\n"
                +"• the game's log of this run and of the run before, and this app's system log\n"
                +"• the shaders the driver rejected, if any\n"
                +"• the map's door log (door flag changes with your position), if Log door flags was on\n\n"
                +"It does not hold your saves, game files, Lossless.dll or Steam account data. The logs can show folder "
                +"names on your phone; look through the zip first if that matters to you.\n\n"
                +"\"Open GitHub issue\" uploads the zip to a public file host (gofile.io) and links it in the issue. Anyone with the link can "
                +"open it. \"Share zip\" does not upload anything.")
            .setNegativeButton("Cancel",null)
            .setPositiveButton("Save logs",(d,w) -> build())
            .setOnDismissListener(d -> host.closed()).show();
    }

    private void build() {
        busy=true;
        AlertDialog progress=new AlertDialog.Builder(activity).setTitle("Saving logs…").setMessage("Please wait.").setCancelable(false).show();
        host.worker().execute(() -> {
            LogReport.Result result=null; String problem=null;
            try { result=LogReport.build(activity,host.base(),host.facts(),host.downloads()); }
            catch (Exception ex) {
                problem=ex.getMessage()==null ? ex.toString() : ex.getMessage();
                android.util.Log.e("Bloodborne","report: "+ex);
            }
            LogReport.Result done=result; String failure=problem;
            host.post(() -> {
                busy=false; progress.dismiss();
                if (host.destroyed()) return;
                if (done==null) {
                    new AlertDialog.Builder(activity).setTitle("Could not save the logs").setMessage(failure)
                        .setPositiveButton("OK",null).setOnDismissListener(d -> host.closed()).show();
                } else result(done);
            });
        });
    }

    private void result(LogReport.Result r) {
        String where=r.saved!=null ? "Saved to\n"+r.saved.getPath()
            : "Could not copy it to the Download folder ("+r.saveProblem+"). \"Share zip\" still sends it.";
        AlertDialog dialog=new AlertDialog.Builder(activity).setTitle("Logs saved")
            .setMessage(where+"\n\n\"Open GitHub issue\" uploads the zip to gofile.io (a public link anyone with it can open) "
                +"and links it in the issue. \"Share zip\" sends it to another app instead, without uploading.")
            .setPositiveButton("Open GitHub issue",null)
            .setNeutralButton("Share zip",null)
            .setNegativeButton("Close",null)
            .setOnDismissListener(d -> host.closed()).create();
        dialog.show();
        // The two actions leave the dialog open: the zip is then shared after the issue was opened, or the other way.
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> openIssue(r));
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(v -> share(r));
    }

    /** Uploads the zip (once), then opens the issue with its link. */
    private void openIssue(LogReport.Result r) {
        if (busy) return;
        String link=uploaded.get(r.zip.getPath());
        if (link!=null) { openIssue(r,link); return; }
        busy=true;
        ReportUpload.Job job=new ReportUpload.Job();
        long total=r.zip.length();
        AlertDialog progress=new AlertDialog.Builder(activity).setTitle("Uploading the logs…")
            .setMessage("0.0 / "+ReportUpload.megabytes(total)+" MB").setCancelable(false)
            .setNegativeButton("Cancel",(d,w) -> job.cancel()).show();
        String agent="Bloodborne-Android/"+version()+" (report upload)";
        long[] lastPost={0};
        new Thread(() -> {
            String url=null, problem=null; boolean cancelled=false;
            try {
                url=ReportUpload.upload(r.zip,agent,job,(sent,all) -> {
                    long now=System.nanoTime();
                    if (sent<all && now-lastPost[0]<200_000_000L) return;
                    lastPost[0]=now;
                    host.post(() -> progress.setMessage(ReportUpload.megabytes(sent)+" / "+ReportUpload.megabytes(all)+" MB"));
                });
            } catch (ReportUpload.Cancelled ex) { cancelled=true; }
            catch (Exception ex) { problem=ex.getMessage()==null ? ex.toString() : ex.getMessage(); }
            String link2=url, failure=problem; boolean stop=cancelled;
            host.post(() -> {
                busy=false; progress.dismiss();
                if (stop || host.destroyed()) return;
                if (link2!=null) { uploaded.put(r.zip.getPath(),link2); openIssue(r,link2); }
                else uploadFailed(r,failure);
            });
        },"report-upload").start();
    }
    private void uploadFailed(LogReport.Result r,String why) {
        new AlertDialog.Builder(activity).setTitle("Could not upload the logs")
            .setMessage(why+"\n\nYou can open the issue anyway and drag the zip into it from your Download folder, or try again.")
            .setPositiveButton("Open issue without upload",(d,w) -> openIssue(r,null))
            .setNeutralButton("Try again",(d,w) -> openIssue(r))
            .setNegativeButton("Close",null).show();
    }
    private void openIssue(LogReport.Result r,String link) {
        try { activity.startActivity(new Intent(Intent.ACTION_VIEW,Uri.parse(issueUrl(r,link)))); }
        catch (Exception ex) { Toast.makeText(activity,"No browser to open GitHub: "+ex.getMessage(),Toast.LENGTH_LONG).show(); }
    }
    private String version() {
        try { return activity.getPackageManager().getPackageInfo(activity.getPackageName(),0).versionName; }
        catch (Exception ex) { return "unknown"; }
    }
    private void share(LogReport.Result r) {
        try {
            Uri uri=ReportProvider.uriOf(activity,r.zip);
            Intent send=new Intent(Intent.ACTION_SEND).setType("application/zip")
                .putExtra(Intent.EXTRA_STREAM,uri).putExtra(Intent.EXTRA_SUBJECT,"Bloodborne for Android logs")
                .putExtra(Intent.EXTRA_TEXT,"Logs for a Bloodborne for Android issue: "+r.zip.getName())
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            send.setClipData(ClipData.newRawUri(r.zip.getName(),uri));
            activity.startActivity(Intent.createChooser(send,"Share the logs"));
        } catch (Exception ex) { Toast.makeText(activity,"Cannot share: "+ex.getMessage(),Toast.LENGTH_LONG).show(); }
    }

    // ---- the issue ----
    /** The report's "Key: value" lines. */
    private static Map<String,String> lines(String summary) {
        Map<String,String> map=new LinkedHashMap<>();
        for (String line:summary.split("\n")) {
            int colon=line.indexOf(": ");
            if (colon>0) map.put(line.substring(0,colon),line.substring(colon+2));
        }
        return map;
    }
    /** The new-issue address for the repository's "Bug report" form (.github/ISSUE_TEMPLATE/bug_report.yml): its fields
     *  are prefilled by id, and a plain "body" is sent too for the case the form is gone. Under {@link #URL_LIMIT}.
     *  link: where the zip was uploaded, or null for the "drag it into the box" text. */
    static String issueUrl(LogReport.Result r,String link) {
        Map<String,String> m=lines(r.summary);
        String gpu=m.get("GPU");
        String device=m.get("Device")+(gpu==null || gpu.equals("unknown") ? "" : ", GPU id "+gpu);
        String attach=link!=null ? "Logs: "+link+" (uploaded by the app)"
            : "Attach "+r.zip.getName()+" from your Download folder (drag it into this box).";
        String what="What happened:\n\n\nSteps to reproduce:\n1. \n\nExpected:\n";
        StringBuilder env=new StringBuilder();
        for (Map.Entry<String,String> e:m.entrySet()) env.append("- ").append(e.getKey()).append(": ").append(e.getValue()).append('\n');
        String body="### What happened\n\n\n### Steps\n1. \n\n### Expected\n\n\n### Environment\n"+env+"\n"+attach+"\n";

        String[][] fields={{"template","bug_report.yml"},{"title","[Android] Short description of the problem"},
            {"device",device},{"android",m.get("Android")},{"ram",m.get("RAM")},{"app-version",m.get("App")},
            {"game",m.get("Game folder")},{"driver",m.get("GPU driver")},
            {"settings","Mods: "+m.get("Mods")+"; Lossless.dll "+m.get("Lossless.dll")+". Full settings: bbport.ini in the zip."},
            {"what",what},{"log",attach},{"body",body}};
        for (int count=fields.length; count>=3; count--) { // drop the last fields until it fits (body, log, what, ...)
            StringBuilder url=new StringBuilder(ISSUES);
            for (int i=0; i<count; i++) {
                if (fields[i][1]==null) continue;
                url.append(i==0 ? '?' : '&').append(fields[i][0]).append('=').append(encode(fields[i][1]));
            }
            if (url.length()<=URL_LIMIT) return url.toString();
        }
        return ISSUES+"?template=bug_report.yml";
    }
    private static String encode(String s) {
        try { return URLEncoder.encode(s,"UTF-8").replace("+","%20"); }
        catch (UnsupportedEncodingException e) { throw new IllegalStateException(e); }
    }
}
