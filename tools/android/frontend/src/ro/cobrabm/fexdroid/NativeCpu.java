// SPDX-License-Identifier: GPL-2.0-or-later
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

/** Native CPU: the game's code compiled ahead of time to ARM64 (libbbaot.so, made once from the player's own game files: on the phone
 *  by the app (AotBuildService runs the Rebuild Tool's libbbrecomp.so), or on a PC with the Rebuild Tool, TOOL_URL). It is the only way
 *  this app runs the game: there is no translator or interpreter to fall back on, so the game cannot start without a library that
 *  matches the game and this app's ABI. The files live in bbport/aot/ (libbbaot.so and libbbaot.json); run-bionic.sh loads the library
 *  (or a BB_AOT from bionic-local.sh). This class reads and checks the files, imports a picked pair, and decides whether the rebuild
 *  dialog is due (trigger). No Android classes, so it is tested on a desktop JDK (tests/android-native-cpu). */
final class NativeCpu {
    private NativeCpu() {}

    /** The libbbaot.json layout and the library's ABI (bbr_abi_version, docs/RECOMP_ABI.md) this app and its runtime take. */
    static final int FORMAT = 1, ABI_VERSION = 2; // ABI 2: Rebuild Tool 1.2.0 (every library of tool 1.1.0 or older is stale)
    static final String KIND = "bb-recomp-aot", SO = "libbbaot.so", JSON = "libbbaot.json";
    /** The Rebuild Tool (Windows/Linux): its README has the step-by-step guide. */
    static final String TOOL_URL = "https://github.com/ducvd89/Bloodborne-android-rebuild-tool";
    /** The links of the Native CPU page: the newest tool release to download, and its guide. */
    static final String DOWNLOAD_URL = TOOL_URL + "/releases/latest", GUIDE_URL = TOOL_URL + "#how-to-use-it";
    /** The Rebuild Tool the app ships (assets/recomp) and the ABI version it writes: what "built by a matching tool" means. */
    static final String TOOL_VERSION_TEXT = "1.2.0";
    /** The rebuild dialog's title and the full-screen message's text (the reason leads, see {@link Needed}). */
    static final String NEEDED_TITLE = "Native code needed";
    static final String NEEDED_TEXT = "Bloodborne runs as native ARM64 code made from your own game files. It has to be built again for this phone: once, "
        + "and your saves are not touched.";

    /** Why the native code has to be built (or built again). */
    enum Need { NONE, MISSING, ABI, GAME, OTHER }

    /** A reason shown to the player as it is. */
    static final class Problem extends Exception {
        final Need need;
        Problem(String message) { this(message, Need.OTHER); }
        Problem(String message, Need need) { super(message); this.need = need; }
    }

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
        Need problemNeed = Need.OTHER; // the kind of that problem (ABI, damaged, ...)
        Match match = Match.UNKNOWN;
        String matchNote;    // the reason, for a mismatch or an unchecked match
        boolean manualOverride; // bionic-local.sh sets BB_AOT: that library is the one that runs
        /** The runtime can load a library that fits (or the player pointed BB_AOT at one) at the next start. */
        boolean ready() { return manualOverride || (installed && info != null && problem == null && match != Match.MISMATCH); }
        /** Why the game cannot start, or null when it can. */
        /** What has to be built, or NONE when the game can start. */
        Need need() {
            if (ready()) return Need.NONE;
            if (!installed) return Need.MISSING;
            if (problem != null) return problemNeed;
            if (match == Match.MISMATCH) return Need.GAME;
            return Need.OTHER;
        }
        String blocker() {
            if (ready()) return null;
            if (!installed) return "libbbaot.so is missing.";
            if (problem != null) return problem;
            if (match == Match.MISMATCH) return "libbbaot.so does not match this game. " + matchNote;
            return "libbbaot.so is unusable.";
        }
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
        if (format != FORMAT) throw new Problem("This libbbaot.json has format " + format + "; this app reads format " + FORMAT + ". Use the Rebuild Tool version that matches this app version (" + TOOL_URL + ").");
        Info info = new Info();
        info.abiVersion = num(m, "abi_version", -1);
        if (info.abiVersion != ABI_VERSION)
            throw new Problem("This library was built for another app version (ABI version " + info.abiVersion + "; this app takes version " + ABI_VERSION
                + "). It has to be built again: on the phone in the app, or with Rebuild Tool " + TOOL_VERSION_TEXT + " or newer (" + TOOL_URL + ").", Need.ABI);
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

