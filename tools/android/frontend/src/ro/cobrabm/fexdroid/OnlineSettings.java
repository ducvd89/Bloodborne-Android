package ro.cobrabm.fexdroid;

import android.content.Context;
import android.content.SharedPreferences;
import java.io.File;
import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Online play: the player's choices and what they become for the game process.
 *
 *  Everything lives in the app's private preferences (shared_prefs/bloodborne_online.xml, readable by this app only;
 *  allowBackup is off), never in bbport.ini, the logs or the problem report. The account's password (Seamless Hunt)
 *  and the token (The Hunter's Dream) reach the game only through its process environment (MainActivity.launchScript),
 *  never through a file; scrub() takes them out of anything the app copies from the game's output.
 *  The addresses and the environment follow bloodborne_pc 0.6 (launcher/bbport_launcher.py game_environment, run.sh,
 *  scripts/online.py; GPL-2.0-or-later). */
final class OnlineSettings {
    static final String POOL_SEAMLESS="seamless", POOL_THD="thd";
    /** SHADPS4_P2P_RELAY: the module's default (unset) sends every packet through the server's relay; "auto" tries direct
     *  first and the relay after 2 s; "0" has no relay. */
    static final String RELAY_SERVER="", RELAY_AUTO="auto", RELAY_OFF="0";

    private static final String PREFS="bloodborne_online";
    private static final String K_ON="online", K_POOL="pool", K_NPID="npid", K_PASSWORD="password", K_SERVER="server",
        K_WEBAPI="webapi", K_COMMUNITY="community", K_P2P_PORT="p2p_port", K_UPNP="upnp", K_BBSC="bbsc", K_RELAY="relay",
        K_THD_SERVER="thd_server", K_THD_ID="thd_online_id", K_THD_TOKEN="thd_token", K_LOG="detailed_log", K_DEVICE="device_id";

    /** The literals scrub() removes (the password and the token); written whole, read by other threads. */
    private static volatile String[] secrets=new String[0];
    /** What scrub() puts in their place. */
    private static final String MASK="***";

    private final SharedPreferences prefs;
    /** The environment the running game was started with (null: no game, or it started offline), without the files' paths. */
    private volatile Map<String,String> launched;

    OnlineSettings(Context context) {
        prefs=context.getSharedPreferences(PREFS,Context.MODE_PRIVATE);
        refreshSecrets();
    }

    // ---- the choices ----
    boolean enabled() { return prefs.getBoolean(K_ON,false); }
    void setEnabled(boolean on) { prefs.edit().putBoolean(K_ON,on).apply(); }
    String pool() { return POOL_THD.equals(prefs.getString(K_POOL,POOL_SEAMLESS)) ? POOL_THD : POOL_SEAMLESS; }
    void setPool(String pool) { prefs.edit().putString(K_POOL,POOL_THD.equals(pool) ? POOL_THD : POOL_SEAMLESS).apply(); }
    String npid() { return prefs.getString(K_NPID,""); }
    String password() { return prefs.getString(K_PASSWORD,""); }
    /** Empty means the default of the pool. */
    String server() { return prefs.getString(K_SERVER,""); }
    String webapi() { return prefs.getString(K_WEBAPI,""); }
    String community() { return prefs.getString(K_COMMUNITY,""); }
    String thdServer() { return prefs.getString(K_THD_SERVER,""); }
    /** The UDP port of peer to peer traffic: empty is the module's default (3658, the PS4's); 0 is any free port. */
    String p2pPort() { return prefs.getString(K_P2P_PORT,""); }
    boolean upnp() { return prefs.getBoolean(K_UPNP,false); }
    /** "Detailed online log": the online module logs at info (BB_GPU_LOG=info, set by run-bionic.sh) instead of warning. */
    boolean detailedLog() { return prefs.getBoolean(K_LOG,false); }
    void setDetailedLog(boolean on) { prefs.edit().putBoolean(K_LOG,on).apply(); }
    /** A random id made once per install (BB_NET_DEVICE_ID): the module derives a fake MAC address from it, as Android
     *  does not let an app read the real one. */
    String deviceId() {
        String id=prefs.getString(K_DEVICE,"");
        if (id.isEmpty()) { id=java.util.UUID.randomUUID().toString(); prefs.edit().putString(K_DEVICE,id).apply(); }
        return id;
    }
    boolean bbsc() { return prefs.getBoolean(K_BBSC,false); }
    String relay() { String v=prefs.getString(K_RELAY,RELAY_SERVER); return v.equals(RELAY_AUTO) || v.equals(RELAY_OFF) ? v : RELAY_SERVER; }
    String thdOnlineId() { return prefs.getString(K_THD_ID,""); }
    boolean thdSignedIn() { return !thdOnlineId().isEmpty() && !prefs.getString(K_THD_TOKEN,"").isEmpty(); }

    /** Seamless Hunt's account and the advanced fields, saved together. */
    void saveSeamless(String npid,String password,String server,String webapi,String community) {
        prefs.edit().putString(K_NPID,npid.trim()).putString(K_PASSWORD,password)
            .putString(K_SERVER,server.trim()).putString(K_WEBAPI,webapi.trim()).putString(K_COMMUNITY,community.trim()).apply();
        refreshSecrets();
    }
    void saveCommon(String thdServer,String p2pPort,boolean upnp,boolean bbsc,String relay) {
        prefs.edit().putString(K_THD_SERVER,thdServer.trim()).putString(K_P2P_PORT,validPort(p2pPort) ? p2pPort.trim() : "")
            .putBoolean(K_UPNP,upnp).putBoolean(K_BBSC,bbsc).putString(K_RELAY,relay).apply();
    }
    void saveThdAccount(String onlineId,String token) {
        prefs.edit().putString(K_THD_ID,onlineId).putString(K_THD_TOKEN,token).apply();
        refreshSecrets();
    }
    void signOutThd() { saveThdAccount("",""); }

    static boolean validPort(String port) {
        port=port==null ? "" : port.trim();
        if (port.isEmpty() || port.length()>5) return false;
        for (int i=0;i<port.length();++i) if (port.charAt(i)<'0' || port.charAt(i)>'9') return false;
        return Integer.parseInt(port)<=65535;
    }

    // ---- what it comes to ----

    /** The account of the chosen pool is there (and looks right). */
    boolean hasCredentials() {
        if (pool().equals(POOL_THD)) return thdSignedIn();
        String npid=npid().trim();
        return !npid.isEmpty() && npid.indexOf('@')<0 && !password().isEmpty();
    }
    /** The game will start online: the switch is on and the chosen pool has its account. Cheats are off then. */
    boolean playingOnline() { return enabled() && hasCredentials(); }

    /** The drawer row's second line. */
    String summary() {
        String poolName=pool().equals(POOL_THD) ? "The Hunter's Dream" : "Seamless Hunt";
        if (!enabled()) return "Off";
        if (hasCredentials()) return "On · "+poolName;
        if (pool().equals(POOL_SEAMLESS) && npid().indexOf('@')>=0) return "On · Online ID is not an email";
        return "On · "+poolName+" · "+(pool().equals(POOL_THD) ? "sign in needed" : "account needed");
    }

    /** The environment variables of the online game (empty when it plays offline), without the host overrides' path
     *  (a file, written at launch). BB_NET_MODULE is run-bionic.sh's (it knows where the runtime is). */
    Map<String,String> env(File base) {
        Map<String,String> env=new LinkedHashMap<>();
        if (!playingOnline()) return env;
        boolean thd=pool().equals(POOL_THD);
        env.put("BB_ONLINE","1");
        env.put("BB_NET_GEN","20");
        env.put("BB_NET_BANNER_FILE",new File(base,"net-banner.txt").getPath());
        env.put("BB_NET_STATUS_FILE",OnlineStatus.file(base).getPath());
        env.put("BB_UPNP",upnp() ? "1" : "0");
        env.put("BB_NET_DEVICE_ID",deviceId());
        if (detailedLog()) env.put("BB_ONLINE_LOG","1");
        if (validPort(p2pPort())) env.put("BB_P2P_PORT",p2pPort().trim());
        if (!relay().isEmpty()) env.put("SHADPS4_P2P_RELAY",relay());
        if (thd) {
            String address=OnlineAddr.thdBase(thdServer());
            env.put("BB_NET_BACKEND","thd");
            env.put("BB_COMMUNITY_SERVER",address);
            env.put("BB_THD_SERVER",address+":"+OnlineAddr.COMMUNITY_API_PORT);
            env.put("BB_THD_AUTH",address);
            env.put("BB_THD_RULESET","vanilla");
            env.put("BB_THD_VERIFY_TLS","1");
            env.put("BB_THD_ONLINE_ID",thdOnlineId());
            env.put("BB_USER_NAME",thdOnlineId());
            env.put("BB_THD_TOKEN",prefs.getString(K_THD_TOKEN,""));
        } else {
            String server=server().trim().isEmpty() ? OnlineAddr.SHADNET_SERVER : server().trim();
            String community=OnlineAddr.communityAddress(community());
            env.put("BB_COMMUNITY_SERVER",community);
            env.put("BB_SHADNET_SERVER",server);
            env.put("BB_SHADNET_WEBAPI",OnlineAddr.webapiSetting(webapi(),server,community));
            env.put("BB_SHADNET_NPID",npid().trim());
            env.put("BB_USER_NAME",npid().trim());
            env.put("BB_SHADNET_PASSWORD",password());
            if (bbsc()) env.put("BB_BBSC","1");
        }
        return env;
    }

    /** Before the game starts (off the UI thread: the Seamless Hunt pool asks its WebAPI whether it has the summon
     *  service): adds the online variables and the host overrides' path to the game's environment, records what the
     *  game was started with, and returns a notice for the player when online was asked for but cannot start (else
     *  null). Nothing secret is logged: `log` gets plain lines only. */
    String prepareLaunch(List<String> extra,File base,List<String> log) {
        launched=null;
        File banner=new File(base,"net-banner.txt");
        banner.delete();
        OnlineStatus.file(base).delete(); // the last game's state is not this one's
        if (!enabled()) return null;
        if (!hasCredentials()) {
            String why=pool().equals(POOL_THD) ? "sign in to The Hunter's Dream first"
                : npid().indexOf('@')>=0 ? "the Online ID is the account name, not an email address" : "enter the Seamless Hunt Online ID and password first";
            log.add("Online: starting offline ("+why+")");
            return "Online play is on, but "+why+". Starting offline.";
        }
        Map<String,String> env=env(base);
        boolean thd=pool().equals(POOL_THD);
        File user=new File(base,"user");
        File overrides=new File(user,"host_overrides.json"), custom=new File(user,"host_overrides.custom.json");
        try {
            user.mkdirs();
            if (custom.isFile()) {
                overrides=custom;
                log.add("Online: addresses from "+custom.getName());
            } else if (thd) {
                SafeWrite.write(overrides,OnlineAddr.thdOverrides(OnlineAddr.thdBase(thdServer())));
            } else {
                String webapi=env.get("BB_SHADNET_WEBAPI");
                int status=OnlineNet.summonStatus(webapi);
                boolean summons=status==OnlineNet.SUMMON_YES;
                log.add(summons ? "Online: summon signs through "+webapi
                    : "Online: "+webapi+(status==OnlineNet.SUMMON_NO ? " has no summon service" : " did not answer the summon probe")
                        +"; summon signs through "+env.get("BB_COMMUNITY_SERVER"));
                SafeWrite.write(overrides,OnlineAddr.seamlessOverrides(env.get("BB_COMMUNITY_SERVER"),webapi,summons));
            }
            env.put("SHADPS4_HTTP_HOST_OVERRIDES_JSON",overrides.getPath());
        } catch (IOException e) {
            log.add("Online: starting offline (the host overrides could not be written: "+e.getMessage()+")");
            return "Online play could not start: "+e.getMessage();
        }
        log.add("Online: "+(thd ? "The Hunter's Dream "+env.get("BB_COMMUNITY_SERVER")
            : "Seamless Hunt, shadNet "+env.get("BB_SHADNET_SERVER")+", game server "+env.get("BB_COMMUNITY_SERVER")
              +(bbsc() ? ", seamless co-op (BBSC)" : ""))+", cheats off");
        for (Map.Entry<String,String> e:env.entrySet()) extra.add(e.getKey()+"="+e.getValue());
        env.remove("SHADPS4_HTTP_HOST_OVERRIDES_JSON");
        launched=env;
        return null;
    }
    /** The game that runs now was started online. */
    boolean launchedOnline() { return launched!=null; }
    /** The game runs with other online settings than the saved ones: a restart applies them. */
    boolean differsFromLaunch(File base) {
        Map<String,String> now=env(base), then=launched;
        return !now.equals(then==null ? new LinkedHashMap<String,String>() : then);
    }

    // ---- cheats ----

    /** Writes cheats.state (the running game follows it): "00" while the game plays or will play online, else the
     *  player's choice from bbport.ini, unless the running game started online (its cheats stay off until a restart
     *  starts it offline; run-bionic.sh writes the file at every start). */
    void syncCheatState(File base) {
        try {
            File state=new File(base,"cheats.state");
            if (playingOnline() || launchedOnline()) { SafeWrite.write(state,"00\n"); return; }
            String health="0", items="0";
            for (String line:SafeWrite.readLines(new File(base,"bbport.ini"))) {
                if (line.startsWith("cheat_health=")) health=line.substring(13).trim();
                else if (line.startsWith("cheat_items=")) items=line.substring(12).trim();
            }
            SafeWrite.write(state,("1".equals(health) ? "1" : "0")+("1".equals(items) ? "1" : "0")+"\n");
        } catch (IOException e) { android.util.Log.w("Bloodborne","cheats.state: "+e); }
    }

    // ---- secrets kept out of logs ----

    private void refreshSecrets() {
        java.util.ArrayList<String> list=new java.util.ArrayList<>();
        for (String s:new String[]{prefs.getString(K_PASSWORD,""),prefs.getString(K_THD_TOKEN,"")}) if (s.length()>=4) list.add(s);
        secrets=list.toArray(new String[0]);
    }
    /** The line without the account's password or token, wherever they appear. */
    static String scrub(String line) {
        for (String s:secrets) if (line.contains(s)) line=line.replace(s,MASK);
        return line;
    }
}
