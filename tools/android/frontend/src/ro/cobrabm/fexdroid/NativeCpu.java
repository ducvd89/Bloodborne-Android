package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.Map;

/** Native CPU: the game's code compiled ahead of time to ARM64 (libbbaot.so, made on the player's PC with bb-recomp from
 *  their own game dump: docs/NATIVE_CPU.md), run by the runtime in front of the FEX JIT. The files live in
 *  bbport/aot/ (libbbaot.so and libbbaot.json); run-bionic.sh loads the library when it is there, unless bbport.ini
 *  has native_cpu=0 (or a BB_AOT from bionic-local.sh says otherwise). This class reads and checks the files and
 *  imports a picked pair. No Android classes, so it is tested on a desktop JDK (tests/android-native-cpu). */
final class NativeCpu {
    private NativeCpu() {}

    /** The libbbaot.json layout and the library's ABI (bbr_abi_version, docs/RECOMP_ABI.md) this app and its runtime take. */
    static final int FORMAT = 1, ABI_VERSION = 1;
    static final String KIND = "bb-recomp-aot", SO = "libbbaot.so", JSON = "libbbaot.json", SETTING = "native_cpu";
    static final String DOCS = "https://github.com/ducvd89/Bloodborne-Android/blob/master/docs/NATIVE_CPU.md";

    /** A reason shown to the player as it is. */
    static final class Problem extends Exception { Problem(String message) { super(message); } }

    /** libbbaot.json. */
    static final class Info {
        String gameId, gameVersion, gameTitle, imageHash, tool, created, soSha256;
        long unitCount, functionCount, soSize, abiVersion, abiFlags;
    }

    /** Whether the library was made from the game image on this phone. */
    enum Match { MATCH, MISMATCH, UNKNOWN }

    /** Everything the menu shows. */
    static final class State {
        boolean installed;   // libbbaot.so is in bbport/aot
        Info info;           // its libbbaot.json (null: missing or unusable, see problem)
        String problem;      // why the installed files cannot be used, or null
        Match match = Match.UNKNOWN;
        String matchNote;    // the reason, for a mismatch or an unchecked match
        boolean enabled;     // native_cpu is not 0
        boolean manualOverride; // bionic-local.sh sets BB_AOT: it wins over the switch
        /** The runtime loads the library at the next start. */
        boolean active() { return installed && info != null && problem == null && enabled && match != Match.MISMATCH && !manualOverride; }
    }

    static File dir(File base) { return new File(base, "aot"); }
    static File so(File base) { return new File(dir(base), SO); }
    static File json(File base) { return new File(dir(base), JSON); }
    static File bootImage(File base) { return new File(new File(base, "data"), "boot-linked.bin"); }

    // ---------------------------------------------------------------- libbbaot.json

    private static String str(Map<?, ?> m, String key) { Object v = m.get(key); return v instanceof String ? (String) v : null; }
    private static long num(Map<?, ?> m, String key, long fallback) { Object v = m.get(key); return v instanceof Long ? (Long) v : fallback; }

