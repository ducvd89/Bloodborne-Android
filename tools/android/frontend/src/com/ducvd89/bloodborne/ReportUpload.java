package com.ducvd89.bloodborne;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.UUID;

/** Uploads the report zip to an anonymous file host so the GitHub issue can link it (a new-issue URL cannot carry a
 *  file). gofile.io first (reachable where catbox.moe and 0x0.st are blocked, e.g. in Vietnam), then catbox.moe, then
 *  0x0.st. Blocks: runs off the UI thread. Streams the file, never loads it. */
final class ReportUpload {
    interface Progress { void update(long sent,long total); }

    static final class Cancelled extends IOException { Cancelled() { super("Cancelled"); } }

    /** Lets another thread abort a running {@link #upload}. */
    static final class Job {
        private volatile boolean cancelled;
        private volatile HttpURLConnection conn;
        boolean cancelled() { return cancelled; }
        /** Safe from the UI thread: the socket is closed on a helper thread. */
        void cancel() {
            cancelled=true;
            HttpURLConnection c=conn;
            if (c!=null) new Thread(c::disconnect,"report-upload-cancel").start();
        }
    }

    private static final class Host {
        final String name, endpoint, field, prefix; final String[][] extra;
        /** The answer is JSON ({"status":"ok","data":{"downloadPage":link}}, gofile.io) rather than the bare link. */
        final boolean json;
        Host(String name,String endpoint,String field,String prefix,String[][] extra) { this(name,endpoint,field,prefix,extra,false); }
        Host(String name,String endpoint,String field,String prefix,String[][] extra,boolean json) {
            this.name=name; this.endpoint=endpoint; this.field=field; this.prefix=prefix; this.extra=extra; this.json=json;
        }
    }
    private static final Host[] HOSTS={
        new Host("gofile.io","https://upload.gofile.io/uploadfile","file","https://gofile.io/d/",new String[0][],true),
        new Host("catbox.moe","https://catbox.moe/user/api.php","fileToUpload","https://files.catbox.moe/",new String[][]{{"reqtype","fileupload"}}),
        new Host("0x0.st","https://0x0.st","file","https://0x0.st/",new String[0][]),
    };
    static final int CONNECT_MS=30000, READ_MS=120000;
    private ReportUpload() {}

    /** The link to the uploaded zip. Throws {@link Cancelled} when the job was cancelled, else an IOException that
     *  names what each host answered. */
    static String upload(File zip,String userAgent,Job job,Progress progress) throws IOException {
        StringBuilder why=new StringBuilder();
        for (Host h:HOSTS) {
            if (job.cancelled) throw new Cancelled();
            try { return upload(h,zip,userAgent,job,progress); }
            catch (IOException ex) {
                if (job.cancelled) throw new Cancelled();
                android.util.Log.w("Bloodborne","report upload to "+h.name+": "+ex);
                if (why.length()>0) why.append('\n');
                why.append(h.name).append(": ").append(ex.getMessage()==null ? ex.toString() : ex.getMessage());
            }
        }
        throw new IOException(why.toString());
    }

    private static String upload(Host h,File zip,String userAgent,Job job,Progress progress) throws IOException {
        String boundary="----BloodborneReport"+UUID.randomUUID().toString().replace("-","");
        StringBuilder head=new StringBuilder();
        for (String[] kv:h.extra) {
            head.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"").append(kv[0])
                .append("\"\r\n\r\n").append(kv[1]).append("\r\n");
        }
        String name=zip.getName().replaceAll("[^A-Za-z0-9._-]","_");
        head.append("--").append(boundary).append("\r\nContent-Disposition: form-data; name=\"").append(h.field)
            .append("\"; filename=\"").append(name).append("\"\r\nContent-Type: application/zip\r\n\r\n");
        byte[] pre=head.toString().getBytes(StandardCharsets.UTF_8);
        byte[] post=("\r\n--"+boundary+"--\r\n").getBytes(StandardCharsets.UTF_8);
        long size=zip.length();

        HttpURLConnection c=(HttpURLConnection)new URL(h.endpoint).openConnection();
        job.conn=c;
        try {
            if (job.cancelled) throw new Cancelled();
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setConnectTimeout(CONNECT_MS);
            c.setReadTimeout(READ_MS);
            c.setUseCaches(false);
            c.setRequestProperty("User-Agent",userAgent);
            c.setRequestProperty("Accept","*/*");
            c.setRequestProperty("Content-Type","multipart/form-data; boundary="+boundary);
            c.setFixedLengthStreamingMode(pre.length+size+post.length);
            try (OutputStream out=c.getOutputStream(); InputStream in=new FileInputStream(zip)) {
                out.write(pre);
                byte[] buf=new byte[64*1024];
                long sent=0, lastReport=0; int n;
                progress.update(0,size);
                while ((n=in.read(buf))>0) {
                    if (job.cancelled) throw new Cancelled();
                    out.write(buf,0,n);
                    sent+=n;
                    if (sent-lastReport>=256*1024 || sent==size) { lastReport=sent; progress.update(sent,size); }
                }
                out.write(post);
                out.flush();
            }
            int code=c.getResponseCode();
            InputStream body=code>=400 ? c.getErrorStream() : c.getInputStream();
            String text=body==null ? "" : read(body).trim();
            if (code<200 || code>=300) throw new IOException("HTTP "+code+(text.isEmpty() ? "" : " "+clip(text)));
            if (h.json) {
                try {
                    org.json.JSONObject o=new org.json.JSONObject(text);
                    org.json.JSONObject data=o.optJSONObject("data");
                    if (!"ok".equals(o.optString("status")) || data==null) throw new IOException("unexpected answer: "+clip(text));
                    text=data.optString("downloadPage","").trim();
                } catch (org.json.JSONException ex) { throw new IOException("unexpected answer: "+clip(text)); }
            }
            if (!text.startsWith(h.prefix) || text.indexOf('\n')>=0 || text.indexOf(' ')>=0) {
                throw new IOException("unexpected answer: "+clip(text));
            }
            return text;
        } finally {
            job.conn=null;
            c.disconnect();
        }
    }

    private static String read(InputStream in) throws IOException {
        try (InputStream s=in) {
            ByteArrayOutputStream bo=new ByteArrayOutputStream();
            byte[] buf=new byte[2048]; int n;
            while ((n=s.read(buf))>0 && bo.size()<4096) bo.write(buf,0,n);
            return bo.toString("UTF-8");
        }
    }
    private static String clip(String s) {
        s=s.replaceAll("\\s+"," ");
        return s.length()>120 ? s.substring(0,120)+"…" : s;
    }
    static String megabytes(long bytes) { return String.format(Locale.US,"%.1f",bytes/1048576.0); }
}
