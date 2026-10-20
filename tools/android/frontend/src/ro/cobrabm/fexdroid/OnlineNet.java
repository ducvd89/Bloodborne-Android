package ro.cobrabm.fexdroid;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/** The network side of the Online menu, ported from bloodborne_pc's scripts/online.py (GPL-2.0-or-later): the connection
 *  checks of both pools and The Hunter's Dream's device-code sign-in (bbhost's protocol, GPL-3.0-or-later).
 *  Blocking calls: run them off the UI thread. No Android classes. Nothing here logs a token or a password. */
final class OnlineNet {
    private OnlineNet() {}

    static final class NetException extends Exception { NetException(String m) { super(m); } }

    /** One line of the connection test. */
    static final class Check {
        final String name, address, detail; final boolean ok;
        /** The server answered, but with a valid "no" (neither a success nor a failure to reach it). */
        final boolean note;
        Check(String name,String address,boolean ok,String detail) { this(name,address,ok,false,detail); }
        Check(String name,String address,boolean ok,boolean note,String detail) { this.name=name; this.address=address; this.ok=ok; this.note=note; this.detail=detail; }
    }

    private static String reason(Exception e) {
        String m=e.getMessage();
        return m==null || m.isEmpty() ? e.getClass().getSimpleName() : m;
    }
    private static byte[] readUpTo(InputStream in,int max) throws IOException {
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        byte[] buffer=new byte[1024]; int n;
        while (out.size()<max && (n=in.read(buffer,0,Math.min(buffer.length,max-out.size())))>0) out.write(buffer,0,n);
        return out.toByteArray();
    }
    /** An HTTP answer: the status code and the first bytes of the body. */
    static final class Reply {
        final int code; final byte[] body;
        Reply(int code,byte[] body) { this.code=code; this.body=body; }
        String text() { return new String(body,StandardCharsets.UTF_8); }
    }

    /** One HTTP request, never following a redirect. http:// goes over a plain Socket (the servers of the Online menu speak
     *  plain http, which Android's cleartext policy blocks for HttpURLConnection, whatever host the player enters);
     *  https:// goes through HttpURLConnection. The body (up to maxBody bytes) is returned with the status, also for 4xx/5xx. */
    static Reply request(String url,String method,String contentType,byte[] body,int timeoutMs,int maxBody) throws IOException {
        if (url.regionMatches(true,0,"http://",0,7)) return rawRequest(url,method,contentType,body,timeoutMs,maxBody);
        HttpURLConnection c=(HttpURLConnection)new URL(url).openConnection();
        try {
            c.setRequestMethod(method); c.setConnectTimeout(timeoutMs); c.setReadTimeout(timeoutMs);
            c.setInstanceFollowRedirects(false);
            c.setRequestProperty("User-Agent","bbport-android");
            if (body!=null) {
                c.setDoOutput(true);
                if (contentType!=null) c.setRequestProperty("Content-Type",contentType);
                c.setFixedLengthStreamingMode(body.length);
                try (OutputStream out=c.getOutputStream()) { out.write(body); }
            }
            int code=c.getResponseCode();
            InputStream in=code>=400 ? c.getErrorStream() : c.getInputStream();
            return new Reply(code,in==null ? new byte[0] : readUpTo(in,maxBody));
        } finally { c.disconnect(); }
    }