    /** The reason a library is not for this phone's game, or null. The game image decides: every region's 1.09 shares one eboot
     *  (GameCheck.SUPPORTED_IMAGE), so a library built from CUSA03173 serves a CUSA00207 dump too. `imageHash` null means the game
     *  was not prepared yet: then the game version and the title id (it has to be a Bloodborne one) are all there is to compare.
     *  The game's id and version (its param.sfo) may be null (no game folder chosen). */
    static String mismatch(Info info, String imageHash, String gameId, String gameVersion) {
        if (imageHash != null) {
            if (imageHash.equalsIgnoreCase(info.imageHash)) return null;
            return "It was built from another build of the game's code (image " + info.imageHash + ", " + describe(info.gameId, info.gameVersion)
                + "; this phone has image " + imageHash + (gameId != null ? ", " + describe(gameId, gameVersion) : "")
                + "). Build it from the same game folder the app uses, with the 1.09 update merged in.";
        }
        if (info.gameId != null && !GameCheck.SUPPORTED_TITLES.contains(info.gameId))
            return "It was built from " + info.gameId + ", which is not a Bloodborne game folder the app supports.";
        if (info.gameVersion != null && !GameCheck.SUPPORTED_VERSION.equals(info.gameVersion))
            return "It was built from game version " + info.gameVersion + "; the app runs version " + GameCheck.SUPPORTED_VERSION + ".";
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

    /** What is installed and whether it fits the game on this phone. `imageHash` is deviceImageHash(...) (null: unknown),
     *  gameId/gameVersion the game folder's param.sfo (null: unknown). Cheap: no hashing here. */
    static State state(File base, String imageHash, String gameId, String gameVersion) {
        State s = new State();
        s.manualOverride = manualOverride(base);
        File so = so(base);
        s.installed = so.isFile();
        if (!s.installed) return s;
        try {
            String text = SafeWrite.readText(json(base));
            if (text == null) { s.problem = "libbbaot.json is missing next to libbbaot.so: import both files again."; return s; }
            s.info = parse(text);
        } catch (IOException e) { s.problem = "libbbaot.json cannot be read: " + e.getMessage(); return s; }
        catch (Problem p) { s.problem = p.getMessage(); s.problemNeed = p.need; return s; }
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

    // ---------------------------------------------------------------- the library's own checksum

    private static File verifiedMarker(File base) { return new File(dir(base), "libbbaot.verified"); }

    /** Checks libbbaot.so against the SHA-256 in libbbaot.json (a damaged copy, or a library swapped by hand), at the game's start. The result
     *  is remembered for this file (size and time), so the 125 MB are read once. Blocking. Returns the problem, or null when it is fine. */
    static String checksumProblem(File base, Info info) {
        File so = so(base);
        String key = so.length() + ":" + so.lastModified() + ":" + info.soSha256.toLowerCase(Locale.ROOT);
        try {
            String cached = SafeWrite.readText(verifiedMarker(base));
            if (cached != null && cached.trim().equals(key)) return null;
        } catch (IOException ignored) {}
        try (InputStream in = new java.io.FileInputStream(so)) {
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) sha.update(buf, 0, n);
            if (!hex(sha.digest()).equalsIgnoreCase(info.soSha256))
                return "libbbaot.so does not match its libbbaot.json (checksum): the file is damaged. Copy or import it again from the PC.";
        } catch (Exception e) { return "libbbaot.so cannot be read: " + e.getMessage(); }
        try { SafeWrite.write(verifiedMarker(base), key + "\n"); } catch (IOException ignored) {}
        return null;
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
        if (why != null) throw new Problem(why, Need.GAME);
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

    // ---------------------------------------------------------------- the game folder's own copy

    enum SyncKind { NONE, UNCHANGED, IMPORTED, FAILED }
    /** What syncFromGame did. */
    static final class Sync { SyncKind kind = SyncKind.NONE; Info info; String problem; Need need = Need.OTHER; }

    /** The game folder made by the rebuild tool (bb-recomp --pkgs) holds aot/libbbaot.so and aot/libbbaot.json. The runtime cannot load a
     *  library from shared storage (noexec), so they are copied into bbport/aot/ here. Nothing is copied
     *  again while the installed pair is the folder's (same description, so same build and checksum, and the same size). A pair that fails
     *  the checks (ABI, game, checksum) is reported and leaves what is installed alone. Blocking: not for the UI thread. */
    static Sync syncFromGame(File base, File gameDir, String gameId, String gameVersion, Progress progress) {
        Sync result = new Sync();
        if (gameDir == null) return result;
        File srcSo = new File(new File(gameDir, "aot"), SO), srcJson = new File(new File(gameDir, "aot"), JSON);
        if (!srcSo.isFile() || !srcJson.isFile()) return result;
        try {
            String jsonText = SafeWrite.readText(srcJson);
            if (jsonText == null) { result.kind = SyncKind.FAILED; result.problem = "aot/libbbaot.json in the game folder is empty."; return result; }
            Info info = parse(jsonText);
            String installedJson = SafeWrite.readText(json(base));
            File so = so(base);
            if (installedJson != null && installedJson.equals(jsonText) && so.isFile() && so.length() == srcSo.length() && so.length() == info.soSize) {
                result.kind = SyncKind.UNCHANGED; result.info = info; return result;
            }
            try (InputStream in = new java.io.FileInputStream(srcSo)) {
                result.info = install(base, jsonText, in, srcSo.length(), gameId, gameVersion, progress);
            }
            result.kind = SyncKind.IMPORTED;
        } catch (Problem p) { result.kind = SyncKind.FAILED; result.problem = "The native CPU files in the game folder were not used: " + p.getMessage(); result.need = p.need; }
        catch (IOException | RuntimeException e) { result.kind = SyncKind.FAILED; result.problem = "The native CPU files in the game folder could not be read: " + e; }
        return result;
    }

    /** What the rebuild dialog leads with: the kind of need and the reason, worded for the player. */
    static final class Needed {
        final Need need; final String reason;
        Needed(Need need, String reason) { this.need = need; this.reason = reason; }
    }
    static final String NEEDED_MISSING = "This game has no native code yet (no aot/libbbaot.so in its folder, none installed).";

    /** The trigger rule at the game's start: whether the native code has to be built, and why; null when the game can start. `sync` is what
     *  syncFromGame did with the game folder's aot/ (null: not run) and `state` the start state (startState: the checksum checked).
     *  The game starts when the installed library fits (it may have come from the folder's aot/ just now, or earlier by hand). Else
     *  the reason is, in this order: the folder's aot/ pair was refused (another app version = ABI, another game build, damaged), only one
     *  of its two files is there, the installed library does not fit (the same kinds), or there is no native code at all. */
    static Needed needed(File gameDir, Sync sync, State state) {
        if (gameDir == null || state.ready()) return null;
        if (sync != null && sync.kind == SyncKind.FAILED && sync.problem != null) return new Needed(sync.need, sync.problem);
        File aot = new File(gameDir, "aot");
        boolean hasSo = new File(aot, SO).isFile(), hasJson = new File(aot, JSON).isFile();
        if (hasSo != hasJson)
            return new Needed(Need.OTHER, "The aot/ folder in this game folder has " + (hasSo ? SO + " but no " + JSON : JSON + " but no " + SO) + ": both are needed.");
        String blocker = state.blocker();
        if (state.installed && blocker != null) return new Needed(state.need(), blocker);
        return new Needed(Need.MISSING, NEEDED_MISSING);
    }

    /** The game's id and version from a game folder's param.sfo; null each when unknown. */
    static String[] identity(File gameDir) {
        if (gameDir == null) return new String[]{null, null};
        try {
            Map<String, Object> sfo = Sfo.parse(java.nio.file.Files.readAllBytes(new File(gameDir, "sce_sys/param.sfo").toPath()));
            return new String[]{Sfo.text(sfo, "TITLE_ID"), Sfo.text(sfo, "APP_VER")};
        } catch (IOException | Sfo.Invalid | RuntimeException e) { return new String[]{null, null}; }
    }

    /** Copies a built pair into the game folder's aot/ on shared storage (so the folder always holds the current library and the start-up
     *  sync never brings back a stale one): both files renamed whole, the description last. Returns null, or why it failed. */
    static String copyToGame(File gameDir, File so, File json) {
        if (gameDir == null) return "no game folder";
        File dir = new File(gameDir, "aot");
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) return "cannot create " + dir;
            File partSo = new File(dir, SO + ".part"), partJson = new File(dir, JSON + ".part");
            try (InputStream in = new java.io.FileInputStream(so); FileOutputStream out = new FileOutputStream(partSo)) {
                byte[] buf = new byte[1 << 16]; int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                out.flush(); out.getFD().sync();
            }
            java.nio.file.Files.copy(json.toPath(), partJson.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            File dstSo = new File(dir, SO), dstJson = new File(dir, JSON);
            dstJson.delete(); dstSo.delete(); // never a new json beside an old library
            if (!partSo.renameTo(dstSo)) { partSo.delete(); partJson.delete(); return "cannot write " + dstSo; }
            if (!partJson.renameTo(dstJson)) { partJson.delete(); dstSo.delete(); return "cannot write " + dstJson; }
            return null;
        } catch (IOException | RuntimeException e) {
            new File(dir, SO + ".part").delete(); new File(dir, JSON + ".part").delete();
            return e.toString();
        }
    }

    /** Removes the installed library and its description. */
    static void remove(File base) {
        so(base).delete(); json(base).delete(); new File(dir(base), SO + ".part").delete(); verifiedMarker(base).delete();
    }

    // ---------------------------------------------------------------- text

    static String megabytes(long bytes) { return String.format(Locale.ROOT, "%.0f MB", bytes / 1048576.0); }

    /** The drawer row's second line (no hashing: the cached image hash only). */
    static String summary(State s) {
        if (s.manualOverride) return "Set by bionic-local.sh";
        if (!s.installed) return "Required: not installed";
        if (s.problem != null || s.info == null) return "Installed, but unusable: rebuild it";
        if (s.match == Match.MISMATCH) return "Another game version: rebuild it";
        return "Ready";
    }
}
