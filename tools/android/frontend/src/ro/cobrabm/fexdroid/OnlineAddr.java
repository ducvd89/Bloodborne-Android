package ro.cobrabm.fexdroid;

/** The online addresses and the HTTP host overrides, ported from bloodborne_pc's scripts/online.py (GPL-2.0-or-later).
 *  No Android classes: built on a desktop JDK too. */
final class OnlineAddr {
    private OnlineAddr() {}

    /** Seamless Hunt (BBSC's public servers): shadNet, its WebAPI and the game server (plain http: BBSC's game server
     *  listens on 20443 without TLS). */
    static final String COMMUNITY_SERVER="http://play.seamlesshunt.com";
    static final String SHADNET_SERVER="play.seamlesshunt.com:31313";
    static final String WEBAPI_SERVER="http://play.seamlesshunt.com:31315";
    static final String SEAMLESS_SITE="https://seamlesshunt.com";
    /** Bloodborne's own game server, which the host overrides send to the community server. */
    static final String GAME_SERVER="https://ss4.scej-network.jp:20443";
    /** The game server's own ss.info names this port for its API (summons among them). */
    static final int COMMUNITY_API_PORT=18671;
    static final String HUNTERS_DREAM_SERVER="https://thehuntersdream.com";
    /** The play logs the game sends to FromSoftware's old storage buckets go to the game server's API. */
    static final String[] PLAY_LOG_HOSTS={"bb-playlog-prod.s3.amazonaws.com","bb-playlog-test.s3.amazonaws.com"};

    /** The game server as scheme://host (the game picks the ports), Seamless Hunt by default. Without a scheme: https,
     *  but http for the default server's host (it serves plain http). */
    static String communityAddress(String value) {
        value=value==null ? "" : value.trim();
        if (value.isEmpty()) value=COMMUNITY_SERVER;
        int sep=value.lastIndexOf("://");
        String scheme=sep>=0 ? value.substring(0,sep) : "";
        String rest=sep>=0 ? value.substring(sep+3) : value;
        String host=hostOf(rest);
        String defaultHost=COMMUNITY_SERVER.substring(COMMUNITY_SERVER.indexOf("://")+3);
        if (scheme.isEmpty()) scheme=host.equals(defaultHost) ? "http" : "https";
        return scheme+"://"+host;
    }
    /** "host" of "host:port/path". */
    private static String hostOf(String rest) {
        int slash=rest.indexOf('/');
        if (slash>=0) rest=rest.substring(0,slash);
        int colon=rest.indexOf(':');
        return colon>=0 ? rest.substring(0,colon) : rest;
    }
    private static int count(String s,char c) { int n=0; for (int i=0;i<s.length();++i) if (s.charAt(i)==c) ++n; return n; }

    /** The shadNet WebAPI of a server address host:port (the server's default port 31315). */
    static String webapiAddress(String server) {
        server=server==null ? "" : server.trim();
        if (server.isEmpty()) server=SHADNET_SERVER;
        String host=count(server,':')==1 ? server.substring(0,server.lastIndexOf(':')) : server;
        return "http://"+host+":31315";
    }
    /** The WebAPI field, unless it holds the game server's address (a common mix-up): then the shadNet server's own
     *  WebAPI. */
    static String webapiSetting(String webapi,String server,String community) {
        webapi=webapi==null ? "" : webapi.trim();
        String communityAddress=communityAddress(community);
        String communityHost=communityAddress.substring(communityAddress.indexOf("://")+3);
        if (!webapi.isEmpty() && webapi.contains(communityHost)) webapi="";
        return webapi.isEmpty() ? webapiAddress(server) : webapi;
    }
    /** The Hunter's Dream's address as scheme://host (https unless a scheme is given). */
    static String thdBase(String value) {
        value=value==null ? "" : value.trim();
        while (value.endsWith("/")) value=value.substring(0,value.length()-1);
        if (value.isEmpty()) value=HUNTERS_DREAM_SERVER;
        if (!value.contains("://")) value="https://"+value;
        int sep=value.indexOf("://");
        String scheme=value.substring(0,sep), rest=value.substring(sep+3);
        int slash=rest.indexOf('/');
        if (slash>=0) rest=rest.substring(0,slash);
        return scheme+"://"+rest;
    }

    private static String quote(String s) {
        StringBuilder b=new StringBuilder("\"");
        for (int i=0;i<s.length();++i) {
            char c=s.charAt(i);
            if (c=='"' || c=='\\') b.append('\\').append(c);
            else if (c<0x20) b.append(String.format("\\u%04x",(int)c));
            else b.append(c);
        }
        return b.append('"').toString();
    }
    private static String json(java.util.LinkedHashMap<String,String> map) {
        StringBuilder b=new StringBuilder("{\n");
        int n=0;
        for (java.util.Map.Entry<String,String> e:map.entrySet())
            b.append("  ").append(quote(e.getKey())).append(": ").append(quote(e.getValue())).append(++n<map.size() ? ",\n" : "\n");
        return b.append("}\n").toString();
    }

    /** Seamless Hunt's host overrides (online.py write_overrides): the game's server goes to the community server; the
     *  summon signs go to the shadNet WebAPI when it has the Bloodborne summon service (summonService), else they stay
     *  with the community server. */
    static String seamlessOverrides(String community,String webapi,boolean summonService) {
        community=communityAddress(community);
        String host=community.substring(community.indexOf("://")+3);
        java.util.LinkedHashMap<String,String> overrides=new java.util.LinkedHashMap<>();
        overrides.put(GAME_SERVER,community);
        if (summonService)
            for (String scheme:new String[]{"https","http"})
                overrides.put(scheme+"://"+host+":"+COMMUNITY_API_PORT+"/summon_messenger",webapi);
        return json(overrides);
    }
    /** The Hunter's Dream's host overrides (online.py write_thd_overrides): the game's servers and the play logs. */
    static String thdOverrides(String base) {
        java.util.LinkedHashMap<String,String> overrides=new java.util.LinkedHashMap<>();
        overrides.put("*.scej-network.jp",base);
        for (String host:PLAY_LOG_HOSTS) overrides.put(host,base+":"+COMMUNITY_API_PORT);
        return json(overrides);
    }
}
