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
        Map<String, String> ini = new HashMap<>();
        NativeCpu.State s = NativeCpu.state(base, ini, hash, "CUSA00207", "01.09");
        check(s.installed && s.info != null && s.problem == null && s.match == NativeCpu.Match.MATCH && s.enabled && s.active(), "state: installed, matches, active");
        check(s.info.unitCount == 185934 && "CUSA00207".equals(s.info.gameId) && hash.equals(s.info.imageHash), "info fields");
        check("On".equals(NativeCpu.summary(s)), "summary On");
        ini.put("native_cpu", "0");
        s = NativeCpu.state(base, ini, hash, "CUSA00207", "01.09");
        check(!s.enabled && !s.active() && "Installed, off".equals(NativeCpu.summary(s)), "switch off");
        ini.put("native_cpu", "1");
        s = NativeCpu.state(base, ini, null, null, null);
        check(s.match == NativeCpu.Match.UNKNOWN && s.active() && s.matchNote != null, "no image yet: unknown, still usable");
        s = NativeCpu.state(base, ini, "0123456789abcdef", "CUSA00207", "01.09");
        check(s.match == NativeCpu.Match.MISMATCH && !s.active() && s.matchNote.contains("another build"), "image mismatch: not active");
        Files.write(new File(base, "bionic-local.sh").toPath(), "#!/bin/sh\n# export BB_AOT=0\nexport BB_AOT=0\n".getBytes(StandardCharsets.UTF_8));
        s = NativeCpu.state(base, ini, hash, null, null);
        check(s.manualOverride && !s.active() && "Set by bionic-local.sh".equals(NativeCpu.summary(s)), "bionic-local.sh BB_AOT wins");
        Files.write(new File(base, "bionic-local.sh").toPath(), "# export BB_AOT=0\nexport BB_X=1\n".getBytes(StandardCharsets.UTF_8));
        check(!NativeCpu.manualOverride(base), "a commented BB_AOT is not an override");

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
        s = NativeCpu.state(b3, ini, null, null, null);
        check(s.problem != null && s.problem.contains("not the one libbbaot.json describes") && !s.active(), "so and json of different builds");
        Files.delete(NativeCpu.json(b3).toPath());
        s = NativeCpu.state(b3, ini, null, null, null);
        check(s.problem != null && s.problem.contains("json is missing"), "json missing");
        NativeCpu.remove(b3);
        s = NativeCpu.state(b3, ini, null, null, null);
        check(!s.installed && "Not installed".equals(NativeCpu.summary(s)), "removed");

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

        System.out.println(checks + " checks, " + failed + " failed");
        if (failed != 0) System.exit(1);
    }
}
