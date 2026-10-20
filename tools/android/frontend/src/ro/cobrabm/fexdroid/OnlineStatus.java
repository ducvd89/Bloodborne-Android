package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;

/** What the online session of the running game is doing: the line `state|detail` the game's online module rewrites in
 *  BB_NET_STATUS_FILE (files/bbport/net-status.txt) on every change. state is connecting, online, offline, rejected or
 *  error; detail is the module's text for the player ("Signed in to Seamless Hunt as X", "Disconnected — reconnecting",
 *  "Server rejected this client: ...", "Login failed: ..."). No Android classes: tested on a desktop JDK. */
final class OnlineStatus {
    static final int CONNECTING=0, ONLINE=1, OFFLINE=2, REJECTED=3, ERROR=4;
    static final int GREEN=0xff5fcf6f, AMBER=0xffe8b04a, RED=0xffe0584a;

    final int state;
    final String detail;

    OnlineStatus(int state,String detail) { this.state=state; this.detail=detail==null ? "" : detail; }

    static File file(File base) { return new File(base,"net-status.txt"); }

    /** The file's line, or null when it is missing, empty or not a status line. */
    static OnlineStatus read(File file) {
        try (FileInputStream in=new FileInputStream(file)) {
            byte[] data=new byte[(int)Math.min(Math.max(file.length(),0),4096)];
            int n=0; for (int r; n<data.length && (r=in.read(data,n,data.length-n))>0;) n+=r;
            return parse(new String(data,0,n,StandardCharsets.UTF_8));
        } catch (IOException e) { return null; }
    }
    static OnlineStatus parse(String text) {
        if (text==null) return null;
        int end=text.indexOf('\n');
        String line=(end<0 ? text : text.substring(0,end)).replaceAll("[\\p{Cntrl}]"," ").trim();
        int bar=line.indexOf('|');
        if (bar<=0) return null;
        int state;
        switch (line.substring(0,bar)) {
            case "connecting": state=CONNECTING; break;
            case "online": state=ONLINE; break;
            case "offline": state=OFFLINE; break;
            case "rejected": state=REJECTED; break;
            case "error": state=ERROR; break;
            default: return null;
        }
        return new OnlineStatus(state,line.substring(bar+1).trim());
    }

    /** The badge's word. */
    String shortText() {
        switch (state) {
            case ONLINE: return "Online";
            case CONNECTING: return "Connecting…";
            case REJECTED: return "Rejected";
            case ERROR: return "Error";
            default: return "Offline";
        }
    }
    int color() { return state==ONLINE ? GREEN : state==CONNECTING ? AMBER : RED; }

    /** The drawer row's second line while the game runs online. */
    String menuLine() {
        switch (state) {
            case ONLINE: return detail.isEmpty() ? "Online" : detail;
            case CONNECTING: return "Connecting…";
            case OFFLINE: return "Offline: "+(detail.isEmpty() ? "no connection" : detail);
            default: return detail.isEmpty() ? shortText() : detail;
        }
    }

    boolean same(OnlineStatus o) { return o!=null && o.state==state && o.detail.equals(detail); }

    private static final Pattern ACCOUNT=Pattern.compile("(Signed in to .+? as ).+?( · |$)");
    /** The status for the problem report: the account name is taken out (the report never holds the account), and so are
     *  the password and token if they were in it. */
    static String forReport(OnlineStatus s,String npid,String thdId) {
        if (s==null) return "none (the game ran offline, or has not reported)";
        String text=ACCOUNT.matcher(s.detail).replaceFirst("$1<account>$2");
        for (String name:new String[]{npid,thdId}) if (name!=null && name.trim().length()>=3) text=text.replace(name.trim(),"<account>");
        return new String[]{"connecting","online","offline","rejected","error"}[s.state]+" | "+OnlineSettings.scrub(text);
    }
}
