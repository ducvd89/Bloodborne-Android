package com.ducvd89.bloodborne;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.Charset;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

/** The game files bbport runs: Bloodborne with the 1.09 update merged in. Java port of scripts/game_check.py (no
 *  Android classes, so it is tested on a desktop JDK). The retail releases (the US CUSA00900, the European Game of the
 *  Year edition CUSA03173, the Asian The Old Hunters Edition CUSA03023, ...) share the same 1.09 executable.
 *
 *  Other versions start and then fail inside the game's code (the base game 1.00 faults at guest offset 0x20348b8):
 *  hooks and patches use the addresses of this one executable. The check compares the loaded executable image, whose
 *  hash is the same whatever tool dumped it (the SELF headers around it differ). The shaders must also unpack: an
 *  extraction tool that leaves parts of compressed sectors stale (LibOrbisPkg PkgTool on .NET 6 or newer, issue #81)
 *  makes the game hang while it loads them, with no error. BB_SKIP_GAME_CHECK=1 skips both. */
final class GameCheck {
    private GameCheck() {}

    static final List<String> SUPPORTED_TITLES = Collections.unmodifiableList(Arrays.asList("CUSA00900", "CUSA00207",
        "CUSA00208", "CUSA00299", "CUSA01363", "CUSA03179", "CUSA03173", "CUSA03014", "CUSA03023"));
    static final String SUPPORTED_VERSION = "01.09";
    static final String SUPPORTED_IMAGE = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
    /** The 1.09 executable with Lance McDonald's 60 fps patch applied to it, as some dumps ship it (seen on US
     *  CUSA00900): one of its edits inverts a branch in the menu code (+0xf6d90d) and the gestures menu crashes the
     *  game. bbport has its own 60 fps; the clean 1.09 eboot.bin is needed. */
    static final String LANCE_60FPS_IMAGE = "bbc91f4dff6bc3118b039464289e39878a8fdfc802a07fab9d5fe84d935a676f";
    /** The game loads these first; a broken extraction damages most files, these among them. */
    static final String CHECKED_FOLDER = "dvdroot_ps4/shader";

    /** What is wrong with a game folder: kind is "missing_update" (base game or an older update), "wrong_eboot"
     *  (param.sfo says 1.09, eboot.bin is another version), "patched_eboot" (the 1.09 eboot.bin with a 60 fps patch
     *  baked in), "other_title" (another edition), "unreadable" or "damaged_files" (the right game, but its shaders do
     *  not unpack: a broken extraction). */
    static final class Found {
        final String kind, title, version;
        /** "damaged_files": the shaders that do not unpack (paths relative to the game); else empty. */
        final List<String> files;
        Found(String kind, String title, String version) { this(kind, title, version, new ArrayList<String>()); }
        Found(String kind, String title, String version, List<String> files) { this.kind = kind; this.title = title; this.version = version; this.files = files; }
    }

    /** eboot.bin is not a decrypted PS4 executable the check can read (Python's ValueError). */
    static final class Unreadable extends Exception { Unreadable(String m) { super(m); } }

    // ---- The executable's image ----

    private static final class Segment {
        long type, flags, offset, vaddr, filesz, memsz;
    }
    /** `length` bytes of the executable's file that end up at `elf` offset `at` (a piece of the virtual ELF). */
    private static final class Piece {
        final long at, file, length;
        Piece(long at, long file, long length) { this.at = at; this.file = file; this.length = length; }
    }

    private static long u64(byte[] d, int at) throws Unreadable {
        if (at < 0 || at + 8 > d.length) throw new Unreadable("out of bounds");
        long v = 0;
        for (int i = 7; i >= 0; --i) v = v << 8 | (d[at + i] & 0xffL);
        return v;
    }
    private static long u32(byte[] d, int at) throws Unreadable {
        if (at < 0 || at + 4 > d.length) throw new Unreadable("out of bounds");
        return (d[at] & 0xffL) | (d[at + 1] & 0xffL) << 8 | (d[at + 2] & 0xffL) << 16 | (d[at + 3] & 0xffL) << 24;
    }
    private static int u16(byte[] d, int at) throws Unreadable {
        if (at < 0 || at + 2 > d.length) throw new Unreadable("out of bounds");
        return (d[at] & 0xff) | (d[at + 1] & 0xff) << 8;
    }
    /** span(): bounds-checked read of the file. */
    private static byte[] read(RandomAccessFile f, long offset, int size) throws IOException, Unreadable {
        if (offset < 0 || size < 0 || offset + size > f.length())
            throw new Unreadable(String.format(Locale.ROOT, "out of bounds: 0x%x+0x%x / 0x%x", offset, (long)size, f.length()));
        byte[] out = new byte[size];
        f.seek(offset);
        f.readFully(out);
        return out;
    }
    private static void check(RandomAccessFile f, long offset, long size) throws IOException, Unreadable {
        if (offset < 0 || size < 0 || offset + size > f.length())
            throw new Unreadable(String.format(Locale.ROOT, "out of bounds: 0x%x+0x%x / 0x%x", offset, size, f.length()));
    }

