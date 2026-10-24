// SPDX-License-Identifier: GPL-2.0-or-later
package ro.cobrabm.fexdroid;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** Host-JDK test of the Native CPU code (NativeCpu: libbbaot.json, the checks, the import, the state). Run: tests/android-native-cpu/run.sh */
public final class NativeCpuTest {
    static int checks, failed;
    static void check(boolean ok, String what) { ++checks; if (!ok) { ++failed; System.out.println("FAIL: " + what); } }
    static void refuses(String what, String needle, NativeCpu.Problem p) {
        check(p != null && p.getMessage().contains(needle), what + (p == null ? " (was accepted)" : " (message: " + p.getMessage() + ")"));
    }

    static File temp() throws IOException { File d = Files.createTempDirectory("bbnative").toFile(); d.deleteOnExit(); return d; }

    /** A BBPROBE5 boot file with `image` as its flat image (no modules' details, 1 segment, 2 imports, 3 relocations). */
    static File bootFile(File base, byte[] image) throws IOException {
        long ns = 1, nr = 3, ni = 2, mc = 1, nb = 1;
        ByteBuffer b = ByteBuffer.allocate((int) (8 + 12 * 8 + mc * 56 + 8 + nb * 24 + ns * 24 + ni * 128 + nr * 32 + image.length)).order(ByteOrder.LITTLE_ENDIAN);
        b.put("BBPROBE5".getBytes(StandardCharsets.US_ASCII));
        long[] w = {image.length, 0xa0, ns, nr, ni, 0, 0, 0, 0, 0, 0, mc};
        for (long v : w) b.putLong(v);
        b.position(b.position() + (int) (mc * 56)); // the module table: zeros
        b.putLong(nb);
        b.position(b.position() + (int) (nb * 24 + ns * 24 + ni * 128 + nr * 32)); // bindings, segments, imports, relocations: zeros
        b.put(image);
        File boot = NativeCpu.bootImage(base);
        boot.getParentFile().mkdirs();
        Files.write(boot.toPath(), b.array());
        return boot;
    }
    static String fnv(byte[] d) {
        long h = 1469598103934665603L;
        for (byte x : d) { h ^= x & 0xff; h *= 0x100000001b3L; }
        return String.format(Locale.ROOT, "%016x", h);
    }
    static String sha256(byte[] d) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (byte x : MessageDigest.getInstance("SHA-256").digest(d)) sb.append(String.format(Locale.ROOT, "%02x", x & 0xff));
        return sb.toString();
    }
    static byte[] so(int machine, int size) {
        byte[] d = new byte[size];
        d[0] = 0x7f; d[1] = 'E'; d[2] = 'L'; d[3] = 'F'; d[4] = 2; d[5] = 1; d[16] = 3; d[18] = (byte) machine; d[19] = (byte) (machine >> 8);
        for (int i = 64; i < size; ++i) d[i] = (byte) (i * 31);
        return d;
    }
    static String json(String id, String version, String hash, byte[] so, int abi, int format) throws Exception {
        return "{\n  \"format\": " + format + ",\n  \"kind\": \"bb-recomp-aot\",\n  \"game_id\": \"" + id + "\",\n  \"game_version\": \"" + version + "\",\n"
            + "  \"content_id\": \"EP9000-CUSA03173_00-BLOODBORNE0000EU\",\n  \"game_title\": \"Bloodborne\",\n  \"image_hash\": \"" + hash + "\",\n  \"image_size\": 1234,\n"
            + "  \"abi_version\": " + abi + ",\n  \"abi_flags\": 2,\n  \"unit_count\": 185934,\n  \"function_count\": 182663,\n  \"tool\": \"bb-recomp 0.1.0\",\n"
            + "  \"llvm\": \"23.1.3\",\n  \"created\": \"2026-10-24T05:00:00Z\",\n  \"so_size\": " + so.length + ",\n  \"so_sha256\": \"" + sha256(so) + "\"\n}\n";
    }
    static NativeCpu.Problem tryInstall(File base, String json, byte[] so, String id, String version) throws Exception {
        try { NativeCpu.install(base, json, new ByteArrayInputStream(so), 0, id, version, null); return null; }
        catch (NativeCpu.Problem p) { return p; }
    }

    /** NativeCpu.neededLead as MainActivity.startGame gets it: sync the folder, then the start state (null: no dialog). */
    static String needed(File phone, File game) {
        NativeCpu.Sync sync = NativeCpu.syncFromGame(phone, game, "CUSA00207", "01.09", null);
        String hash = NativeCpu.deviceImageHash(phone, true);
        NativeCpu.State state = NativeCpu.state(phone, hash, "CUSA00207", "01.09");
        if (state.installed && state.info != null && state.problem == null && !state.manualOverride) {
            String bad = NativeCpu.checksumProblem(phone, state.info);
            if (bad != null) state.problem = bad;
        }
        return NativeCpu.neededLead(game, sync, state);
    }

    public static void main(String[] args) throws Exception {
        byte[] image = new byte[5000];
        for (int i = 0; i < image.length; ++i) image[i] = (byte) (i * 7 + 3);
        String hash = fnv(image);

        // the hash of the game image, from the boot file (and its cache)
        File base = temp();
        check(NativeCpu.deviceImageHash(base, true) == null, "no image: no hash");
        bootFile(base, image);
        check(hash.equals(NativeCpu.imageHash(NativeCpu.bootImage(base))), "imageHash is bb-recomp's FNV-1a of the flat image");
        check(NativeCpu.deviceImageHash(base, false) == null, "not cached yet");
        check(hash.equals(NativeCpu.deviceImageHash(base, true)), "computed");
        check(hash.equals(NativeCpu.deviceImageHash(base, false)), "cached");

        // a good pair
        byte[] good = so(183, 100000);
        String goodJson = json("CUSA00207", "01.09", hash, good, 1, 1);
        check(tryInstall(base, goodJson, good, "CUSA00207", "01.09") == null, "good pair is installed");
        check(NativeCpu.so(base).length() == good.length && NativeCpu.json(base).isFile(), "files are in aot/");
        check(!new File(NativeCpu.dir(base), "libbbaot.so.part").exists(), "no .part left");
        NativeCpu.State s = NativeCpu.state(base, hash, "CUSA00207", "01.09");
        check(s.installed && s.info != null && s.problem == null && s.match == NativeCpu.Match.MATCH && s.ready() && s.blocker() == null, "state: installed, matches, ready");
        check(s.info.unitCount == 185934 && "CUSA00207".equals(s.info.gameId) && hash.equals(s.info.imageHash), "info fields");
        check("Ready".equals(NativeCpu.summary(s)), "summary Ready");
        check(NativeCpu.checksumProblem(base, s.info) == null && new File(NativeCpu.dir(base), "libbbaot.verified").isFile(), "checksum fine, remembered");
        check(NativeCpu.checksumProblem(base, s.info) == null, "checksum from the marker");
        s = NativeCpu.state(base, null, null, null);
        check(s.match == NativeCpu.Match.UNKNOWN && s.ready() && s.matchNote != null, "no image yet: unknown, still usable");
        s = NativeCpu.state(base, "0123456789abcdef", "CUSA00207", "01.09");
        check(s.match == NativeCpu.Match.MISMATCH && !s.ready() && s.matchNote.contains("another build") && s.blocker().contains("does not match this game")
            && "Another game version: rebuild it".equals(NativeCpu.summary(s)), "image mismatch: the game cannot start");
        Files.write(new File(base, "bionic-local.sh").toPath(), "#!/bin/sh\n# export BB_AOT=0\nexport BB_AOT=0\n".getBytes(StandardCharsets.UTF_8));
        s = NativeCpu.state(base, hash, null, null);
        check(s.manualOverride && s.ready() && "Set by bionic-local.sh".equals(NativeCpu.summary(s)), "bionic-local.sh BB_AOT is what runs");
        Files.write(new File(base, "bionic-local.sh").toPath(), "# export BB_AOT=0\nexport BB_X=1\n".getBytes(StandardCharsets.UTF_8));
        check(!NativeCpu.manualOverride(base), "a commented BB_AOT is not an override");

        // a library damaged after the import (the checksum is checked at the game's start)
        {
            File b4 = temp(); bootFile(b4, image);
            check(tryInstall(b4, goodJson, good, "CUSA00207", "01.09") == null, "installed again");
            NativeCpu.State st4 = NativeCpu.state(b4, hash, "CUSA00207", "01.09");
            byte[] damaged = good.clone(); damaged[5000] ^= 1;
            Files.write(NativeCpu.so(b4).toPath(), damaged);
            check(NativeCpu.checksumProblem(b4, st4.info) != null && NativeCpu.checksumProblem(b4, st4.info).contains("checksum"), "a damaged library is caught by its checksum");
            check(NativeCpu.NEEDED_TEXT.startsWith("Bloodborne on Android 2.0 runs as native ARM64 code that you build once on a PC (Windows or Linux)") && NativeCpu.NEEDED_TEXT.endsWith("then choose it here."), "the message text");
            check("https://github.com/ducvd89/Bloodborne-android-rebuild-tool/releases/latest".equals(NativeCpu.DOWNLOAD_URL) && "https://github.com/ducvd89/Bloodborne-android-rebuild-tool#how-to-use-it".equals(NativeCpu.GUIDE_URL), "the two links");
        }

        // refusals (each leaves the installed pair alone)
        File b2 = temp(); bootFile(b2, image);
        refuses("ABI 2", "ABI version 2", tryInstall(b2, json("CUSA00207", "01.09", hash, good, 2, 1), good, "CUSA00207", "01.09"));
        refuses("format 2", "format 2", tryInstall(b2, json("CUSA00207", "01.09", hash, good, 1, 2), good, "CUSA00207", "01.09"));
        refuses("not json", "not readable", tryInstall(b2, "hello", good, null, null));
        refuses("not a bb-recomp file", "not a bb-recomp", tryInstall(b2, "{\"a\": 1}", good, null, null));
        // (b2 has its game image: the hash decides, whatever the ids say; without an image the version and the title id are compared)
        check(tryInstall(temp(), json("CUSA00207", "01.09", hash, good, 1, 1), good, "CUSA03173", "01.09") == null, "another region of the same 1.09, game not prepared: accepted");
        File b2b = temp(); bootFile(b2b, image);
        check(tryInstall(b2b, json("CUSA00207", "01.09", hash, good, 1, 1), good, "CUSA03173", "01.09") == null, "same image, another title id: accepted");
        refuses("other title", "not a Bloodborne", tryInstall(temp(), json("CUSA12345", "01.09", hash, good, 1, 1), good, null, null));
        refuses("library from 1.08", "version 01.08", tryInstall(temp(), json("CUSA00207", "01.08", hash, good, 1, 1), good, null, null));
        refuses("another version on the phone", "version 01.08", tryInstall(temp(), json("CUSA00207", "01.09", hash, good, 1, 1), good, "CUSA00207", "01.08"));
        refuses("another image", "another build", tryInstall(b2, json("CUSA00207", "01.09", "ffffffffffffffff", good, 1, 1), good, "CUSA00207", "01.09"));
        byte[] x86 = so(62, 100000);
        refuses("x86 library", "not for ARM64", tryInstall(b2, json("CUSA00207", "01.09", hash, x86, 1, 1), x86, "CUSA00207", "01.09"));
        byte[] text = new byte[100000]; java.util.Arrays.fill(text, (byte) 'x');
        refuses("not ELF", "not an ELF", tryInstall(b2, json("CUSA00207", "01.09", hash, text, 1, 1), text, "CUSA00207", "01.09"));
        byte[] shortSo = java.util.Arrays.copyOf(good, 99000);
        refuses("truncated", "99000 bytes", tryInstall(b2, json("CUSA00207", "01.09", hash, good, 1, 1), shortSo, "CUSA00207", "01.09"));
        byte[] flipped = good.clone(); flipped[5000] ^= 1;
        refuses("flipped byte", "checksum", tryInstall(b2, json("CUSA00207", "01.09", hash, good, 1, 1), flipped, "CUSA00207", "01.09"));
        byte[] longer = java.util.Arrays.copyOf(good, 100500);
        refuses("larger than json", "larger", tryInstall(b2, json("CUSA00207", "01.09", hash, good, 1, 1), longer, "CUSA00207", "01.09"));
        check(!NativeCpu.so(b2).exists() && !NativeCpu.json(b2).exists() && !new File(NativeCpu.dir(b2), "libbbaot.so.part").exists(), "nothing installed after refusals");

        // game not prepared yet and no game folder: accepted, the check happens later
        File b3 = temp();
        check(tryInstall(b3, goodJson, good, null, null) == null, "no image, no game: accepted");

        // a second import replaces the pair; a wrong json size marks the files as not belonging together
        byte[] other = so(183, 120000);
        check(tryInstall(b3, json("CUSA00207", "01.09", hash, other, 1, 1), other, null, null) == null && NativeCpu.so(b3).length() == 120000, "re-import replaces");
        Files.write(NativeCpu.json(b3).toPath(), goodJson.getBytes(StandardCharsets.UTF_8));
        s = NativeCpu.state(b3, null, null, null);
        check(s.problem != null && s.problem.contains("not the one libbbaot.json describes") && !s.ready() && s.blocker() != null, "so and json of different builds");
        Files.delete(NativeCpu.json(b3).toPath());
        s = NativeCpu.state(b3, null, null, null);
        check(s.problem != null && s.problem.contains("json is missing"), "json missing");
        NativeCpu.remove(b3);
        s = NativeCpu.state(b3, null, null, null);
        check(!s.installed && !s.ready() && "libbbaot.so is missing.".equals(s.blocker()) && "Required: not installed".equals(NativeCpu.summary(s)), "removed: the game cannot start");

        // the game folder's aot/ (made by bb-recomp --pkgs)
        File phone = temp(), game = temp(), aot = new File(game, "aot");
        bootFile(phone, image);
        check(NativeCpu.syncFromGame(phone, game, "CUSA03173", "01.09", null).kind == NativeCpu.SyncKind.NONE, "no aot/ in the game folder");
        aot.mkdirs();
        Files.write(new File(aot, "libbbaot.so").toPath(), good); Files.write(new File(aot, "libbbaot.json").toPath(), goodJson.getBytes(StandardCharsets.UTF_8));
        NativeCpu.Sync sy = NativeCpu.syncFromGame(phone, game, "CUSA03173", "01.09", null);
        check(sy.kind == NativeCpu.SyncKind.IMPORTED && NativeCpu.so(phone).length() == good.length && sy.info.unitCount == 185934, "imported from the game folder");
        long stamp = NativeCpu.so(phone).lastModified(); Thread.sleep(20);
        check(NativeCpu.syncFromGame(phone, game, "CUSA03173", "01.09", null).kind == NativeCpu.SyncKind.UNCHANGED && NativeCpu.so(phone).lastModified() == stamp, "not copied again");
        Files.write(new File(aot, "libbbaot.so").toPath(), other); Files.write(new File(aot, "libbbaot.json").toPath(), json("CUSA00207", "01.09", hash, other, 1, 1).getBytes(StandardCharsets.UTF_8));
        check(NativeCpu.syncFromGame(phone, game, "CUSA03173", "01.09", null).kind == NativeCpu.SyncKind.IMPORTED && NativeCpu.so(phone).length() == 120000, "copied again when it changed");
        Files.write(new File(aot, "libbbaot.so").toPath(), flipped);
        sy = NativeCpu.syncFromGame(phone, game, "CUSA03173", "01.09", null);
        check(sy.kind == NativeCpu.SyncKind.FAILED && NativeCpu.so(phone).length() == 120000, "a damaged pair is reported and the installed one stays");
        Files.write(new File(aot, "libbbaot.json").toPath(), json("CUSA00207", "01.09", hash, good, 2, 1).getBytes(StandardCharsets.UTF_8));
        sy = NativeCpu.syncFromGame(phone, game, "CUSA03173", "01.09", null);
        check(sy.kind == NativeCpu.SyncKind.FAILED && sy.problem.contains("ABI version 2"), "ABI mismatch in the game folder is reported");

        // the "Native code needed" dialog: when it is due and what it leads with (what MainActivity.startGame does with a game folder)
        File p1 = temp(); bootFile(p1, image);
        File g1 = temp();
        check(NativeCpu.NEEDED_MISSING.equals(needed(p1, g1)), "no native code anywhere: the folder has none");
        check(needed(p1, null) == null, "no game folder: no dialog");
        File a1 = new File(g1, "aot"); a1.mkdirs();
        Files.write(new File(a1, "libbbaot.so").toPath(), good);
        check(needed(p1, g1).contains("libbbaot.so but no libbbaot.json") && needed(p1, g1).contains("both are needed"), "only the library in aot/");
        new File(a1, "libbbaot.so").delete();
        Files.write(new File(a1, "libbbaot.json").toPath(), goodJson.getBytes(StandardCharsets.UTF_8));
        check(needed(p1, g1).contains("libbbaot.json but no libbbaot.so"), "only the description in aot/");
        Files.write(new File(a1, "libbbaot.so").toPath(), good);
        check(needed(p1, g1) == null && NativeCpu.state(p1, hash, null, null).ready(), "a good aot/: imported, no dialog");
        check(needed(p1, g1) == null, "good aot/, imported already: no dialog");
        File g2 = temp(); // no aot/ at all, but the library was imported earlier: fine
        check(needed(p1, g2) == null, "folder without aot/, library installed in the app and fitting: no dialog");
        Files.write(new File(a1, "libbbaot.json").toPath(), json("CUSA00207", "01.09", "0123456789abcdef", good, 1, 1).getBytes(StandardCharsets.UTF_8));
        check(needed(p1, g1) == null, "bad aot/ but a fitting library installed: no dialog");

        File p2 = temp(); bootFile(p2, image);
        File g3 = temp(), a3 = new File(g3, "aot"); a3.mkdirs();
        Files.write(new File(a3, "libbbaot.so").toPath(), good);
        Files.write(new File(a3, "libbbaot.json").toPath(), json("CUSA00207", "01.09", "0123456789abcdef", good, 1, 1).getBytes(StandardCharsets.UTF_8));
        String lead = needed(p2, g3);
        check(lead != null && lead.contains("not used") && lead.contains("another build of the game's code"), "aot/ built from another game build: that reason");
        Files.write(new File(a3, "libbbaot.json").toPath(), json("CUSA00207", "01.09", hash, good, 2, 1).getBytes(StandardCharsets.UTF_8));
        lead = needed(p2, g3);
        check(lead != null && lead.contains("ABI version 2"), "aot/ with another ABI version: that reason");
        Files.write(new File(a3, "libbbaot.json").toPath(), goodJson.getBytes(StandardCharsets.UTF_8));
        Files.write(new File(a3, "libbbaot.so").toPath(), flipped);
        lead = needed(p2, g3);
        check(lead != null && lead.contains("checksum"), "aot/ with a damaged library: the checksum reason");
        Files.write(new File(a3, "libbbaot.so").toPath(), good);
        check(needed(p2, g3) == null && NativeCpu.so(p2).isFile(), "fixed aot/: imported, no dialog");

        File p3 = temp(); // a library kept in the app that is for another game build, and a folder without aot/
        bootFile(p3, image);
        check(tryInstall(p3, goodJson, good, null, null) == null, "installed");
        File boot3 = NativeCpu.bootImage(p3); byte[] other3 = image.clone(); other3[100] ^= 1; bootFile(p3, other3);
        boot3.setLastModified(boot3.lastModified() + 5000);
        lead = needed(p3, temp());
        check(lead != null && lead.contains("does not match this game"), "installed library for another build, folder without aot/: that reason");
        Files.write(new File(p3, "bionic-local.sh").toPath(), "export BB_AOT=/x/libbbaot.so\n".getBytes(StandardCharsets.UTF_8));
        check(needed(p3, temp()) == null, "BB_AOT set by hand: no dialog");

        System.out.println(checks + " checks, " + failed + " failed");
        if (failed != 0) System.exit(1);
    }
}