    /** A minimal HTTP/1.1 exchange (Connection: close, Content-Length or chunked or read-to-close bodies). */
    private static Reply rawRequest(String url,String method,String contentType,byte[] body,int timeoutMs,int maxBody) throws IOException {
        java.net.URI uri;
        try { uri=new java.net.URI(url); } catch (java.net.URISyntaxException e) { throw new IOException("bad address "+url); }
        String host=uri.getHost();
        if (host==null) throw new IOException("bad address "+url);
        int port=uri.getPort()<0 ? 80 : uri.getPort();
        String path=uri.getRawPath()==null || uri.getRawPath().isEmpty() ? "/" : uri.getRawPath();
        if (uri.getRawQuery()!=null) path+="?"+uri.getRawQuery();
        StringBuilder head=new StringBuilder(method).append(' ').append(path).append(" HTTP/1.1\r\nHost: ")
            .append(host.indexOf(':')>=0 ? "["+host+"]" : host).append(port==80 ? "" : ":"+port)
            .append("\r\nUser-Agent: bbport-android\r\nAccept: */*\r\nConnection: close\r\n");
        if (body!=null) {
            if (contentType!=null) head.append("Content-Type: ").append(contentType).append("\r\n");
            head.append("Content-Length: ").append(body.length).append("\r\n");
        } else if (method.equals("POST") || method.equals("PUT")) head.append("Content-Length: 0\r\n");
        head.append("\r\n");
        try (Socket s=new Socket()) {
            s.connect(new InetSocketAddress(host,port),timeoutMs);
            s.setSoTimeout(timeoutMs);
            OutputStream out=s.getOutputStream();
            out.write(head.toString().getBytes(StandardCharsets.ISO_8859_1));
            if (body!=null) out.write(body);
            out.flush();
            InputStream in=new java.io.BufferedInputStream(s.getInputStream(),4096);
            String status=readLine(in);
            if (status==null || !status.startsWith("HTTP/")) throw new IOException("not an HTTP server");
            String[] parts=status.split(" ",3);
            int code;
            try { code=Integer.parseInt(parts[1].trim()); } catch (RuntimeException e) { throw new IOException("not an HTTP server"); }
            long length=-1; boolean chunked=false;
            for (String line; (line=readLine(in))!=null && !line.isEmpty();) {
                int colon=line.indexOf(':');
                if (colon<0) continue;
                String name=line.substring(0,colon).trim().toLowerCase(java.util.Locale.ROOT), value=line.substring(colon+1).trim();
                if (name.equals("content-length")) { try { length=Long.parseLong(value); } catch (NumberFormatException e) { length=-1; } }
                else if (name.equals("transfer-encoding") && value.toLowerCase(java.util.Locale.ROOT).contains("chunked")) chunked=true;
            }
            if (method.equals("HEAD") || code==204 || code==304 || code/100==1) return new Reply(code,new byte[0]);
            ByteArrayOutputStream data=new ByteArrayOutputStream();
            if (chunked) {
                while (data.size()<maxBody) {
                    String size=readLine(in);
                    if (size==null) break;
                    int semi=size.indexOf(';');
                    long n;
                    try { n=Long.parseLong((semi<0 ? size : size.substring(0,semi)).trim(),16); } catch (NumberFormatException e) { break; }
                    if (n<=0) break;
                    byte[] buffer=new byte[1024];
                    while (n>0) {
                        int got=in.read(buffer,0,(int)Math.min(buffer.length,n));
                        if (got<0) { n=0; break; }
                        n-=got;
                        if (data.size()<maxBody) data.write(buffer,0,Math.min(got,maxBody-data.size()));
                    }
                    if (n!=0) break;
                    readLine(in); // the CRLF after the chunk
                }
            } else {
                long want=length<0 ? maxBody : Math.min(length,maxBody);
                byte[] buffer=new byte[1024];
                while (data.size()<want) {
                    int got=in.read(buffer,0,(int)Math.min(buffer.length,want-data.size()));
                    if (got<0) break;
                    data.write(buffer,0,got);
                }
            }
            return new Reply(code,data.toByteArray());
        }
    }
    private static String readLine(InputStream in) throws IOException {
        StringBuilder b=new StringBuilder();
        int c;
        while ((c=in.read())>=0) {
            if (c=='\n') break;
            if (c!='\r') b.append((char)c);
            if (b.length()>8192) throw new IOException("header too long");
        }
        return c<0 && b.length()==0 ? null : b.toString();
    }


    // ---- the Seamless Hunt pool ----