    private static List<Segment> programHeaders(RandomAccessFile f, long headerAt, byte[] header, long phoff, int count) throws IOException, Unreadable {
        List<Segment> ph = new ArrayList<>();
        for (int i = 0; i < count; ++i) {
            byte[] e = read(f, headerAt + phoff + i * 56L, 56);
            Segment s = new Segment();
            s.type = u32(e, 0); s.flags = u32(e, 4); s.offset = u64(e, 8); s.vaddr = u64(e, 16);
            s.filesz = u64(e, 32); s.memsz = u64(e, 40);
            ph.add(s);
        }
        return ph;
    }
    private static boolean elfHeaderOk(byte[] h) throws Unreadable {
        boolean ident = h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 2 && h[5] == 1 && h[6] == 1;
        if (!ident || u16(h, 18) != 62) throw new Unreadable("expected little-endian x86-64 ELF");
        int phentsize = u16(h, 54), phnum = u16(h, 56);
        if (phentsize != 56 || !(phnum > 0 && phnum < 256)) throw new Unreadable("unsupported program headers");
        return true;
    }

    /** The SHA-256 of eboot.bin's loaded image (its loadable segments at their addresses), computed without holding
     *  the image in memory. Unreadable when it is not a plain PS4 SELF or ELF (prepare.py's parse_self). */
    static String imageSha256(File game) throws IOException, Unreadable {
        try (RandomAccessFile f = new RandomAccessFile(new File(game, "eboot.bin"), "r")) {
            byte[] magic = read(f, 0, 4);
            List<Segment> ph;
            List<Piece> pieces = new ArrayList<>(); // later pieces overwrite earlier ones, as slice assignments did
            long elfSize;
            if (magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
                byte[] h = read(f, 0, 64);
                elfHeaderOk(h);
                long phoff = u64(h, 32); int phnum = u16(h, 56);
                ph = programHeaders(f, 0, h, phoff, phnum);
                long end = phoff + phnum * 56L;
                for (Segment p : ph) end = Math.max(end, p.offset + p.filesz);
                if (end > 512L * 1024 * 1024) throw new Unreadable("probe image exceeds 512 MiB limit");
                check(f, 0, end);
                pieces.add(new Piece(0, 0, end));
                elfSize = end;
            } else if (magic[0] == 'O' && magic[1] == 0x15 && magic[2] == '=' && magic[3] == 0x1d) {
                int count = u16(read(f, 24, 2), 0);
                long base = 32 + count * 32L;
                byte[] h = read(f, base, 64);
                elfHeaderOk(h);
                long phoff = u64(h, 32); int phnum = u16(h, 56);
                ph = programHeaders(f, base, h, phoff, phnum);
                long end = 0;
                for (Segment p : ph) end = Math.max(end, p.offset + p.filesz);
                if (end > 512L * 1024 * 1024) throw new Unreadable("probe image exceeds 512 MiB limit");
                long headerEnd = phoff + phnum * 56L;
                check(f, base, headerEnd);
                // The header and program headers are copied in first; a segment table that points past them would
                // have grown the buffer (the Python's bytearray(end)): only offsets inside `end` are kept.
                if (headerEnd > end) throw new Unreadable("unsupported program headers");
                pieces.add(new Piece(0, base, headerEnd));
                List<long[]> covered = new ArrayList<>();
                covered.add(new long[]{0, headerEnd});
                for (int i = 0; i < count; ++i) {
                    byte[] e = read(f, 32 + i * 32L, 32);
                    long flags = u64(e, 0), off = u64(e, 8), size = u64(e, 16), mem = u64(e, 24);
                    if ((flags & 0x800) == 0) continue;
                    if ((flags & 10) != 0) throw new Unreadable("encrypted/compressed SELF segment is unsupported");
                    int index = (int)((flags >> 20) & 4095);
                    if (index >= ph.size()) throw new Unreadable("invalid segment index");
                    Segment p = ph.get(index);
                    if (size != p.filesz || size != mem) throw new Unreadable("unsupported blocked segment layout");
                    check(f, off, size);
                    pieces.add(new Piece(p.offset, off, size));
                    covered.add(new long[]{p.offset, p.offset + size});
                }
                for (int i = 0; i < ph.size(); ++i) {
                    Segment p = ph.get(i);
                    if (p.filesz == 0) continue;
                    boolean found = false;
                    for (long[] c : covered) if (c[0] <= p.offset && p.offset + p.filesz <= c[1]) found = true;
                    if (!found && (p.type == 1 || p.type == 2 || p.type == 0x61000000L || p.type == 0x61000010L))
                        throw new Unreadable("required segment " + i + " is unavailable");
                }
                elfSize = end;
            } else {
                throw new Unreadable("expected PS4 SELF or ELF (eboot.bin is neither: an encrypted or damaged dump?)");
            }
            List<Segment> loads = new ArrayList<>();
            long imageSize = 0;
            for (Segment p : ph) if (p.type == 1 || p.type == 0x61000010L) { loads.add(p); imageSize = Math.max(imageSize, p.vaddr + p.memsz); }
            if (loads.isEmpty()) throw new Unreadable("no loadable segments");
            for (Segment p : loads) if (p.offset + p.filesz > elfSize) throw new Unreadable("out of bounds");
            MessageDigest sha;
            try { sha = MessageDigest.getInstance("SHA-256"); }
            catch (NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
            final int chunk = 1 << 20;
            byte[] image = new byte[chunk];
            for (long at = 0; at < imageSize; at += chunk) {
                int n = (int)Math.min(chunk, imageSize - at);
                Arrays.fill(image, 0, n, (byte)0);
                for (Segment p : loads) fill(f, image, at, n, p, pieces);
                sha.update(image, 0, n);
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : sha.digest()) hex.append(String.format(Locale.ROOT, "%02x", b & 0xff));
            return hex.toString();
        }
    }

    /** Copies into image[0..n) (the image at `at`) the part of load segment `p` that falls in it. */
    private static void fill(RandomAccessFile f, byte[] image, long at, int n, Segment p, List<Piece> pieces) throws IOException {
        long from = Math.max(at, p.vaddr), to = Math.min(at + n, p.vaddr + p.filesz);
        if (from >= to) return;
        // elf offsets [from - vaddr + offset, to - vaddr + offset)
        long elfFrom = from - p.vaddr + p.offset, elfTo = to - p.vaddr + p.offset;
        // The virtual ELF is zeros under the pieces in order: apply each to the part of the image it reaches.
        byte[] part = new byte[(int)(to - from)];
        for (Piece piece : pieces) {
            long a = Math.max(elfFrom, piece.at), b = Math.min(elfTo, piece.at + piece.length);
            if (a >= b) continue;
            f.seek(piece.file + (a - piece.at));
            f.readFully(part, (int)(a - elfFrom), (int)(b - a));
        }
        System.arraycopy(part, 0, image, (int)(from - at), part.length);
    }

    // ---- The shaders ----

    /** True when a DFLT .dcx file does not unpack to its stated size or fails zlib's checksum. Other compressions
     *  are not checked. */
    static boolean dcxBroken(File path) throws IOException {
        try (RandomAccessFile f = new RandomAccessFile(path, "r")) {
            byte[] head = new byte[0x4C];
            int got = 0;
            for (int n; got < head.length && (n = f.read(head, got, head.length - got)) > 0; ) got += n;
            if (got < 0x2C) return false;
            if (!(head[0] == 'D' && head[1] == 'C' && head[2] == 'X' && head[3] == 0)) return false;
            if (!(head[0x18] == 'D' && head[0x19] == 'C' && head[0x1A] == 'S' && head[0x1B] == 0)) return false;
            if (!(head[0x28] == 'D' && head[0x29] == 'F' && head[0x2A] == 'L' && head[0x2B] == 'T')) return false;
            if (got < 0x24) return false;
            long size = ((head[0x1C] & 0xffL) << 24) | ((head[0x1D] & 0xffL) << 16) | ((head[0x1E] & 0xffL) << 8) | (head[0x1F] & 0xffL);
            long packed = ((head[0x20] & 0xffL) << 24) | ((head[0x21] & 0xffL) << 16) | ((head[0x22] & 0xffL) << 8) | (head[0x23] & 0xffL);
            long available = Math.max(0, Math.min(packed, f.length() - 0x4C));
            f.seek(0x4C);
            Inflater inflater = new Inflater();
            try {
                byte[] in = new byte[64 << 10], out = new byte[64 << 10];
                long total = 0, left = available;
                while (!inflater.finished()) {
                    if (inflater.needsInput()) {
                        if (left <= 0) return true; // truncated stream
                        int n = f.read(in, 0, (int)Math.min(in.length, left));
                        if (n <= 0) return true;
                        left -= n;
                        inflater.setInput(in, 0, n);
                    }
                    int n = inflater.inflate(out);
                    total += n;
                    if (total > size) return true;
                    if (n == 0 && (inflater.needsDictionary() || (!inflater.needsInput() && !inflater.finished()))) return true;
                }
                return total != size;
            } catch (DataFormatException e) {
                return true;
            } finally {
                inflater.end();
            }
        }
    }

    /** The .dcx files under the game's folder that do not unpack (paths relative to the game, posix). */
    static List<String> brokenFiles(File game, String folder) throws IOException {
        List<String> out = new ArrayList<>();
        File root = new File(game, folder);
        List<String> found = new ArrayList<>();
        collect(root, "", found);
        Collections.sort(found, FsUtil.CODEPOINTS);
        for (String relative : found) {
            if (dcxBroken(new File(root, relative))) out.add(folder + "/" + relative);
        }
        return out;
    }
    private static void collect(File dir, String prefix, List<String> out) {
        for (String name : FsUtil.names(dir)) {
            File f = new File(dir, name);
            if (f.isDirectory()) collect(f, prefix + name + "/", out);
            else if (name.endsWith(".dcx")) out.add(prefix + name);
        }
    }

    /** Whether BB_SKIP_GAME_CHECK=1 asks to skip the check. */
    static boolean skipped() { return "1".equals(System.getenv("BB_SKIP_GAME_CHECK")); }

    /** null for the supported game, else what is wrong. `skip`: BB_SKIP_GAME_CHECK. */
    static Found problem(File game, boolean skip) { return problem(game, skip, null, SUPPORTED_IMAGE, LANCE_60FPS_IMAGE); }
    static Found problem(File game, boolean skip, String imageHash, String supported, String lance) {
        if (skip) return null;
        Map<String, Object> info = null;
        try { info = Sfo.parse(java.nio.file.Files.readAllBytes(new File(game, "sce_sys/param.sfo").toPath())); }
        catch (IOException | Sfo.Invalid e) { info = null; }
        String title = Sfo.text(info, "TITLE_ID"), version = Sfo.text(info, "APP_VER");
        if (title == null) title = "?";
        if (version == null) version = "?";
        try {
            if (imageHash == null) imageHash = imageSha256(game);
            if (imageHash.equals(supported)) {
                List<String> broken = brokenFiles(game, CHECKED_FOLDER);
                return broken.isEmpty() ? null : new Found("damaged_files", title, version, broken);
            }
        } catch (IOException | Unreadable e) {
            return new Found("unreadable", title, version);
        }
        if (imageHash.equals(lance)) return new Found("patched_eboot", title, version);
        if (!SUPPORTED_TITLES.contains(title)) return new Found("other_title", title, version);
        if (!version.equals(SUPPORTED_VERSION)) return new Found("missing_update", title, version);
        return new Found("wrong_eboot", title, version);
    }

    /** What is wrong and what to do, for the player (English). */
    static String explain(Found found) {
        String f = "Found " + found.title + " version " + found.version + " (sce_sys/param.sfo).";
        switch (found.kind) {
            case "missing_update": return f + " bbport needs the 1.09 update merged into the game folder: copy "
                + "everything from the dumped 1.09 update (e.g. CUSA03173-patch) into the "
                + "game folder, replacing files (eboot.bin and sce_sys too).";
            case "wrong_eboot": return f + " param.sfo is from 1.09 but eboot.bin is not: copy eboot.bin from "
                + "the dumped 1.09 update into the game folder, replacing the old one.";
            case "patched_eboot": return f + " This eboot.bin has the 60 fps patch by Lance McDonald applied "
                + "to it, which crashes the game when the gestures menu opens (bbport has its "
                + "own 60 fps): copy eboot.bin from a clean dump of the 1.09 update into the "
                + "game folder, replacing this one.";
            case "other_title": return f + " This is not a retail release of Bloodborne: bbport runs Bloodborne "
                + "(any region, The Old Hunters editions too) with update 1.09.";
            case "unreadable": return f + " eboot.bin could not be read as a decrypted PS4 executable: dump "
                + "the game and the 1.09 update again.";
            case "damaged_files": {
                StringBuilder list = new StringBuilder();
                for (int i = 0; i < found.files.size() && i < 5; ++i) list.append("\n- ").append(found.files.get(i));
                if (found.files.size() > 5) list.append("\n- and ").append(found.files.size() - 5).append(" more");
                return f + " The installed game has damaged files: " + found.files.size() + " of the shaders in " + CHECKED_FOLDER
                    + " do not unpack, and the game would hang while loading them." + list
                    + "\n\nThe PKG was damaged or incomplete, or the install was cut short. Delete the game folder, copy the base "
                    + "game PKG and the 1.09 update PKG again (a fresh download), and install both again.";
            }
            default: return f;
        }
    }
}
