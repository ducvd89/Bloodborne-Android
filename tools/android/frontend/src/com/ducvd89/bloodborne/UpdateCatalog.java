package com.ducvd89.bloodborne;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** The newest release of this app (tag android-<version>) on GitHub. The releases are listed and filtered
 *  by tag, as DriverCatalog does (the Turnip driver releases share the repository, so /latest would
 *  find either). Only an APK of the repository's own releases is offered: Bloodborne-<version>.apk, from
 *  a release that is neither a draft nor a pre-release. */
final class UpdateCatalog {
    private static final String API="https://api.github.com/repos/ducvd89/Bloodborne-Android/releases?per_page=50";
    /** Only assets of this repository's releases are downloaded, whatever the list says. */
    static final String DOWNLOADS="https://github.com/ducvd89/Bloodborne-Android/releases/download/";
    private static final Pattern TAG=Pattern.compile("android-(\\d{1,9}(?:\\.\\d{1,9})*)");
    private static final Pattern VERSION=Pattern.compile("^\\d{1,9}(?:\\.\\d{1,9})*");
    private static final int NOTES=4000;

    /** One release of the app. sha256 is GitHub's digest of the APK ("" when it lists none): then
     *  shaUrl, the release's Bloodborne-<version>.apk.sha256 asset, may hold it ("" when there is none). */
    static final class Release {
        final String version, notes, asset, url, sha256, shaUrl;
        final long size;
        Release(String version,String notes,String asset,long size,String url,String sha256,String shaUrl) {
            this.version=version; this.notes=notes; this.asset=asset; this.size=size;
            this.url=url; this.sha256=sha256; this.shaUrl=shaUrl;
        }
    }

    private UpdateCatalog() {}

    /** Compares dotted numbers component by component, a missing one being 0: "0.8" equals "0.8.0". Text
     *  after the leading number ("0.8.0-dev") is ignored; text without one counts as 0. */
    static int compare(String a,String b) {
        long[] x=numbers(a), y=numbers(b);
        for (int i=0;i<Math.max(x.length,y.length);++i) {
            long p=i<x.length ? x[i] : 0, q=i<y.length ? y[i] : 0;
            if (p!=q) return p<q ? -1 : 1;
        }
        return 0;
    }
    private static long[] numbers(String version) {
        Matcher m=VERSION.matcher(version==null ? "" : version.trim());
        if (!m.find()) return new long[0];
        String[] parts=m.group().split("\\.");
        long[] out=new long[parts.length];
        for (int i=0;i<parts.length;++i) out[i]=Long.parseLong(parts[i]);
        return out;
    }

    /** The highest release in a GitHub releases answer, or null when it has none. */
    static Release parse(String json) throws JSONException {
        JSONArray releases=new JSONArray(json);
        Release best=null;
        for (int i=0;i<releases.length();++i) {
            JSONObject release=releases.optJSONObject(i);
            if (release==null || release.optBoolean("draft") || release.optBoolean("prerelease")) continue;
            Matcher tag=TAG.matcher(text(release,"tag_name"));
            if (!tag.matches()) continue;
            String version=tag.group(1), name="Bloodborne-"+version+".apk";
            if (best!=null && compare(version,best.version)<=0) continue;
            JSONArray assets=release.optJSONArray("assets");
            String url="", digest="", shaUrl="";
            long size=0;
            for (int j=0; assets!=null && j<assets.length(); ++j) {
                JSONObject asset=assets.optJSONObject(j);
                if (asset==null) continue;
                String file=text(asset,"name"), address=text(asset,"browser_download_url");
                if (!address.startsWith(DOWNLOADS) || (!asset.isNull("state") && !"uploaded".equals(text(asset,"state")))) continue;
                if (file.equals(name) && asset.optLong("size")>0) {
                    url=address; size=asset.optLong("size"); digest=text(asset,"digest");
                } else if (file.equals(name+".sha256")) shaUrl=address;
            }
            if (url.isEmpty()) continue;
            best=new Release(version,notes(text(release,"body")),name,size,url,
                digest.startsWith("sha256:") ? digest.substring(7) : "",shaUrl);
        }
        return best;
    }
    /** org.json's optString turns a JSON null into "null". */
    private static String text(JSONObject o,String key) { return o.isNull(key) ? "" : o.optString(key); }
    /** The release's text as plain lines: no carriage returns, no runs of blank lines, trimmed to a length a dialog can scroll. */
    private static String notes(String body) {
        String s=body.replace("\r\n","\n").replace('\r','\n').replaceAll("\n{3,}","\n\n").trim();
        return s.length()>NOTES ? s.substring(0,NOTES).trim()+"…" : s;
    }

    /** Asks GitHub for the newest release (null: none published). Blocks: not for the UI thread. An
     *  IOException's message says what to tell the player. */
    static Release latest() throws IOException {
        HttpURLConnection c=open(API);
        try {
            c.setRequestProperty("Accept","application/vnd.github+json");
            int code=c.getResponseCode();
            if (code==403 || code==429) throw new IOException("GitHub is limiting requests right now");
            if (code!=200) throw new IOException("GitHub answered "+code);
            return parse(read(c,16<<20,true));
        } catch (JSONException e) {
            throw new IOException("GitHub's answer could not be read");
        } finally { c.disconnect(); }
    }

    /** The SHA-256 in the release's Bloodborne-<version>.apk.sha256 ("<hex>  <file name>", as sha256sum
     *  writes it), lowercase; "" when the release has no such asset or it does not read so. Blocks. */
    static String checksum(Release release) throws IOException {
        if (!release.shaUrl.startsWith(DOWNLOADS)) return "";
        HttpURLConnection c=open(release.shaUrl);
        try {
            if (c.getResponseCode()!=200) return "";
            String[] parts=read(c,4096,false).trim().split("\\s+",2);
            if (!parts[0].matches("[0-9a-fA-F]{64}")) return "";
            // A name beside the sum must be the APK's.
            if (parts.length>1 && !parts[1].trim().replaceFirst("^\\*","").equals(release.asset)) return "";
            return parts[0].toLowerCase(Locale.ROOT);
        } finally { c.disconnect(); }
    }

    /** The answer's text; one over the limit fails, or (strict false) is cut at it. */
    private static String read(HttpURLConnection c,int limit,boolean strict) throws IOException {
        ByteArrayOutputStream body=new ByteArrayOutputStream();
        try (InputStream in=c.getInputStream()) {
            byte[] buffer=new byte[1<<14];
            for (int n; (n=in.read(buffer))>0;) {
                body.write(buffer,0,n);
                if (body.size()>limit) { if (strict) throw new IOException("The answer is too large"); break; }
            }
        }
        return body.toString("UTF-8");
    }
    private static HttpURLConnection open(String address) throws IOException {
        HttpURLConnection c=(HttpURLConnection)new URL(address).openConnection();
        c.setConnectTimeout(15000); c.setReadTimeout(30000); c.setInstanceFollowRedirects(true);
        c.setRequestProperty("User-Agent","Bloodborne-Android update check");
        return c;
    }
}