    /** Reads and checks a libbbaot.json: the format, the kind, the ABI version and the fields the checks need. */
    static Info parse(String text) throws Problem {
        Object root;
        try { root = MiniJson.parse(text); }
        catch (MiniJson.ParseException e) { throw new Problem("libbbaot.json is not readable (" + e.getMessage() + "). Copy it again from the PC."); }
        if (!(root instanceof Map)) throw new Problem("libbbaot.json is not a bb-recomp description.");
        Map<?, ?> m = (Map<?, ?>) root;
        if (!KIND.equals(str(m, "kind"))) throw new Problem("That file is not a bb-recomp libbbaot.json.");
        long format = num(m, "format", -1);
        if (format != FORMAT) throw new Problem("This libbbaot.json has format " + format + "; this app reads format " + FORMAT + ". Use the bb-recomp that matches this app version.");
        Info info = new Info();
        info.abiVersion = num(m, "abi_version", -1);
        if (info.abiVersion != ABI_VERSION)
            throw new Problem("This library uses ABI version " + info.abiVersion + "; this app's runtime takes version " + ABI_VERSION
                + ". Build it again with the bb-recomp that matches this app version (docs/NATIVE_CPU.md).");
        info.gameId = str(m, "game_id"); info.gameVersion = str(m, "game_version"); info.gameTitle = str(m, "game_title");
        info.imageHash = str(m, "image_hash"); info.tool = str(m, "tool"); info.created = str(m, "created"); info.soSha256 = str(m, "so_sha256");
        info.unitCount = num(m, "unit_count", 0); info.functionCount = num(m, "function_count", 0); info.soSize = num(m, "so_size", 0);
        info.abiFlags = num(m, "abi_flags", 0);
        if (info.imageHash == null || info.imageHash.length() != 16 || info.soSha256 == null || info.soSha256.length() != 64 || info.soSize <= 0 || info.unitCount <= 0)
            throw new Problem("libbbaot.json is incomplete (image hash, size or unit count missing). Copy it again from the PC.");
        return info;
    }

    // ---------------------------------------------------------------- the game image on this phone