    /** True when a shadNet WebAPI answers Bloodborne's summon calls: the route exists there (an empty request is refused
     *  with 400), where other servers answer 404. */
    static boolean summonService(String webapi) { return summonStatus(webapi)==SUMMON_YES; }
    static final int SUMMON_UNREACHABLE=-1, SUMMON_NO=0, SUMMON_YES=1;
    /** SUMMON_YES when the route exists, SUMMON_NO when the server answered that it does not (404), SUMMON_UNREACHABLE
     *  when it did not answer at all. */
    static int summonStatus(String webapi) {
        try {
            Reply r=request(OnlineNet.trim(webapi)+"/summon_messenger/create","POST",null,new byte[0],4000,64);
            return r.code==404 ? SUMMON_NO : SUMMON_YES;
        } catch (IOException | RuntimeException e) { return SUMMON_UNREACHABLE; }
    }
    private static String trim(String webapi) {
        while (webapi.endsWith("/")) webapi=webapi.substring(0,webapi.length()-1);
        return webapi;
    }

    /** The game server, the shadNet server, its WebAPI and its summon service (online.py check). */
    static List<Check> checkSeamless(String server,String webapi,String community) {
        List<Check> results=new ArrayList<>();
        results.add(httpCheck("Game server",community,community+":20443/bb-eu/ss.info",6000,false));
        String host=server, port="31313";
        int colon=server.lastIndexOf(':');
        if (colon>=0) { host=server.substring(0,colon); port=server.substring(colon+1); }
        try (Socket s=new Socket()) {
            s.connect(new InetSocketAddress(host.isEmpty() ? server : host,Integer.parseInt(port.isEmpty() ? "31313" : port)),4000);
            results.add(new Check("shadNet server",server,true,""));
        } catch (IOException | RuntimeException e) { results.add(new Check("shadNet server",server,false,reason(e))); }
        try {
            Reply r=request(trim(webapi)+"/status","GET",null,null,4000,512);
            if (r.code>=400) results.add(new Check("WebAPI",webapi,false,"HTTP "+r.code));
            else {
                boolean ok=r.text().replace(" ","").contains("\"ok\":true");
                results.add(new Check("WebAPI",webapi,ok,ok ? "" : "not a shadNet server"));
            }
        } catch (IOException | RuntimeException e) { results.add(new Check("WebAPI",webapi,false,reason(e))); }
        int summons=summonStatus(webapi);
        results.add(summons==SUMMON_YES ? new Check("Summon service",webapi,true,"")
            : summons==SUMMON_NO ? new Check("Summon service",webapi,false,true,"the server answered: no summon service there, summon signs go through the game server")
            : new Check("Summon service",webapi,false,"no answer"));
        return results;
    }

    /** A GET that counts as a success when it is answered with 2xx/3xx (or, with anyAnswer, with any HTTP status). */
    private static Check httpCheck(String name,String shown,String url,int timeoutMs,boolean anyAnswer) {
        try {
            int code=request(url,"GET",null,null,timeoutMs,64).code;
            if (code>=400 && !anyAnswer) return new Check(name,shown,false,"HTTP "+code);
            return new Check(name,shown,true,"");
        } catch (IOException | RuntimeException e) { return new Check(name,shown,false,reason(e)); }
    }

    // ---- The Hunter's Dream ----

    /** The game server, its co-op service and the account service (online.py check_thd); an HTTP error status means the
     *  server answered. */
    static List<Check> checkThd(String community) {
        String base=OnlineAddr.thdBase(community);
        List<Check> results=new ArrayList<>();
        results.add(httpCheck("Game server",hostPort(base+":20443"),base+":20443/bb-eu/ss.info",6000,true));
        results.add(httpCheck("Co-op service",hostPort(base+":"+OnlineAddr.COMMUNITY_API_PORT),base+":"+OnlineAddr.COMMUNITY_API_PORT+"/",6000,true));
        results.add(httpCheck("Account service",hostPort(base),base+"/",6000,true));
        return results;
    }
    private static String hostPort(String url) { return url.substring(url.indexOf("://")+3); }

