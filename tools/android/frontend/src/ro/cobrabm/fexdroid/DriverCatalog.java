package ro.cobrabm.fexdroid;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CancellationException;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** The Android driver builds published as GitHub releases of this app (tags turnip-android-* for Turnip, for
 *  Adreno, and panvk-android-* for PanVK, for Mali), and their download. Linux (glibc) Turnip releases (other
 *  turnip-* tags) are not listed: the game runs as an Android process.
 *  A release can hold several builds, each driver zip asset is its own entry: Turnip-android-<variant>-<version>-<sha>.zip,
 *  Turnip_<name>.zip (a mirrored build of someone else's, such as Turnip_Gen8_V37.zip) and PanVK-*.zip
 *  (patches-*.tar.gz and other assets are ignored). The app's own releases (android-*) are in the same
 *  list, so the releases are listed and filtered by tag; /latest would only find the app. The 10 newest
 *  builds (by the date of their asset) are kept. The last answer is cached in files/drivers/.catalog.json with its ETag: the list shows at
 *  once, and a "not modified" answer does not count against GitHub's 60 requests an hour. */
final class DriverCatalog {
    private static final String API="https://api.github.com/repos/ducvd89/Bloodborne-Android/releases?per_page=50";
    /** Only assets of this repository's releases are downloaded, whatever the list says. */
    private static final String DOWNLOADS="https://github.com/ducvd89/Bloodborne-Android/releases/download/";
    private static final int LIMIT=10;
    /** Version of the cache file's layout; a cache without it (or with another number) is ignored. */
    private static final int CACHE_VERSION=3;

    /** One downloadable driver build. published is GitHub's ISO time of the asset (sorts as text). */
    static final class Entry {
        final String tag, title, variant, published, asset, url, sha256;
        final long size;
        Entry(String tag,String title,String variant,String published,String asset,long size,String url,String sha256) {
            this.tag=tag; this.title=title; this.variant=variant; this.published=published;
            this.asset=asset; this.size=size; this.url=url; this.sha256=sha256;
        }
        /** The GPUs the build is for. */
        String gpu() { return gpuName(variant); }
        /** The GPU family ("main", "gen8" or "mali") the variant belongs to. */
        String family() { return DriverCatalog.family(variant); }
        /** The lowest Android API the build runs on (its meta.json minApi). */
        int minApi() { return DriverCatalog.minApiOf(asset); }
        /** The publishing day, like "13 Oct 2026" (or "" when GitHub gave none). */
        String date() {
            try {
                java.text.SimpleDateFormat in=new java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'",Locale.ROOT);
                in.setTimeZone(java.util.TimeZone.getTimeZone("UTC"));
                return day(in.parse(published).getTime());
            } catch (Exception e) { return ""; }
        }
        JSONObject toJson() throws JSONException {
            return new JSONObject().put("tag",tag).put("title",title).put("variant",variant).put("published",published)
                .put("asset",asset).put("size",size).put("url",url).put("sha256",sha256);
        }
        static Entry fromJson(JSONObject o) throws JSONException {
            return new Entry(o.getString("tag"),o.getString("title"),o.getString("variant"),o.getString("published"),
                o.getString("asset"),o.getLong("size"),o.getString("url"),o.optString("sha256",""));
        }
    }

    /** The list and when (milliseconds) it was fetched; etag is GitHub's, for the next request. */
    static final class Result {
        final List<Entry> entries; final long fetched; final String etag;
        Result(List<Entry> entries,long fetched,String etag) { this.entries=entries; this.fetched=fetched; this.etag=etag; }
    }

    private final File cache;
    /** files: the app's files directory. */
    DriverCatalog(File files) { cache=new File(new File(files,"drivers"),".catalog-android.json"); }
    /** Whether a release tag is one of the Android driver builds: turnip-android-* or panvk-android-*. */
    static boolean tagMatches(String tag) { return tag.startsWith("turnip-android-") || tag.startsWith("panvk-android-"); }
    /** Whether a release asset is a driver zip: Turnip-*.zip, Turnip_*.zip or PanVK-*.zip. */
    static boolean assetMatches(String name) {
        return name.endsWith(".zip") && (name.startsWith("Turnip-") || name.startsWith("Turnip_") || name.startsWith("PanVK-"));
    }

    /** The GPU family of a variant: "gen8v37" and "gen8" are both "gen8" (Adreno 8xx). Null stays null. */
    static String family(String variant) {
        return variant!=null && variant.startsWith("gen8") ? "gen8" : variant;
    }
    static String gpuName(String variant) {
        String family=family(variant);
        return "main".equals(family) ? "Adreno 6xx/7xx" : "gen8".equals(family) ? "Adreno 8xx" : "mali".equals(family) ? "Mali" : variant;
    }
    /** The asset's name without the maker's prefix: "Turnip-android-main-26.3.0-35b085c.zip" is "main-26.3.0-35b085c",
     *  "Turnip_Gen8_V37.zip" is "Gen8_V37", "PanVK-android-v3.2-universal-2a9358a.zip" is "v3.2-universal-2a9358a". */
    private static String rest(String asset) {
        String rest=asset.endsWith(".zip") ? asset.substring(0,asset.length()-4) : asset;
        int cut=rest.startsWith("Turnip") ? 7 : rest.startsWith("PanVK") ? 6 : 0;
        rest=rest.substring(Math.min(cut,rest.length()));
        return rest.startsWith("android-") ? rest.substring(8) : rest;
    }
    /** The GPU variant of an asset: "main" or "gen8" for Turnip-android-<variant>-..., "gen8" for Turnip_Gen8_V37.zip
     *  (the token after the prefix, lower case), "main" for another mirrored Turnip_ build (Turnip_v26.3.0-R6-Oneui.zip,
     *  Adreno 6xx/7xx), "mali" for PanVK-*.zip. "" when there is none. */
    static String variantOf(String asset) {
        if (asset.startsWith("PanVK")) return "mali";
        if (asset.startsWith("Turnip_") && !asset.toLowerCase(Locale.ROOT).startsWith("turnip_gen8")) return "main";
        String rest=rest(asset);
        int end=0;
        while (end<rest.length() && rest.charAt(end)!='-' && rest.charAt(end)!='_') ++end;
        return rest.substring(0,end).toLowerCase(Locale.ROOT);
    }
    /** The lowest Android API of a build: PanVK (Mali, kbase) needs 35, the Turnip for Android builds 30, a mirrored
     *  Turnip_ build 28 (the values of their meta.json). */
    static int minApiOf(String asset) {
        return asset.startsWith("PanVK") ? 35 : asset.startsWith("Turnip-") ? 30 : 28;
    }
    /** A readable title: "Turnip 26.3.0 main (35b085c) - Adreno 6xx/7xx" for Turnip-android-main-26.3.0-35b085c.zip,
     *  "Turnip Gen8 V37 (StevenMXZ) - Adreno 8xx" for Turnip_Gen8_V37.zip, "PanVK V3.2 (Mali)" for PanVK-android-v3.2-universal-*.zip. */
    static String titleOf(String asset) {
        String variant=variantOf(asset), rest=rest(asset);
        if (variant.equals("mali")) {
            String version=rest.contains("-") ? rest.substring(0,rest.indexOf('-')) : rest;
            return version.matches("[vV][0-9][0-9.]*") ? "PanVK V"+version.substring(1)+" (Mali)" : "PanVK (Mali)";
        }
        String where=" - "+gpuName(variant);
        if (asset.startsWith("Turnip_")) return "Turnip "+rest.replace('_',' ')+" (StevenMXZ)"+where;
        rest=rest.length()>variant.length() ? rest.substring(variant.length()+1) : "";
        String name=variant.startsWith("gen8") && variant.length()>4 ? "gen8 "+variant.substring(4).toUpperCase(Locale.ROOT) : variant;
        int dash=rest.lastIndexOf('-');
        String version=dash>0 ? rest.substring(0,dash) : rest, sha=dash>0 ? rest.substring(dash+1) : "";
        StringBuilder title=new StringBuilder("Turnip");
        if (!version.isEmpty()) title.append(' ').append(version);
        title.append(' ').append(name);
        if (!sha.isEmpty()) title.append(" (").append(sha).append(')');
        return title.append(where).toString();
    }
    static String day(long millis) {
        java.text.SimpleDateFormat out=new java.text.SimpleDateFormat("d MMM yyyy",Locale.ENGLISH);
        return out.format(new java.util.Date(millis));
    }
    static String megabytes(long bytes) { return String.format(Locale.ROOT,"%.1f MB",bytes/1048576.0); }

    /** The driver builds in a GitHub releases answer: every driver zip asset (assetMatches) of every turnip-android-*
     *  and panvk-android-* release is an entry. Drafts skipped; newest asset first, at most 10. */
    static List<Entry> parse(String json) throws JSONException {
        JSONArray releases=new JSONArray(json);
        List<Entry> out=new ArrayList<>();
        for (int i=0;i<releases.length();++i) {
            JSONObject release=releases.optJSONObject(i);
            if (release==null || release.optBoolean("draft")) continue;
            String tag=text(release,"tag_name");
            if (!tagMatches(tag)) continue;
            String published=text(release,"published_at");
            JSONArray assets=release.optJSONArray("assets");
            for (int j=0; assets!=null && j<assets.length(); ++j) {
                JSONObject asset=assets.optJSONObject(j);
                if (asset==null) continue;
                String name=text(asset,"name"), url=text(asset,"browser_download_url"), digest=text(asset,"digest");
                long size=asset.optLong("size");
                if (!assetMatches(name) || size<=0 || !url.startsWith(DOWNLOADS)) continue;
                String variant=variantOf(name);
                if (variant.isEmpty()) continue;
                // An asset added later to an older release is newer than the release.
                String date=text(asset,"created_at");
                if (date.isEmpty()) date=text(asset,"updated_at");
                if (date.isEmpty()) date=published;
                out.add(new Entry(tag,titleOf(name),variant,date,name,size,url,
                    digest.startsWith("sha256:") ? digest.substring(7) : ""));
            }
        }
        Collections.sort(out,(a,b) -> b.published.compareTo(a.published)); // stable: equal times keep GitHub's order
        return new ArrayList<>(out.subList(0,Math.min(out.size(),LIMIT)));
    }
    /** org.json's optString turns a JSON null into "null". */
    private static String text(JSONObject o,String key) { return o.isNull(key) ? "" : o.optString(key); }

    /** The cached list, or null when there is none (or it cannot be read). */
    Result cached() {
        try {
            JSONObject o=new JSONObject(new String(Files.readAllBytes(cache.toPath()),StandardCharsets.UTF_8));
            if (o.optInt("version")!=CACHE_VERSION) return null; // an older layout: fetch the list again
            JSONArray array=o.getJSONArray("entries");
            List<Entry> entries=new ArrayList<>();
            for (int i=0;i<array.length();++i) entries.add(Entry.fromJson(array.getJSONObject(i)));
            return new Result(entries,o.getLong("fetched"),o.optString("etag",""));
        } catch (Exception e) { return null; }
    }

    /** Asks GitHub for the list (conditionally on the cached one's ETag) and caches the answer.
     *  Blocks: not for the UI thread. An IOException's message says what to tell the player. */
    Result refresh(Result known) throws IOException {
        HttpURLConnection c=open(API);
        try {
            c.setRequestProperty("Accept","application/vnd.github+json");
            if (known!=null && !known.etag.isEmpty()) c.setRequestProperty("If-None-Match",known.etag);
            int code=c.getResponseCode();
            Result result;
            if (code==304 && known!=null) {
                result=new Result(known.entries,System.currentTimeMillis(),known.etag);
            } else if (code==200) {
                ByteArrayOutputStream body=new ByteArrayOutputStream();
                try (InputStream in=c.getInputStream()) {
                    byte[] buffer=new byte[1<<14];
                    for (int n; (n=in.read(buffer))>0;) { body.write(buffer,0,n); if (body.size()>16<<20) throw new IOException("GitHub's answer is too large"); }
                }
                String etag=c.getHeaderField("ETag");
                result=new Result(parse(body.toString("UTF-8")),System.currentTimeMillis(),etag==null ? "" : etag);
            } else if (code==403 || code==429) {
                throw new IOException("GitHub is limiting requests right now");
            } else throw new IOException("GitHub answered "+code);
            save(result);
            return result;
        } catch (JSONException e) {
            throw new IOException("GitHub's answer could not be read");
        } finally { c.disconnect(); }
    }
    private void save(Result result) {
        try {
            JSONArray array=new JSONArray();
            for (Entry e:result.entries) array.put(e.toJson());
            File dir=cache.getParentFile(), partial=new File(dir,".catalog.json.part");
            if (!dir.isDirectory() && !dir.mkdirs()) return;
            Files.write(partial.toPath(),new JSONObject().put("version",CACHE_VERSION).put("fetched",result.fetched).put("etag",result.etag)
                .put("entries",array).toString().getBytes(StandardCharsets.UTF_8));
            if (!partial.renameTo(cache)) partial.delete();
        } catch (Exception e) { android.util.Log.w("Bloodborne","driver catalog cache: "+e); } // only the cache is lost
    }

    private static HttpURLConnection open(String address) throws IOException {
        HttpURLConnection c=(HttpURLConnection)new URL(address).openConnection();
        c.setConnectTimeout(15000); c.setReadTimeout(30000); c.setInstanceFollowRedirects(true);
        c.setRequestProperty("User-Agent","Bloodborne-Android driver list");
        return c;
    }

    /** A short reason for the player: "Offline" when GitHub cannot be reached. */
    static String message(Exception e) {
        if (e instanceof java.net.UnknownHostException || e instanceof java.net.ConnectException
            || e instanceof java.net.SocketTimeoutException || e instanceof javax.net.ssl.SSLException) return "Offline";
        String text=e.getMessage();
        return text==null || text.isEmpty() ? e.getClass().getSimpleName() : text;
    }

    /** Receives the bytes done and the total, at most a few times a second. */
    interface Progress { void update(long done,long total); }

    /** One download of a driver zip; cancel() may be called from any thread to stop it. */
    static final class Download {
        private volatile boolean cancelled;
        private volatile HttpURLConnection connection;

        void cancel() {
            cancelled=true;
            HttpURLConnection c=connection;
            if (c!=null) new Thread(c::disconnect,"Driver-cancel").start(); // closing a socket blocks the UI thread otherwise
        }

        /** Saves the entry's zip to file and checks its size (and SHA-256 when GitHub lists one).
         *  Throws CancellationException after cancel(). */
        void run(Entry entry,File file,Progress progress) throws IOException {
            HttpURLConnection c=open(entry.url);
            connection=c;
            try {
                if (cancelled) throw new CancellationException();
                int code=c.getResponseCode();
                if (code!=200) throw new IOException("The download failed: GitHub answered "+code);
                MessageDigest hash;
                try { hash=MessageDigest.getInstance("SHA-256"); } catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e.toString()); }
                long done=0, last=0;
                byte[] buffer=new byte[1<<16];
                try (InputStream in=c.getInputStream(); FileOutputStream out=new FileOutputStream(file)) {
                    for (int n; (n=in.read(buffer))>0;) {
                        done+=n;
                        if (done>entry.size) throw new IOException("The download is larger than the release says");
                        out.write(buffer,0,n); hash.update(buffer,0,n);
                        long now=System.nanoTime();
                        if (now-last>150_000_000L) { last=now; progress.update(done,entry.size); }
                        if (cancelled) throw new CancellationException();
                    }
                } catch (IOException e) {
                    if (cancelled) throw new CancellationException(); // disconnect() made the read fail
                    throw e;
                }
                if (cancelled) throw new CancellationException();
                if (done!=entry.size) throw new IOException("The download is incomplete ("+done+" of "+entry.size+" bytes). Try again.");
                progress.update(done,entry.size);
                if (!entry.sha256.isEmpty()) {
                    StringBuilder hex=new StringBuilder();
                    for (byte b:hash.digest()) hex.append(String.format(Locale.ROOT,"%02x",b&0xff));
                    if (!hex.toString().equalsIgnoreCase(entry.sha256)) throw new IOException("The download is damaged (its checksum does not match). Try again.");
                }
            } finally { c.disconnect(); }
        }
    }
}