    /** FNV-1a of the flat memory image inside a boot-linked.bin (BBPROBE5: the header, the module and segment tables, the
     *  import names and relocations, then the image): bb-recomp's image hash (bbr_image_hash). Takes a second on a phone. */
    static String imageHash(File boot) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(boot, "r")) {
            byte[] head = new byte[8 + 12 * 8];
            f.readFully(head);
            if (!"BBPROBE5".equals(new String(head, 0, 8, "US-ASCII"))) throw new IOException("not a BBPROBE5 game image");
            ByteBuffer b = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN);
            long size = b.getLong(8), ns = b.getLong(24), nr = b.getLong(32), ni = b.getLong(40), mc = b.getLong(8 + 11 * 8);
            if (size <= 0 || size > (512L << 20) || mc < 1 || mc > 16 || ns > 64) throw new IOException("bad game image header");
            f.seek(head.length + mc * 7 * 8);
            byte[] nb = new byte[8]; f.readFully(nb);
            long bindings = ByteBuffer.wrap(nb).order(ByteOrder.LITTLE_ENDIAN).getLong();
            long start = f.getFilePointer() + bindings * 24 + ns * 24 + ni * 128 + nr * 32;
            if (start + size != f.length()) throw new IOException("the game image size does not match its header");
            f.seek(start);
            long h = 1469598103934665603L, left = size; // the basis bb-recomp and libbbcpu use (it is not the textbook FNV offset)
            byte[] buf = new byte[1 << 20];
            while (left > 0) {
                int n = f.read(buf, 0, (int) Math.min(left, buf.length));
                if (n <= 0) throw new IOException("short game image");
                for (int i = 0; i < n; ++i) { h ^= buf[i] & 0xff; h *= 0x100000001b3L; }
                left -= n;
            }
            return String.format(Locale.ROOT, "%016x", h);
        }
    }

    private static File hashCache(File base) { return new File(new File(base, "data"), "boot-linked.fnv"); }

    /** The image hash, from its cache when the image file is the one the cache was made for (size and time), else computed
     *  and cached. Null when there is no image yet (the game was not prepared). */
    static String deviceImageHash(File base, boolean compute) {
        File boot = bootImage(base);
        if (!boot.isFile()) return null;
        String key = boot.length() + ":" + boot.lastModified();
        try {
            String cached = SafeWrite.readText(hashCache(base));
            if (cached != null) {
                String[] p = cached.trim().split(" ");
                if (p.length == 2 && p[0].equals(key) && p[1].length() == 16) return p[1];
            }
        } catch (IOException ignored) {}
        if (!compute) return null;
        try {
            String hash = imageHash(boot);
            try { SafeWrite.write(hashCache(base), key + " " + hash + "\n"); } catch (IOException ignored) {}
            return hash;
        } catch (IOException e) { return null; }
    }

    /** The reason a library is not for this phone's game, or null. `imageHash` may be null (no image yet); the game's id and
     *  version (its param.sfo) may be null (no game folder chosen). */
    static String mismatch(Info info, String imageHash, String gameId, String gameVersion) {
        if (imageHash != null && !imageHash.equalsIgnoreCase(info.imageHash))
            return "It was built from another build of the game's code (image " + info.imageHash + ", " + describe(info.gameId, info.gameVersion)
                + "; this phone has image " + imageHash + (gameId != null ? ", " + describe(gameId, gameVersion) : "")
                + "). Build it from the same game folder the app uses, with the 1.09 update merged in.";
        if (gameId != null && info.gameId != null && !gameId.equals(info.gameId))
            return "It was built from " + info.gameId + ", but the game on this phone is " + gameId + ".";
        if (gameVersion != null && info.gameVersion != null && !gameVersion.equals(info.gameVersion))
            return "It was built from game version " + info.gameVersion + ", but the game on this phone is version " + gameVersion + ".";
        return null;
    }
    private static String describe(String id, String version) { return (id == null ? "?" : id) + " version " + (version == null ? "?" : version); }

    // ---------------------------------------------------------------- state

    /** bionic-local.sh sets BB_AOT by hand: run-bionic.sh then ignores the switch. */
    static boolean manualOverride(File base) {
        try {
            String text = SafeWrite.readText(new File(base, "bionic-local.sh"));
            if (text == null) return false;
            for (String line : text.split("\n")) {
                String t = line.trim();
                if (!t.startsWith("#") && (t.startsWith("export BB_AOT=") || t.startsWith("BB_AOT="))) return true;
            }
        } catch (IOException ignored) {}
        return false;
    }

    static boolean enabled(Map<String, String> ini) { return !"0".equals(ini.get(SETTING)); }

    /** What is installed and whether it fits the game on this phone. `imageHash` is deviceImageHash(...) (null: unknown),
     *  gameId/gameVersion the game folder's param.sfo (null: unknown). Cheap: no hashing here. */
    static State state(File base, Map<String, String> ini, String imageHash, String gameId, String gameVersion) {
        State s = new State();
        s.enabled = enabled(ini);
        s.manualOverride = manualOverride(base);
        File so = so(base);
        s.installed = so.isFile();
        if (!s.installed) return s;
        try {
            String text = SafeWrite.readText(json(base));
            if (text == null) { s.problem = "libbbaot.json is missing next to libbbaot.so: import both files again."; return s; }
            s.info = parse(text);
        } catch (IOException e) { s.problem = "libbbaot.json cannot be read: " + e.getMessage(); return s; }
        catch (Problem p) { s.problem = p.getMessage(); return s; }
        if (so.length() != s.info.soSize) {
            s.problem = "libbbaot.so (" + so.length() + " bytes) is not the one libbbaot.json describes (" + s.info.soSize + " bytes): import both files again.";
            s.info = null; return s;
        }
        String why = mismatch(s.info, imageHash, gameId, gameVersion);
        if (why != null) { s.match = Match.MISMATCH; s.matchNote = why; }
        else if (imageHash != null) s.match = Match.MATCH;
        else { s.match = Match.UNKNOWN; s.matchNote = "The game image is not prepared yet: it is checked against this library when it is."; }
        return s;
    }

    // ---------------------------------------------------------------- import

    /** Receives the progress of a copy: bytes so far of a total. */
    interface Progress { void bytes(long done, long total); }

    private static String hex(byte[] d) {
        StringBuilder sb = new StringBuilder();
        for (byte b : d) sb.append(String.format(Locale.ROOT, "%02x", b & 0xff));
        return sb.toString();
    }

    /** Checks the first bytes of an AArch64 ELF shared object (what libbbaot.so must be). */
    static String checkElfHeader(byte[] h, int n) {
        if (n < 20 || h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return "libbbaot.so is not an ELF library.";
        if (h[4] != 2 || h[5] != 1) return "libbbaot.so is not a 64-bit little-endian library.";
        int type = (h[16] & 0xff) | (h[17] & 0xff) << 8, machine = (h[18] & 0xff) | (h[19] & 0xff) << 8;
        if (machine != 183) return "libbbaot.so is not for ARM64 phones (machine " + machine + "): was it built with --target x86_64?";
        if (type != 3) return "libbbaot.so is not a shared library.";
        return null;
    }

    /** Checks a picked pair against each other and this phone, then installs both into bbport/aot/ (the library first, the
     *  description last, each renamed whole). Blocking (a copy of about 125 MB): not for the UI thread.
     *  @param gameId,gameVersion the game folder's param.sfo, or null when none is chosen
     *  @return the installed library's description
     *  @throws Problem the reason it was refused; nothing is changed then */
    static Info install(File base, String jsonText, InputStream soIn, long soSizeHint, String gameId, String gameVersion, Progress progress)
            throws Problem, IOException {
        Info info = parse(jsonText);
        String why = mismatch(info, deviceImageHash(base, true), gameId, gameVersion);
        if (why != null) throw new Problem(why);
        File dir = dir(base);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new Problem("Cannot create " + dir + ".");
        if (dir.getUsableSpace() < info.soSize + (64L << 20)) throw new Problem("Not enough free space on the phone: libbbaot.so needs " + (info.soSize >> 20) + " MB.");
        File part = new File(dir, SO + ".part");
        MessageDigest sha;
        try { sha = MessageDigest.getInstance("SHA-256"); } catch (Exception e) { throw new IOException(e); }
        long total = 0;
        try (FileOutputStream out = new FileOutputStream(part)) {
            byte[] buf = new byte[1 << 16];
            int n; byte[] head = new byte[64]; int headLen = 0;
            while ((n = soIn.read(buf)) > 0) {
                if (headLen < head.length) { int c = Math.min(n, head.length - headLen); System.arraycopy(buf, 0, head, headLen, c); headLen += c; }
                total += n;
                if (total > info.soSize) throw new Problem("libbbaot.so is larger than libbbaot.json says (" + info.soSize + " bytes): they are not from the same build.");
                sha.update(buf, 0, n); out.write(buf, 0, n);
                if (progress != null) progress.bytes(total, soSizeHint > 0 ? soSizeHint : info.soSize);
            }
            out.flush(); out.getFD().sync();
            String bad = checkElfHeader(head, headLen);
            if (bad != null) throw new Problem(bad);
        } catch (Problem | IOException | RuntimeException e) { part.delete(); throw e; }
        try {
            if (total != info.soSize) throw new Problem("libbbaot.so is " + total + " bytes, libbbaot.json says " + info.soSize + ": it was damaged in the transfer, or the two files are from different builds. Copy both again.");
            if (!hex(sha.digest()).equalsIgnoreCase(info.soSha256)) throw new Problem("libbbaot.so does not match its libbbaot.json (checksum): it was damaged in the transfer, or the files are from different builds. Copy both again.");
        } catch (Problem p) { part.delete(); throw p; }
        File so = so(base);
        so.delete(); // the old pair stays complete until here; a failure after this leaves no library, not a wrong one
        json(base).delete();
        if (!part.renameTo(so)) { part.delete(); throw new IOException("Could not save " + so); }
        SafeWrite.write(json(base), jsonText);
        return info;
    }

    /** Removes the installed library and its description. */
    static void remove(File base) {
        so(base).delete(); json(base).delete(); new File(dir(base), SO + ".part").delete();
    }

    // ---------------------------------------------------------------- text

    static String megabytes(long bytes) { return String.format(Locale.ROOT, "%.0f MB", bytes / 1048576.0); }

    /** The drawer row's second line (no hashing: the cached image hash only). */
    static String summary(State s) {
        if (!s.installed) return "Not installed";
        if (s.problem != null || s.info == null) return "Installed, but unusable";
        if (s.manualOverride) return "Set by bionic-local.sh";
        if (s.match == Match.MISMATCH) return "Installed, another game version";
        return s.active() ? "On" : "Installed, off";
    }
}