    /** A call to The Hunter's Dream's account service (bbhost's): its reply, or NetException with the server's reason. */
    @SuppressWarnings("unchecked")
    static Map<String,Object> thdCall(String url,String jsonBody) throws NetException {
        byte[] text;
        try { text=request(url,"POST","application/json",jsonBody.getBytes(StandardCharsets.UTF_8),10000,65536).body; }
        catch (IOException | RuntimeException e) { throw new NetException(url+": "+reason(e)); }
        Object reply;
        try { reply=MiniJson.parse(new String(text,StandardCharsets.UTF_8)); }
        catch (Exception e) { reply=null; }
        if (!(reply instanceof Map) || !((Map<String,Object>)reply).containsKey("ResKind")) throw new NetException(url+" has no account service");
        Map<String,Object> map=(Map<String,Object>)reply;
        Object kind=map.get("ResKind");
        if (!(kind instanceof Number) || ((Number)kind).longValue()!=0) {
            Object message=map.get("Message");
            throw new NetException(message instanceof String && !((String)message).isEmpty() ? (String)message : "refused");
        }
        return map;
    }
    private static String str(Map<String,Object> m,String key) { Object v=m.get(key); return v instanceof String ? (String)v : ""; }
    private static long num(Map<String,Object> m,String key) { Object v=m.get(key); return v instanceof Number ? ((Number)v).longValue() : 0; }
    private static String jsonString(String s) {
        StringBuilder b=new StringBuilder("\"");
        for (int i=0;i<s.length();++i) { char c=s.charAt(i); if (c=='"' || c=='\\') b.append('\\'); if (c>=0x20) b.append(c); }
        return b.append('"').toString();
    }

    interface CodeListener { void code(String page,String code); }
    interface Cancel { boolean cancelled(); }

    /** Discord sign-in the way bbhost does it: a code to approve on the website (listener shows it). Returns {online id,
     *  token}; throws NetException ("cancelled" when the player cancelled). */
    static String[] thdSignIn(String server,String device,CodeListener listener,Cancel cancel) throws NetException {
        String auth=OnlineAddr.thdBase(server);
        Map<String,Object> start=thdCall(auth+"/auth/device/start","{\"DeviceName\":"+jsonString(device)+"}");
        String deviceCode=str(start,"DeviceCode"), userCode=str(start,"UserCode");
        if (deviceCode.isEmpty() || userCode.isEmpty()) throw new NetException("no code in the reply");
        long interval=Math.max(1,num(start,"Interval")==0 ? 5 : num(start,"Interval"));
        long expires=num(start,"ExpiresIn")==0 ? 600 : num(start,"ExpiresIn");
        long deadline=System.nanoTime()+expires*1000000000L;
        String page=str(start,"VerificationUri");
        listener.code(page.isEmpty() ? auth+"/link" : page,userCode);
        while (System.nanoTime()<deadline) {
            for (long i=0;i<interval*4;++i) { // a cancel is noticed within a quarter of a second
                if (cancel.cancelled()) throw new NetException("cancelled");
                try { Thread.sleep(250); } catch (InterruptedException e) { throw new NetException("cancelled"); }
            }
            Map<String,Object> poll=thdCall(auth+"/auth/device/poll","{\"DeviceCode\":"+jsonString(deviceCode)+"}");
            String status=str(poll,"Status");
            if (status.equals("approved")) {
                String id=str(poll,"OnlineId"), token=str(poll,"Token");
                if (id.isEmpty() || token.isEmpty()) throw new NetException("no token in the reply");
                return new String[]{id,token};
            }
            if (status.equals("slow_down")) interval+=5;
            else if (status.equals("denied")) throw new NetException("the sign-in was denied");
            else if (status.equals("expired")) break;
        }
        throw new NetException("the code expired; try again");
    }
}
