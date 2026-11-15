package com.ducvd89.bloodborne;

import static com.ducvd89.bloodborne.T.*;

import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;

/** Host-JDK tests of the Java mod and PKG code: ports of tests/test_mods_android.py, tests/test_mods.py and
 *  tests/test_game_check.py. Run: tests/android-mods/run.sh */
public final class ModsTest {
    static final String NOT_A_MOD = "Not a Bloodborne mod: no dvdroot_ps4 or game folders inside";
    static final BooleanSupplier NEVER = () -> false;

    // ---------------------------------------------------------------- mods_android: list / validate / apply

    /** A temp dir with a game, a mods folder, a config. */
    static final class Env {
        final File root, game, mods, config, out;
        Env() throws IOException {
            root = temp(); game = new File(root, "game"); mods = new File(root, "mods");
            config = new File(root, "mods.json"); out = new File(root, "mod-game");
            write(new File(game, "eboot.bin"), "eboot");
            write(new File(game, "dvdroot_ps4/chr/a.dcx"), "orig");
            mods.mkdirs();
        }
        File put(String mod, String relative, String data) throws IOException { File f = new File(mods, mod + "/" + relative); write(f, data); return f; }
        File put(String mod, String relative) throws IOException { return put(mod, relative, "x"); }
        ModManager manager() { return new ModManager(mods, config); }
        ModManager.Listing listing() { return manager().list(); }
        Map<String, ModManager.Mod> rows() {
            Map<String, ModManager.Mod> out = new LinkedHashMap<>();
            for (ModManager.Mod m : listing().mods) out.put(m.name, m);
            return out;
        }
        /** (folder to run, log lines). */
        Object[] apply() {
            List<String> log = new ArrayList<>();
            String folder = manager().apply(game, out, log::add);
            return new Object[]{new File(folder), String.join("\n", log)};
        }
        List<String> names(ModManager.Mod m, boolean overrides) {
            List<String> out = new ArrayList<>();
            for (ModManager.Shared s : overrides ? m.overrides : m.overriddenBy) out.add(s.mod + ":" + s.files);
            return out;
        }
        void close() { delete(root); }
    }
    interface WithEnv { void run(Env e) throws Exception; }
    static void env(String name, WithEnv body) { test(name, () -> { Env e = new Env(); try { body.run(e); } finally { e.close(); } }); }

    static void modsAndroidTests() {
        env("layouts_and_counts", e -> {
            e.put("plain", "dvdroot_ps4/chr/a.dcx", "12345");
            e.put("plain", "dvdroot_ps4/chr/b.dcx", "123");
            e.put("serial", "CUSA03173/dvdroot_ps4/parts/p.dcx");
            e.put("bare", "chr/c.dcx");
            e.put("wrapper", "inner/dvdroot_ps4/msg/m.fmg");
            Map<String, ModManager.Mod> rows = e.rows();
            eq(list("bare", "plain", "serial", "wrapper"), new ArrayList<>(rows.keySet()));
            for (ModManager.Mod m : rows.values()) { yes(m.valid, m.name); yes(m.enabled); eq(null, m.problem); }
            eq(2, rows.get("plain").files); eq(8L, rows.get("plain").bytes);
            eq(1, rows.get("bare").files);
            ModManager.Listing l = e.listing();
            eq(e.mods.getAbsolutePath(), l.modsDir); yes(l.exists); eq(null, l.error);
        });
        env("non_mod_folder_listed_invalid", e -> {
            e.put("docs", "readme.txt");
            new File(e.mods, "empty").mkdirs();
            e.put("real", "dvdroot_ps4/chr/a.dcx");
            write(new File(e.mods, "loose.zip"), "file, not a folder");
            Map<String, ModManager.Mod> rows = e.rows();
            eq(list("real", "docs", "empty"), new ArrayList<>(rows.keySet()));
            for (String n : list("docs", "empty")) { no(rows.get(n).valid); no(rows.get(n).enabled); eq(NOT_A_MOD, rows.get(n).problem); }
        });
        env("system_files_make_mod_invalid", e -> {
            e.put("exe", "dvdroot_ps4/chr/a.dcx"); e.put("exe", "eboot.bin");
            e.put("sys", "dvdroot_ps4/chr/a.dcx"); e.put("sys", "sce_sys/param.sfo");
            e.put("module", "dvdroot_ps4/chr/a.dcx"); e.put("module", "sce_module/libc.prx");
            e.put("sfo", "dvdroot_ps4/chr/a.dcx"); e.put("sfo", "param.sfo");
            e.put("wrapped", "CUSA03173/dvdroot_ps4/chr/a.dcx"); e.put("wrapped", "CUSA03173/EBOOT.BIN");
            e.put("prx", "dvdroot_ps4/chr/a.dcx"); e.put("prx", "patch.sprx");
            e.put("good", "dvdroot_ps4/chr/b.dcx"); e.put("good", "readme.txt");
            Map<String, ModManager.Mod> rows = e.rows();
            for (String n : list("exe", "sys", "module", "sfo", "wrapped", "prx")) {
                no(rows.get(n).valid); contains(rows.get(n).problem, "1.09"); eq(0, rows.get(n).files);
            }
            contains(rows.get("exe").problem, "eboot.bin");
            contains(rows.get("sys").problem, "sce_sys");
            yes(rows.get("good").valid);
            Object[] r = e.apply(); File result = (File)r[0]; String log = (String)r[1];
            contains(log, "Mods: 1 applied: good");
            contains(log, "exe: invalid, skipped");
            eq(list("a.dcx", "b.dcx"), names(new File(result, "dvdroot_ps4/chr")));
            eq("orig", text(new File(result, "dvdroot_ps4/chr/a.dcx")));
        });
        env("case_insensitive_conflicts_both_ways", e -> {
            e.put("A", "dvdroot_ps4/chr/Texture.DCX", "a"); e.put("A", "dvdroot_ps4/chr/only_a.dcx");
            e.put("B", "dvdroot_ps4/CHR/texture.dcx", "bb"); e.put("B", "dvdroot_ps4/chr/b.dcx");
            e.put("C", "dvdroot_ps4/chr/texture.dcx", "ccc");
            Map<String, ModManager.Mod> rows = e.rows();
            eq(list("B:1", "C:1"), e.names(rows.get("A"), false));
            eq(list(), e.names(rows.get("A"), true));
            eq(list("A:1"), e.names(rows.get("B"), true));
            eq(list("C:1"), e.names(rows.get("B"), false));
            eq(list("A:1", "B:1"), e.names(rows.get("C"), true));
            write(e.config, "{\"order\": [\"C\", \"B\", \"A\"]}");
            rows = e.rows();
            eq(list("C:1", "B:1"), e.names(rows.get("A"), true));
            eq(list("B:1", "A:1"), e.names(rows.get("C"), false));
        });
        env("disabled_and_invalid_mods_do_not_conflict", e -> {
            e.put("A", "dvdroot_ps4/chr/a.dcx"); e.put("B", "dvdroot_ps4/chr/a.dcx");
            e.put("Bad", "dvdroot_ps4/chr/a.dcx"); e.put("Bad", "eboot.bin");
            write(e.config, "{\"order\": [\"A\", \"B\", \"Bad\"], \"disabled\": [\"B\"]}");
            Map<String, ModManager.Mod> rows = e.rows();
            no(rows.get("B").enabled); eq(1, rows.get("B").files);
            eq(list(), e.names(rows.get("A"), false));
            eq(list(), e.names(rows.get("B"), true));
            eq(list(), e.names(rows.get("Bad"), true));
        });
        env("config_order_and_new_mod", e -> {
            for (String n : list("A", "B", "C")) e.put(n, "dvdroot_ps4/chr/" + n + ".dcx");
            write(e.config, "{\"order\": [\"C\", \"gone\", \"A\"], \"disabled\": [\"A\"]}");
            List<ModManager.Mod> rows = e.listing().mods;
            eq(list("C", "A", "B"), modNames(rows));
            eq(list(true, false, true), enabledOf(rows));
        });
        env("bad_config_reports_error", e -> {
            e.put("A", "dvdroot_ps4/chr/a.dcx");
            write(e.config, "{not json");
            ModManager.Listing l = e.listing();
            yes(l.error != null); eq(0, l.mods.size());
            contains(l.error, "mods.json");
            eq(e.game, ((File)e.apply()[0])); // unmodded start
            for (String bad : list("[]", "{\"order\": \"A\"}", "{\"disabled\": [1]}", "{\"order\": null}")) {
                write(e.config, bad);
                yes(e.listing().error != null, bad);
            }
            write(e.config, "{\"order\": [], \"extra\": 5}");
            eq(null, e.listing().error);
        });
        env("missing_mods_dir", e -> {
            ModManager.Listing l = new ModManager(new File(e.root, "nope"), e.config).list();
            eq(new File(e.root, "nope").getAbsolutePath(), l.modsDir); no(l.exists); eq(0, l.mods.size()); eq(null, l.error);
        });
        env("validate_one", e -> {
            e.put("Bad", "dvdroot_ps4/chr/a.dcx"); e.put("Bad", "sce_module/x.prx");
            e.put("Good", "dvdroot_ps4/chr/a.dcx", "1234");
            ModManager.Mod bad = e.manager().validate("Bad"), good = e.manager().validate("Good"), missing = e.manager().validate("Nope");
            no(bad.valid); contains(bad.problem, "sce_module");
            yes(good.valid); eq(1, good.files); eq(4L, good.bytes);
            no(missing.valid); eq("ValueError: No such mod folder: Nope", missing.problem);
            eq("ValueError: No such mod folder: ../x", e.manager().validate("../x").problem);
            eq("ValueError: No such mod folder: ..", e.manager().validate("..").problem);
        });
        env("apply_overlay_order_and_skips", e -> {
            e.put("A", "dvdroot_ps4/chr/a.dcx", "A");
            e.put("B", "dvdroot_ps4/chr/a.dcx", "B"); e.put("B", "dvdroot_ps4/chr/new.dcx", "new");
            e.put("Off", "dvdroot_ps4/chr/a.dcx", "Off");
            e.put("Bad", "dvdroot_ps4/chr/a.dcx", "Bad"); e.put("Bad", "eboot.bin");
            write(e.config, "{\"order\": [\"A\", \"B\", \"Off\", \"Bad\"], \"disabled\": [\"Off\"]}");
            Object[] r = e.apply(); File result = (File)r[0]; String log = (String)r[1];
            eq(FsUtil.resolve(e.out), result.getParentFile());
            yes(result.getName().startsWith("mod-game-"));
            eq("B", text(new File(result, "dvdroot_ps4/chr/a.dcx")));
            eq("new", text(new File(result, "dvdroot_ps4/chr/new.dcx")));
            eq("eboot", text(new File(result, "eboot.bin")));
            eq("orig", text(new File(e.game, "dvdroot_ps4/chr/a.dcx")));
            contains(log, "Mods: 2 applied: A, B");
            contains(log, "Off: disabled, skipped");
            contains(log, "Bad: invalid, skipped");
            contains(log, "Mods: dvdroot_ps4/chr/a.dcx: A -> B");
            contains(log, "Mods: 1 game files replaced, 1 added");
        });
        env("apply_removes_stale_overlays", e -> {
            e.put("A", "dvdroot_ps4/chr/a.dcx", "A");
            File stale = new File(e.out, "mod-game-stale/dvdroot_ps4"); stale.mkdirs();
            Files.createSymbolicLink(new File(stale, "x").toPath(), new File(e.game, "eboot.bin").toPath());
            write(new File(e.out, "junk.txt"), "old");
            File first = (File)e.apply()[0];
            eq(list(first.getName()), names(e.out));
            File second = (File)e.apply()[0];
            eq(list(second.getName()), names(e.out));
            yes(new File(e.game, "eboot.bin").exists()); // links were removed, not followed
        });
        env("apply_without_valid_mods_returns_game_and_clears", e -> {
            e.put("Bad", "dvdroot_ps4/chr/a.dcx"); e.put("Bad", "eboot.bin"); e.put("Off", "dvdroot_ps4/chr/a.dcx");
            write(e.config, "{\"disabled\": [\"Off\"]}");
            new File(e.out, "mod-game-stale").mkdirs();
            Object[] r = e.apply();
            eq(e.game, r[0]); eq(0, names(e.out).size()); contains((String)r[1], "Mods: none");
        });
        env("apply_missing_mods_dir_and_refuses_to_clear_game", e -> {
            List<String> log = new ArrayList<>();
            eq(e.game.getPath(), new ModManager(new File(e.root, "nope"), e.config).apply(e.game, e.out, log::add));
            log.clear();
            eq(e.game.getPath(), e.manager().apply(e.game, e.game, log::add));
            contains(String.join("\n", log), "Refusing");
            yes(new File(e.game, "eboot.bin").exists());
        });
        env("selection_agrees_with_the_list", e -> {
            for (String n : list("A", "B", "C")) e.put(n, "dvdroot_ps4/chr/a.dcx");
            write(e.config, "{\"order\": [\"C\"], \"disabled\": [\"B\"]}");
            List<String> enabled = new ArrayList<>();
            for (ModManager.Mod m : e.listing().mods) if (m.enabled) enabled.add(m.name);
            eq(enabled, ModLoader.selected(e.mods, e.config));
        });
        env("dot_folders_are_never_mods", e -> {
            e.put(".staging-dead", "CUSA03173/dvdroot_ps4/msg/m.fmg");
            eq(0, e.listing().mods.size());
            e.put("Real", "dvdroot_ps4/chr/a.dcx");
            Object[] r = e.apply();
            eq(1, e.listing().mods.size());
            contains((String)r[1], "Mods: 1 applied: Real");
        });
    }
    static List<String> modNames(List<ModManager.Mod> mods) { List<String> o = new ArrayList<>(); for (ModManager.Mod m : mods) o.add(m.name); return o; }
    static List<Boolean> enabledOf(List<ModManager.Mod> mods) { List<Boolean> o = new ArrayList<>(); for (ModManager.Mod m : mods) o.add(m.enabled); return o; }

    // ---------------------------------------------------------------- scripts/mods.py (tests/test_mods.py)

    static final class Game {
        final File root, game, assets, moddir;
        Game() throws IOException {
            root = temp(); game = new File(root, "CUSA03173"); assets = new File(game, "dvdroot_ps4/chr"); moddir = new File(root, "mods");
            assets.mkdirs();
            write(new File(game, "eboot.bin"), "original executable");
            write(new File(assets, "a.dcx"), "original");
            write(new File(assets, "b.dcx"), "untouched");
        }
        File mod(String name, String contents, String file) throws IOException {
            File root = new File(moddir, name);
            write(new File(root, "dvdroot_ps4/chr/" + file), contents);
            return root;
        }
        File mod(String name) throws IOException { return mod(name, "mod", "a.dcx"); }
        File overlay(List<ModLoader.Layer> layers) throws IOException { return ModLoader.buildOverlay(game, new File(root, "out"), layers, s -> {}); }
        List<ModLoader.Layer> layer(Object... pairs) { List<ModLoader.Layer> l = new ArrayList<>(); for (int i = 0; i < pairs.length; i += 2) l.add(new ModLoader.Layer((String)pairs[i], (File)pairs[i + 1])); return l; }
    }
    interface WithGame { void run(Game g) throws Exception; }
    static void game(String name, WithGame body) { test(name, () -> { Game g = new Game(); try { body.run(g); } finally { delete(g.root); } }); }

    static void modsPyTests() {
        game("merge_replacement_new_file_and_directory_listing", g -> {
            File a = g.mod("A"); g.mod("A", "new", "new.dcx");
            File result = g.overlay(g.layer("A", a));
            File folder = new File(result, "dvdroot_ps4/chr");
            eq("mod", text(new File(folder, "a.dcx"))); eq("untouched", text(new File(folder, "b.dcx"))); eq("new", text(new File(folder, "new.dcx")));
            eq(list("a.dcx", "b.dcx", "new.dcx"), names(folder));
            eq("original", text(new File(g.assets, "a.dcx"))); no(new File(g.assets, "new.dcx").exists());
            eq("original executable", text(new File(result, "eboot.bin")));
        });
        game("last_mod_wins_and_reordering_changes_winner", g -> {
            File a = g.mod("A", "A", "a.dcx"), b = g.mod("B", "B", "a.dcx");
            eq("B", text(new File(g.overlay(g.layer("A", a, "B", b)), "dvdroot_ps4/chr/a.dcx")));
            eq("A", text(new File(g.overlay(g.layer("B", b, "A", a)), "dvdroot_ps4/chr/a.dcx")));
        });
        game("selection_disable_and_new_mod", g -> {
            for (String n : list("C", "B", "A")) g.mod(n);
            File config = new File(g.root, "mods.json");
            write(config, "{\"order\": [\"B\", \"A\", \"deleted\"], \"disabled\": [\"A\"]}");
            eq(list("B", "C"), ModLoader.selected(g.moddir, config));
        });
        game("no_mod_returns_original_game", g -> {
            eq(FsUtil.resolve(g.game), g.overlay(new ArrayList<>()));
            no(new File(g.root, "out").exists());
        });
        game("directory_conflict_does_not_modify_base", g -> {
            File a = g.mod("A");
            new File(a, "dvdroot_ps4/chr/a.dcx").delete();
            File nested = new File(a, "dvdroot_ps4/chr/b.dcx"); nested.mkdirs();
            write(new File(nested, "child"), "bad");
            try { g.overlay(g.layer("A", a)); throw new AssertionError("no error"); }
            catch (ModLoader.ModError expected) { /* ValueError */ }
            eq("untouched", text(new File(g.assets, "b.dcx")));
            eq(0, names(new File(g.root, "out")).size());
        });
        game("mod_symlinks_rejected", g -> {
            File a = g.mod("A");
            Files.createSymbolicLink(new File(a, "dvdroot_ps4/escape").toPath(), g.assets.toPath());
            try { g.overlay(g.layer("A", a)); throw new AssertionError("no error"); }
            catch (ModLoader.ModError expected) { contains(expected.getMessage(), "Mod symlinks are unsupported"); }
        });
        game("executable_replacement_rejected", g -> {
            File a = g.mod("A");
            write(new File(a, "eboot.bin"), "unsupported");
            try { g.overlay(g.layer("A", a)); throw new AssertionError("no error"); }
            catch (ModLoader.ModError expected) { contains(expected.getMessage(), "Executable/system replacement is unsupported"); }
        });
        game("app0_wrapped_mod_discovered", g -> {
            new File(g.moddir, "Wrapped/app0/dvdroot_ps4").mkdirs();
            eq(list("Wrapped"), ModLoader.discover(g.moddir));
        });
        game("other_case_replaces_the_game_file", g -> {
            File root = new File(g.moddir, "Upper");
            write(new File(root, "DVDROOT_PS4/Chr/A.DCX"), "upper");
            File result = g.overlay(g.layer("Upper", root));
            File folder = new File(result, "dvdroot_ps4/chr");
            eq("upper", text(new File(folder, "a.dcx")));
            eq(list("a.dcx", "b.dcx"), names(folder));
            eq(list("dvdroot_ps4", "eboot.bin"), names(result));
        });
        game("wrapped_and_bare_layouts", g -> {
            Map<String, String> layouts = new LinkedHashMap<>();
            layouts.put("Title", "CUSA03173/dvdroot_ps4/chr"); layouts.put("Archive", "Archive v1.2/dvdroot_ps4/chr");
            layouts.put("Bare", "chr"); layouts.put("Nested", "Nested/app0/dvdroot_ps4/chr");
            for (Map.Entry<String, String> l : layouts.entrySet()) write(new File(g.moddir, l.getKey() + "/" + l.getValue() + "/a.dcx"), l.getKey());
            write(new File(g.moddir, "Archive/readme.txt"), "notes");
            new File(g.moddir, "Junk/stuff").mkdirs();
            eq(list("Archive", "Bare", "Nested", "Title"), ModLoader.discover(g.moddir));
            for (String name : layouts.keySet())
                eq(name, text(new File(g.overlay(g.layer(name, new File(g.moddir, name))), "dvdroot_ps4/chr/a.dcx")));
        });
        game("invalid_profile_fails", g -> {
            g.mod("A");
            File config = new File(g.root, "mods.json");
            write(config, "{\"disabled\":\"A\"}");
            try { ModLoader.selected(g.moddir, config); throw new AssertionError("no error"); }
            catch (ModLoader.ModError expected) { eq("Disabled mods must be a list of folder names", expected.getMessage()); }
            write(config, "{\"order\":{}}");
            try { ModLoader.selected(g.moddir, config); throw new AssertionError("no error"); }
            catch (ModLoader.ModError expected) { eq("Mod order must be a list of folder names", expected.getMessage()); }
        });
        game("overlay_links_point_at_game_and_mod_files", g -> {
            File a = g.mod("A", "A", "a.dcx");
            File result = g.overlay(g.layer("A", a));
            yes(Files.isSymbolicLink(new File(result, "eboot.bin").toPath()));
            eq(FsUtil.resolve(new File(g.game, "eboot.bin")), FsUtil.resolve(new File(result, "eboot.bin")));
            eq(FsUtil.resolve(new File(a, "dvdroot_ps4/chr/a.dcx")), FsUtil.resolve(new File(result, "dvdroot_ps4/chr/a.dcx")));
            eq(FsUtil.resolve(new File(g.assets, "b.dcx")), FsUtil.resolve(new File(result, "dvdroot_ps4/chr/b.dcx")));
        });
    }

    // ---------------------------------------------------------------- small pieces

    static byte[] sfo(String... kv) {
        LinkedHashMap<String, String> values = new LinkedHashMap<>();
        for (int i = 0; i < kv.length; i += 2) values.put(kv[i], kv[i + 1]);
        return sfo(values, 0x0204);
    }
    static byte[] sfo(Map<String, String> values, int format) {
        ByteBuffer entries = ByteBuffer.allocate(16 * values.size()).order(ByteOrder.LITTLE_ENDIAN);
        java.io.ByteArrayOutputStream keys = new java.io.ByteArrayOutputStream(), data = new java.io.ByteArrayOutputStream();
        for (Map.Entry<String, String> e : values.entrySet()) {
            byte[] raw = (e.getValue() + "\0").getBytes(StandardCharsets.UTF_8);
            entries.putShort((short)keys.size()).putShort((short)format).putInt(raw.length).putInt(raw.length).putInt(data.size());
            byte[] k = (e.getKey() + "\0").getBytes(StandardCharsets.US_ASCII);
            keys.write(k, 0, k.length);
            data.write(raw, 0, raw.length);
        }
        int keyStart = 20 + entries.capacity(), dataStart = (keyStart + keys.size() + 3) & ~3;
        ByteBuffer out = ByteBuffer.allocate(dataStart + data.size()).order(ByteOrder.LITTLE_ENDIAN);
        out.put(new byte[]{0, 'P', 'S', 'F'}).putInt(0x101).putInt(keyStart).putInt(dataStart).putInt(values.size());
        out.put(entries.array()).put(keys.toByteArray());
        out.position(dataStart);
        out.put(data.toByteArray());
        return out.array();
    }
    static String sfoFile(String title, String version, String category) { return new String(sfo("TITLE_ID", title, "APP_VER", version, "CATEGORY", category), StandardCharsets.ISO_8859_1); }
    static String sfoFile() { return sfoFile("CUSA03173", "01.09", "gp"); }

    static void sfoTests() {
        test("sfo_parse", () -> {
            Map<String, Object> v = Sfo.parse(sfo("TITLE_ID", "CUSA03173", "APP_VER", "01.09", "CATEGORY", "gp"));
            eq("CUSA03173", v.get("TITLE_ID")); eq("01.09", v.get("APP_VER")); eq("gp", v.get("CATEGORY")); eq(3, v.size());
            LinkedHashMap<String, String> ints = new LinkedHashMap<>(); ints.put("N", "abcd"); // 4 bytes + NUL: integer reads the first 4
            Object n = Sfo.parse(sfo(ints, 0x0404)).get("N");
            eq(0x64636261L, n);
            for (byte[] bad : new byte[][]{new byte[0], {0, 'P', 'S', 'F'}, "nopenopenopenopenopenopenopenopenope".getBytes()}) {
                try { Sfo.parse(bad); throw new AssertionError("no error"); } catch (Sfo.Invalid expected) { contains(expected.getMessage(), "param.sfo: "); }
            }
        });
        test("minijson", () -> {
            Object o = MiniJson.parse("{\"order\": [\"A\", \"Bé\", \"x\\u00e9\\n\"], \"n\": -1.5e2, \"t\": true, \"z\": null, \"i\": 12}");
            Map<?, ?> m = (Map<?, ?>)o;
            eq(list("A", "Bé", "xé\n"), m.get("order")); eq(-150.0, m.get("n")); eq(true, m.get("t")); eq(null, m.get("z")); eq(12L, m.get("i"));
            String[][] errors = {{"{not json", "Expecting property name enclosed in double quotes: line 1 column 2 (char 1)"},
                {"", "Expecting value: line 1 column 1 (char 0)"}, {"{} x", "Extra data: line 1 column 4 (char 3)"},
                {"[1,]", "Expecting value: line 1 column 4 (char 3)"}, {"{\"a\" 1}", "Expecting ':' delimiter: line 1 column 6 (char 5)"},
                {"[1 2]", "Expecting ',' delimiter: line 1 column 4 (char 3)"}, {"\"abc", "Unterminated string starting at: line 1 column 1 (char 0)"}};
            for (String[] e : errors) {
                try { MiniJson.parse(e[0]); throw new AssertionError("no error for " + e[0]); }
                catch (MiniJson.ParseException x) { eq(e[1], x.getMessage()); }
            }
        });
        test("gigabytes_rounds_like_python", () -> {
            eq("1.2 GB", PkgInstaller.gigabytes((5L << 28)));          // 1.25 -> half to even
            eq("0.2 GB", PkgInstaller.gigabytes(1L << 28));            // 0.25
            eq("0.8 GB", PkgInstaller.gigabytes(3L << 28));            // 0.75
            eq("2.0 GB", PkgInstaller.gigabytes(2L << 30));
            eq("0.0 GB", PkgInstaller.gigabytes(0));
        });
    }

    // ---------------------------------------------------------------- fake PKG tool

    static byte[] makePkg(String contentId, byte[] sfo) { return makePkg(contentId, sfo, 4096); }
    static byte[] makePkg(String contentId, byte[] sfo, int padding) {
        byte[] head = new byte[0x440];
        head[0] = 0x7f; head[1] = 'C'; head[2] = 'N'; head[3] = 'T';
        byte[] id = contentId.getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(id, 0, head, 0x40, id.length);
        ByteBuffer h = ByteBuffer.wrap(head).order(ByteOrder.BIG_ENDIAN);
        h.putInt(0x74, 0x1a);
        byte[] entries = new byte[0];
        if (sfo != null) {
            h.putInt(0x10, 1); h.putInt(0x18, 0x440);
            ByteBuffer e = ByteBuffer.allocate(32).order(ByteOrder.BIG_ENDIAN);
            e.putInt(0, 0x1000); e.putInt(16, 0x440 + 32); e.putInt(20, sfo.length);
            entries = e.array();
        }
        byte[] body = new byte[0x440 + entries.length + (sfo == null ? 0 : sfo.length) + padding];
        System.arraycopy(head, 0, body, 0, head.length);
        System.arraycopy(entries, 0, body, 0x440, entries.length);
        if (sfo != null) System.arraycopy(sfo, 0, body, 0x440 + entries.length, sfo.length);
        ByteBuffer.wrap(body).order(ByteOrder.BIG_ENDIAN).putLong(0x430, body.length);
        return body;
    }
    static File makePkgFile(File path, String contentId, byte[] sfo) throws IOException { write(path, makePkg(contentId, sfo)); return path; }
    static File makePkgFile(File path, byte[] sfo) throws IOException { return makePkgFile(path, "EP9000-CUSA03173_00-BLOODBORNE0000EU", sfo); }

    /** A shell script standing in for ps4-pkg-tool; what it does is in the plan folder beside it. */
    static final class FakeTool {
        final File dir, tool, plan;
        FakeTool(File dir) throws IOException {
            this.dir = dir; tool = new File(dir, "fake-tool"); plan = new File(dir, "plan");
            write(tool, "#!/bin/sh\nhere=$(dirname \"$0\"); plan=\"$here/plan\"\n"
                + "echo \"$2\" >> \"$here/tool.log\"\n"
                + "total=100; [ -f \"$plan/total\" ] && total=$(cat \"$plan/total\")\n"
                + "[ -f \"$plan/print_first\" ] && cat \"$plan/print_first\"\n"
                + "echo \"Progress-bytes: 0 $total\"\n"
                + "[ -f \"$plan/sleep\" ] && exec sleep $(cat \"$plan/sleep\")\n"
                + "[ -f \"$plan/signal\" ] && kill -$(cat \"$plan/signal\") $$\n"
                + "d=CUSA03173; [ -f \"$plan/dir\" ] && d=$(cat \"$plan/dir\")\n"
                + "if [ -d \"$plan/files\" ]; then mkdir -p \"$2/$d\" && cp -R \"$plan/files/.\" \"$2/$d/\"; fi\n"
                + "echo \"Progress-bytes: $total $total\"\n"
                + "code=0; [ -f \"$plan/exit\" ] && code=$(cat \"$plan/exit\")\n"
                + "exit $code\n");
            tool.setExecutable(true);
        }
        /** files: relative path -> content; options: total, exit, signal, print_first, dir, sleep (name, value pairs). */
        void plan(Map<String, String> files, String... options) throws IOException {
            delete(plan);
            plan.mkdirs();
            if (files != null) {
                new File(plan, "files").mkdirs();
                for (Map.Entry<String, String> f : files.entrySet()) write(new File(plan, "files/" + f.getKey()), f.getValue().getBytes(StandardCharsets.ISO_8859_1));
            }
            for (int i = 0; i < options.length; i += 2) write(new File(plan, options[i]), options[i + 1]);
        }
        int runs() throws IOException {
            File log = new File(dir, "tool.log");
            return log.exists() ? text(log).split("\n").length : 0;
        }
    }
    static Map<String, String> files(String... kv) {
        Map<String, String> m = new LinkedHashMap<>();
        for (int i = 0; i < kv.length; i += 2) m.put(kv[i], kv[i + 1]);
        return m;
    }
    static Map<String, String> plus(Map<String, String> base, String... kv) {
        Map<String, String> m = new LinkedHashMap<>(base);
        for (int i = 0; i < kv.length; i += 2) m.put(kv[i], kv[i + 1]);
        return m;
    }

    static final class Pkg {
        final File root, mods, game, pkg; final FakeTool tool; final List<String> progress = new ArrayList<>(); final List<String> log = new ArrayList<>();
        PkgInstaller installer;
        Pkg() throws IOException {
            root = temp(); mods = new File(root, "mods"); tool = new FakeTool(root); pkg = new File(root, "a.pkg");
            game = new File(root, "game");
            installer = new PkgInstaller(tool.tool, log::add);
            installer.checker = new PkgInstaller.Checker() {
                @Override public GameCheck.Found problem(File g) { return GameCheck.problem(g, true); }
                @Override public String explain(GameCheck.Found f) { return GameCheck.explain(f); }
            };
        }
        PkgInstaller.Progress sink() { return (d, t, l) -> progress.add("progress " + d + " " + t + " " + l); }
        void close() { delete(root); }
    }

    // ---------------------------------------------------------------- install-pkg

    interface WithPkg { void run(Pkg p) throws Exception; }
    static void pkg(String name, WithPkg body) { test(name, () -> { Pkg p = new Pkg(); try { body.run(p); } finally { p.close(); } }); }

    static PkgInstaller.ModResult installMod(Pkg p, String name, boolean withGame, File pkgFile) {
        return p.installer.installMod(pkgFile != null ? pkgFile : p.pkg, name, p.mods, withGame ? p.game : null, p.sink(), NEVER);
    }
    static PkgInstaller.ModResult installMod(Pkg p, String name) { return installMod(p, name, true, null); }
    static Map<String, String> goodFiles(String... extra) {
        return plus(files("sce_sys/param.sfo", sfoFile(), "dvdroot_ps4/msg/menu.fmg", "text", "dvdroot_ps4/chr/a.dcx", "dcx"), extra);
    }
    static final String RETAIL = "This PKG is encrypted for a retail PS4 and can't be read; only homebrew (fake) PKGs are supported";

    static void installPkgTests() {
        class Setup { static void game(Pkg p) throws IOException {
            write(new File(p.game, "sce_module/libc.prx"), "libc"); write(new File(p.game, "eboot.bin"), "eboot");
            makePkgFile(p.pkg, sfo("TITLE_ID", "CUSA03173", "APP_VER", "01.09", "CATEGORY", "gp"));
        } }
        pkg("pkg_installs_mod_and_cleans_up", p -> {
            Setup.game(p);
            p.tool.plan(goodFiles("eboot.bin", "eboot", "sce_module/libc.prx", "libc", "sce_sys/icon0.png", "x"), "total", "5000");
            PkgInstaller.ModResult r = installMod(p, "Mod");
            yes(r.ok, String.valueOf(r.problem)); eq("Mod", r.name); eq(null, r.problem);
            yes(r.mod.valid); eq(2, r.mod.files); eq("Mod", r.mod.name);
            eq(list("Mod"), names(p.mods));
            eq(list("dvdroot_ps4"), names(new File(p.mods, "Mod")));
            eq("text", text(new File(p.mods, "Mod/dvdroot_ps4/msg/menu.fmg")));
            yes(p.progress.contains("progress 5000 5000 Extracting"), p.progress.toString());
            eq("progress 0 0 Checking", p.progress.get(p.progress.size() - 1));
            eq(list("Mod"), modNames(new ModManager(p.mods, null).list().mods));
        });
        pkg("pkg_name_made_unique_and_bad_names_refused", p -> {
            Setup.game(p);
            p.tool.plan(goodFiles());
            List<String> names = new ArrayList<>();
            for (int i = 0; i < 3; ++i) names.add(installMod(p, "Mod").name);
            eq(list("Mod", "Mod (2)", "Mod (3)"), names);
            eq("mod (4)", installMod(p, "mod").name);
            int runs = p.tool.runs();
            for (String bad : list("a/b", ".hidden", "", "..", "x\\y")) {
                PkgInstaller.ModResult r = installMod(p, bad);
                no(r.ok, bad); eq(null, r.name); contains(r.problem, "name");
            }
            eq(runs, p.tool.runs());
            eq("Tiếng Việt", installMod(p, "Tiếng Việt").name);
        });
        pkg("pkg_wrong_version_and_title_refused_before_unpacking", p -> {
            Setup.game(p);
            p.tool.plan(goodFiles());
            makePkgFile(p.pkg, sfo("TITLE_ID", "CUSA03173", "APP_VER", "01.00", "CATEGORY", "gd"));
            PkgInstaller.ModResult r = installMod(p, "Mod");
            eq("This PKG is for CUSA03173 version 01.00; only Bloodborne 1.09 mods are supported", r.problem);
            no(r.ok); eq(null, r.name); eq(null, r.mod);
            makePkgFile(p.pkg, "EP0000-CUSA12345_00-X", sfo("TITLE_ID", "CUSA12345", "APP_VER", "01.09"));
            contains(installMod(p, "Mod").problem, "CUSA12345 version 01.09");
            eq(0, p.tool.runs()); no(p.mods.exists());
        });
        pkg("pkg_identity_checked_again_on_the_extracted_param_sfo", p -> {
            Setup.game(p);
            makePkgFile(p.pkg, null); // no param.sfo entry in the PKG: only the content ID is known
            p.tool.plan(goodFiles("sce_sys/param.sfo", sfoFile("CUSA03173", "01.06", "gp")));
            contains(installMod(p, "Mod").problem, "version 01.06");
            p.tool.plan(goodFiles("sce_sys/param.sfo", sfoFile("CUSA99999", "01.09", "gp")));
            contains(installMod(p, "Mod").problem, "This PKG is for CUSA99999");
            eq(0, names(p.mods).size());
        });
        pkg("pkg_executable_must_be_the_games", p -> {
            Setup.game(p);
            p.tool.plan(goodFiles("eboot.bin", "other eboot"));
            PkgInstaller.ModResult r = installMod(p, "Mod");
            no(r.ok); eq("This PKG changes the game's executable (eboot.bin), which isn't supported", r.problem);
            eq(0, names(p.mods).size());
            p.tool.plan(goodFiles("eboot.bin", "eboot")); // same
            yes(installMod(p, "Mod").ok);
            p.tool.plan(goodFiles("eboot.bin", "eboot", "sce_module/libc.prx", "patched"));
            contains(installMod(p, "M2").problem, "modules (sce_module/libc.prx)");
            p.tool.plan(goodFiles("sce_module/new.prx", "new"));
            contains(installMod(p, "M3").problem, "sce_module/new.prx");
            eq(list("Mod"), names(p.mods));
        });
        pkg("pkg_eboot_needs_the_game_folder", p -> {
            Setup.game(p);
            p.tool.plan(goodFiles("eboot.bin", "eboot"));
            PkgInstaller.ModResult r = installMod(p, "Mod", false, null);
            no(r.ok); contains(r.problem, "choose the game folder first");
            new File(p.game, "eboot.bin").delete();
            contains(installMod(p, "Mod").problem, "choose the game folder first");
            p.tool.plan(goodFiles()); // without an eboot.bin in the PKG the game is not needed
            yes(installMod(p, "Mod", false, null).ok);
        });
        pkg("pkg_no_data_files", p -> {
            Setup.game(p);
            p.tool.plan(files("sce_sys/param.sfo", sfoFile(), "eboot.bin", "eboot"));
            eq("This PKG has no game data files (dvdroot_ps4)", installMod(p, "Mod").problem);
            eq(0, names(p.mods).size());
        });
        pkg("pkg_tool_failures", p -> {
            Setup.game(p);
            List<String[]> plans = new ArrayList<>();
            plans.add(new String[]{"exit", "1"}); plans.add(new String[]{"signal", "11"});
            plans.add(new String[]{"exit", "1", "print_first", "Failed extracting file index 3: x"});
            for (String[] plan : plans) {
                p.tool.plan(goodFiles(), plan);
                PkgInstaller.ModResult r = installMod(p, "Mod");
                no(r.ok); eq(null, r.name); eq(RETAIL, r.problem, Arrays.toString(plan));
                eq(0, names(p.mods).size()); // staging removed
            }
            p.tool.plan(null, "exit", "1", "print_first", "Failed to open PKG: PKG file size is different");
            contains(installMod(p, "Mod").problem, "damaged or incomplete");
            PkgInstaller missing = new PkgInstaller(new File(p.root, "nope"), null);
            contains(missing.installMod(p.pkg, "M", p.mods, null, p.sink(), NEVER).problem, "PKG tool is missing");
        });
        pkg("pkg_not_enough_space", p -> {
            Setup.game(p);
            p.tool.plan(goodFiles(), "total", String.valueOf(1L << 60), "sleep", "30"); // killed while it sleeps: the shell has no child left writing
            PkgInstaller.ModResult r = installMod(p, "Mod");
            no(r.ok); yes(r.problem.startsWith("Not enough free space"), r.problem);
            eq(0, names(p.mods).size());
            p.installer.space = dir -> 1 << 20; // before the tool runs
            p.tool.plan(goodFiles());
            int runs = p.tool.runs();
            contains(installMod(p, "x").problem, "Not enough free space");
            eq(runs, p.tool.runs());
        });
        pkg("pkg_bad_pkg_files", p -> {
            Setup.game(p);
            makePkgFile(p.pkg, sfo("TITLE_ID", "CUSA03173", "APP_VER", "01.09", "CATEGORY", "gp"));
            File notPkg = new File(p.root, "x.pkg");
            write(notPkg, new byte[2000]); Files.write(notPkg.toPath(), "PK\3\4 not a pkg".getBytes());
            eq("This is not a PS4 PKG file", installMod(p, "Mod", true, notPkg).problem);
            byte[] data = read(p.pkg);
            File cut = new File(p.root, "cut.pkg"); write(cut, Arrays.copyOf(data, data.length - 100));
            contains(installMod(p, "Mod", true, cut).problem, "incomplete");
            File tiny = new File(p.root, "tiny.pkg"); write(tiny, new byte[]{0x7f, 'C', 'N', 'T'});
            no(installMod(p, "Mod", true, tiny).ok);
            no(installMod(p, "Mod", true, new File(p.root, "missing.pkg")).ok);
            contains(installMod(p, "Mod", true, new File(p.root, "missing.pkg")).problem, "Can't read the PKG");
            eq(0, p.tool.runs());
        });
        pkg("pkg_stale_staging_removed_and_hidden_from_list", p -> {
            Setup.game(p);
            write(new File(p.mods, ".staging-dead/CUSA03173/dvdroot_ps4/msg/m.fmg"), "x");
            // A leftover staging folder has the layout of a mod, but is neither listed nor applied.
            eq(0, new ModManager(p.mods, null).list().mods.size());
            File g2 = new File(p.root, "g2"); new File(g2, "dvdroot_ps4").mkdirs();
            eq(g2.getPath(), new ModManager(p.mods, null).apply(g2, new File(p.root, "o"), s -> {}));
            p.tool.plan(goodFiles());
            yes(installMod(p, "Mod").ok);
            eq(list("Mod"), names(p.mods));
        });
        pkg("pkg_cancel_kills_the_tool_and_removes_staging", p -> {
            Setup.game(p);
            p.tool.plan(goodFiles(), "sleep", "30");
            AtomicBoolean cancel = new AtomicBoolean();
            new Thread(() -> { try { Thread.sleep(500); } catch (InterruptedException ignored) {} cancel.set(true); }).start();
            long start = System.nanoTime();
            try { p.installer.installMod(p.pkg, "Mod", p.mods, p.game, p.sink(), cancel::get); throw new AssertionError("not cancelled"); }
            catch (CancellationException expected) { /* ok */ }
            yes((System.nanoTime() - start) / 1e9 < 20, "the tool was killed");
            eq(0, names(p.mods).size());
        });
    }

    // ---------------------------------------------------------------- install-game

    static final class GamePkg {
        final Pkg p; final File dest, base, update;
        GamePkg(Pkg p) throws IOException {
            this.p = p; dest = new File(p.root, "games");
            base = makePkgFile(new File(p.root, "base.pkg"), sfo("TITLE_ID", "CUSA03173", "APP_VER", "01.00", "CATEGORY", "gd"));
            update = makePkgFile(new File(p.root, "update.pkg"), sfo("TITLE_ID", "CUSA03173", "APP_VER", "01.09", "CATEGORY", "gp"));
        }
        PkgInstaller.GameResult install(File pkg, boolean replace, File to) { return p.installer.installGame(pkg, to, replace, p.sink(), NEVER); }
        PkgInstaller.GameResult install(File pkg) { return install(pkg, false, dest); }
        Map<String, String> baseFiles(String... extra) {
            return plus(files("sce_sys/param.sfo", sfoFile("CUSA03173", "01.00", "gd"), "eboot.bin", "old eboot", "dvdroot_ps4/chr/a.dcx", "old a", "dvdroot_ps4/chr/keep.dcx", "keep"), extra);
        }
        Map<String, String> updateFiles(String... extra) {
            return plus(files("sce_sys/param.sfo", sfoFile(), "eboot.bin", "new eboot", "dvdroot_ps4/chr/a.dcx", "new a", "dvdroot_ps4/msg/new.fmg", "new"), extra);
        }
    }
    interface WithGamePkg { void run(GamePkg g) throws Exception; }
    static void gamePkg(String name, WithGamePkg body) { pkg(name, p -> body.run(new GamePkg(p))); }

    static void installGameTests() {
        gamePkg("game_base_then_update_merge", g -> {
            g.p.tool.plan(g.baseFiles(), "total", "2000");
            PkgInstaller.GameResult r = g.install(g.base);
            File game = new File(g.dest, "CUSA03173");
            yes(r.ok, String.valueOf(r.problem)); eq(game.getPath(), r.gameDir); eq("CUSA03173", r.titleId); eq("gd", r.category); eq("01.00", r.appVer);
            yes(r.ready); eq(null, r.next); eq(null, r.problem);
            yes(g.p.progress.contains("progress 2000 2000 Extracting"));
            eq("old eboot", text(new File(game, "eboot.bin")));
            eq(list("CUSA03173"), names(g.dest)); // no staging left
            g.p.tool.plan(g.updateFiles());
            r = g.install(g.update);
            yes(r.ok); eq("gp", r.category); eq("01.09", r.appVer); eq(null, r.problem); eq(game.getPath(), r.gameDir);
            eq("new eboot", text(new File(game, "eboot.bin")));
            eq("new a", text(new File(game, "dvdroot_ps4/chr/a.dcx")));
            eq("keep", text(new File(game, "dvdroot_ps4/chr/keep.dcx")));
            eq("new", text(new File(game, "dvdroot_ps4/msg/new.fmg")));
            contains(text(new File(game, "sce_sys/param.sfo")), "APP_VER");
            eq(list("CUSA03173"), names(g.dest));
            // Giving the game folder itself as the destination works too.
            g.p.tool.plan(g.updateFiles("dvdroot_ps4/msg/second.fmg", "two"));
            yes(g.install(g.update, false, game).ok);
            yes(new File(game, "dvdroot_ps4/msg/second.fmg").isFile());
            eq(list("CUSA03173"), names(g.dest));
        });
        gamePkg("game_update_matches_existing_names_case_insensitively", g -> {
            File game = new File(g.dest, "CUSA03173");
            write(new File(game, "eboot.bin"), "old"); write(new File(game, "DVDROOT_PS4/Chr/a.dcx"), "old a");
            g.p.tool.plan(g.updateFiles());
            yes(g.install(g.update).ok);
            List<String> upper = new ArrayList<>();
            for (String n : names(game)) if (n.equalsIgnoreCase("dvdroot_ps4")) upper.add(n);
            eq(list("DVDROOT_PS4"), upper);
            eq("new a", text(new File(game, "DVDROOT_PS4/Chr/a.dcx")));
        });
        gamePkg("game_update_reports_moving_progress", g -> {
            g.p.tool.plan(g.baseFiles());
            yes(g.install(g.base).ok);
            Map<String, String> files = g.updateFiles();
            g.p.tool.plan(files);
            g.p.progress.clear();
            yes(g.install(g.update).ok);
            long total = 0; for (String v : files.values()) total += v.getBytes(StandardCharsets.ISO_8859_1).length;
            yes(g.p.progress.contains("progress 0 " + total + " Installing"), g.p.progress.toString());
            yes(g.p.progress.contains("progress " + total + " " + total + " Installing"), g.p.progress.toString());
        });
        gamePkg("game_update_without_game", g -> {
            g.p.tool.plan(g.updateFiles());
            PkgInstaller.GameResult r = g.install(g.update);
            no(r.ok); eq(null, r.gameDir); contains(r.problem, "install the base game PKG for CUSA03173 first");
            eq("CUSA03173", r.titleId); eq("gp", r.category); eq(0, g.p.tool.runs());
        });
        gamePkg("game_wrong_title_and_category", g -> {
            File pkg = g.p.pkg;
            makePkgFile(pkg, "EP0000-CUSA12345_00-X", sfo("TITLE_ID", "CUSA12345", "APP_VER", "01.00", "CATEGORY", "gd"));
            PkgInstaller.GameResult r = g.install(pkg);
            no(r.ok); contains(r.problem, "CUSA12345");
            makePkgFile(pkg, sfo("TITLE_ID", "CUSA03173", "APP_VER", "01.00", "CATEGORY", "ac"));
            contains(g.install(pkg).problem, "ac content");
            makePkgFile(pkg, null); // no param.sfo readable
            no(g.install(pkg).ok);
            File other = new File(g.p.root, "CUSA00900"); // the folder of another release
            contains(g.install(g.base, false, other).problem, "another release");
            eq(0, g.p.tool.runs()); no(other.exists());
        });
        gamePkg("game_param_sfo_after_unpacking_must_agree", g -> {
            g.p.tool.plan(g.baseFiles("sce_sys/param.sfo", sfoFile("CUSA00900", "01.00", "gd")));
            no(g.install(g.base).ok);
            eq(0, names(g.dest).size());
        });
        gamePkg("game_replace_refusal_and_replace", g -> {
            g.p.tool.plan(g.baseFiles());
            yes(g.install(g.base).ok);
            File game = new File(g.dest, "CUSA03173");
            write(new File(game, "dvdroot_ps4/chr/stale.dcx"), "stale");
            int runs = g.p.tool.runs();
            PkgInstaller.GameResult r = g.install(g.base);
            no(r.ok); eq("A game is already installed there", r.problem); eq(runs, g.p.tool.runs());
            yes(new File(game, "dvdroot_ps4/chr/stale.dcx").exists());
            g.p.tool.plan(g.baseFiles("dvdroot_ps4/chr/b.dcx", "b"));
            r = g.install(g.base, true, g.dest);
            yes(r.ok, String.valueOf(r.problem));
            no(new File(game, "dvdroot_ps4/chr/stale.dcx").exists()); yes(new File(game, "dvdroot_ps4/chr/b.dcx").exists());
            eq(list("CUSA03173"), names(g.dest));
        });
        gamePkg("game_failed_replace_keeps_the_old_game", g -> {
            g.p.tool.plan(g.baseFiles());
            g.install(g.base);
            g.p.tool.plan(g.baseFiles(), "exit", "1");
            no(g.install(g.base, true, g.dest).ok);
            eq("old eboot", text(new File(g.dest, "CUSA03173/eboot.bin")));
            eq(list("CUSA03173"), names(g.dest));
        });
        gamePkg("game_not_enough_space", g -> {
            g.p.tool.plan(g.baseFiles(), "total", String.valueOf(1L << 60), "sleep", "30");
            PkgInstaller.GameResult r = g.install(g.base);
            no(r.ok);
            yes(r.problem.matches("Not enough free space: .* GB needed .*GB available"), r.problem);
            eq(0, names(g.dest).size());
            g.p.installer.space = dir -> 1 << 20; // before the tool runs
            int runs = g.p.tool.runs();
            contains(g.install(g.base).problem, "Not enough free space");
            eq(runs, g.p.tool.runs());
        });
        gamePkg("game_tool_failure", g -> {
            g.p.tool.plan(g.baseFiles(), "signal", "11");
            PkgInstaller.GameResult r = g.install(g.base);
            no(r.ok); contains(r.problem, "encrypted for a retail PS4");
            eq(0, names(g.dest).size());
        });
        gamePkg("game_stale_staging_removed", g -> {
            write(new File(g.dest, ".staging-old/f"), "x");
            g.p.tool.plan(g.baseFiles());
            yes(g.install(g.base).ok);
            eq(list("CUSA03173"), names(g.dest));
        });
        gamePkg("game_ready_and_next_follow_game_check", g -> {
            GameCheck.Found[] verdict = {new GameCheck.Found("missing_update", "CUSA03173", "01.00")};
            g.p.installer.checker = new PkgInstaller.Checker() {
                @Override public GameCheck.Found problem(File game) { return verdict[0]; }
                @Override public String explain(GameCheck.Found f) { return f.kind + ": " + f.title + " " + f.version; }
            };
            g.p.tool.plan(g.baseFiles());
            PkgInstaller.GameResult r = g.install(g.base);
            yes(r.ok); no(r.ready); eq("Install the 1.09 update PKG for CUSA03173", r.next); eq(null, r.problem);
            eq(new File(g.dest, "CUSA03173").getPath(), r.gameDir);
            g.p.tool.plan(g.updateFiles());
            verdict[0] = null;
            r = g.install(g.update);
            yes(r.ok); yes(r.ready); eq(null, r.next);
            verdict[0] = new GameCheck.Found("damaged_files", "CUSA03173", "01.09");
            r = g.install(g.update);
            no(r.ok); no(r.ready); eq("damaged_files: CUSA03173 01.09", r.problem);
            eq(new File(g.dest, "CUSA03173").getPath(), r.gameDir);
        });
        gamePkg("game_cancel_leaves_nothing", g -> {
            g.p.tool.plan(g.baseFiles(), "sleep", "30");
            AtomicBoolean cancel = new AtomicBoolean();
            new Thread(() -> { try { Thread.sleep(500); } catch (InterruptedException ignored) {} cancel.set(true); }).start();
            try { g.p.installer.installGame(g.base, g.dest, false, g.p.sink(), cancel::get); throw new AssertionError("not cancelled"); }
            catch (CancellationException expected) { /* ok */ }
            eq(0, names(g.dest).size());
        });
    }

    // ---------------------------------------------------------------- game check (tests/test_game_check.py)

    /** The synthetic SELF of tests/test_prepare.py's fixture(). */
    static byte[] selfFixture() {
        ByteBuffer b = ByteBuffer.allocate(0x210).order(ByteOrder.LITTLE_ENDIAN);
        b.put(0, (byte)'O').put(1, (byte)0x15).put(2, (byte)'=').put(3, (byte)0x1d);
        b.putShort(24, (short)1);
        b.putLong(32, 0x800).putLong(40, 0x200).putLong(48, 16).putLong(56, 16);
        byte[] ident = {0x7f, 'E', 'L', 'F', 2, 1, 1, 9};
        for (int i = 0; i < ident.length; ++i) b.put(64 + i, ident[i]);
        b.putShort(80, (short)0xfe10).putShort(82, (short)62).putInt(84, 1).putLong(88, 0).putLong(96, 64).putLong(104, 0);
        b.putInt(112, 0).putShort(116, (short)64).putShort(118, (short)56).putShort(120, (short)1).putShort(122, (short)0).putShort(124, (short)0).putShort(126, (short)0);
        b.putInt(128, 1).putInt(132, 5).putLong(136, 0x1000).putLong(144, 0).putLong(152, 0).putLong(160, 16).putLong(168, 32).putLong(176, 0x1000);
        for (int i = 0; i < 16; ++i) b.put(0x200 + i, (byte)i);
        return b.array();
    }
    static byte[] dcx(byte[] payload, byte[] packedOverride) {
        byte[] packed = packedOverride;
        if (packed == null) {
            java.util.zip.Deflater d = new java.util.zip.Deflater();
            d.setInput(payload); d.finish();
            byte[] buf = new byte[payload.length + 1024];
            int n = d.deflate(buf); d.end();
            packed = Arrays.copyOf(buf, n);
        }
        byte[] header = new byte[0x4C];
        System.arraycopy("DCX\0".getBytes(), 0, header, 0, 4);
        System.arraycopy("DCS\0".getBytes(), 0, header, 0x18, 4);
        System.arraycopy("DCP\0".getBytes(), 0, header, 0x24, 4);
        System.arraycopy("DFLT".getBytes(), 0, header, 0x28, 4);
        ByteBuffer.wrap(header).order(ByteOrder.BIG_ENDIAN).putInt(0x1C, payload.length).putInt(0x20, packed.length);
        System.arraycopy("DCA\0".getBytes(), 0, header, 0x44, 4);
        byte[] out = Arrays.copyOf(header, header.length + packed.length);
        System.arraycopy(packed, 0, out, header.length, packed.length);
        return out;
    }
    static File gameFolder(File root, String title, String version, byte[] eboot) throws IOException {
        write(new File(root, "sce_sys/param.sfo"), sfo("APP_VER", version, "TITLE_ID", title));
        write(new File(root, "eboot.bin"), eboot == null ? selfFixture() : eboot);
        return root;
    }
    interface WithDir { void run(File dir) throws Exception; }
    static void dir(String name, WithDir body) { test(name, () -> { File d = temp(); try { body.run(d); } finally { delete(d); } }); }

    static void gameCheckTests() {
        String SUPPORTED = GameCheck.SUPPORTED_IMAGE, LANCE = GameCheck.LANCE_60FPS_IMAGE;
        dir("check_base_game_without_the_update", d -> {
            GameCheck.Found f = GameCheck.problem(gameFolder(d, "CUSA03173", "01.00", null), false);
            eq("missing_update", f.kind); eq("CUSA03173", f.title); eq("01.00", f.version);
        });
        dir("check_update_metadata_with_an_old_executable", d -> eq("wrong_eboot", GameCheck.problem(gameFolder(d, "CUSA03173", "01.09", null), false).kind));
        dir("check_other_edition", d -> eq("other_title", GameCheck.problem(gameFolder(d, "CUSA01322", "01.09", null), false).kind));
        dir("check_unreadable_executable", d -> eq("unreadable", GameCheck.problem(gameFolder(d, "CUSA03173", "01.09", "junk".getBytes()), false).kind));
        dir("check_image_hash_of_the_fixture", d -> {
            gameFolder(d, "CUSA03173", "01.09", null);
            // image = 16 bytes 0..15 then 16 zeros (memsz 32 at vaddr 0)
            byte[] image = new byte[32]; for (int i = 0; i < 16; ++i) image[i] = (byte)i;
            StringBuilder hex = new StringBuilder();
            for (byte b : java.security.MessageDigest.getInstance("SHA-256").digest(image)) hex.append(String.format("%02x", b & 0xff));
            eq(hex.toString(), GameCheck.imageSha256(d));
        });
        dir("check_supported_image_passes", d -> {
            File game = gameFolder(d, "CUSA03173", "01.09", null);
            eq(null, GameCheck.problem(game, false, null, GameCheck.imageSha256(game), LANCE));
        });
        dir("check_intact_shaders_pass", d -> {
            File game = gameFolder(d, "CUSA03173", "01.09", null);
            byte[] payload = new byte[256 * 64]; for (int i = 0; i < payload.length; ++i) payload[i] = (byte)i;
            write(new File(game, GameCheck.CHECKED_FOLDER + "/gxshader.shaderbnd.dcx"), dcx(payload, null));
            eq(null, GameCheck.problem(game, false, null, GameCheck.imageSha256(game), LANCE));
        });
        dir("check_damaged_shaders", d -> {
            // As LibOrbisPkg on .NET 6+ (issue #81): part of the data replaced by stale bytes.
            File game = gameFolder(d, "CUSA03173", "01.09", null);
            byte[] random = new byte[4096]; new java.util.Random(7).nextBytes(random);
            byte[] good = dcx(random, null);
            byte[] packed = Arrays.copyOfRange(good, 0x4C, good.length);
            int half = packed.length / 2;
            System.arraycopy(packed, 0, packed, half, packed.length - half);
            write(new File(game, GameCheck.CHECKED_FOLDER + "/gxshader.shaderbnd.dcx"), dcx(random, packed));
            eq("damaged_files", GameCheck.problem(game, false, null, GameCheck.imageSha256(game), LANCE).kind);
            eq(list(GameCheck.CHECKED_FOLDER + "/gxshader.shaderbnd.dcx"), GameCheck.brokenFiles(game, GameCheck.CHECKED_FOLDER));
        });
        dir("check_other_compressions_are_not_checked", d -> {
            File game = gameFolder(d, "CUSA03173", "01.09", null);
            byte[] data = dcx(new byte[100], null);
            System.arraycopy("KRAK".getBytes(), 0, data, 0x28, 4);
            write(new File(game, GameCheck.CHECKED_FOLDER + "/x.dcx"), Arrays.copyOf(data, 0x50));
            eq(0, GameCheck.brokenFiles(game, GameCheck.CHECKED_FOLDER).size());
        });
        dir("check_every_retail_base_game_without_the_update", d -> {
            for (String title : GameCheck.SUPPORTED_TITLES) eq("missing_update", GameCheck.problem(gameFolder(d, title, "01.00", null), false).kind, title);
        });
        dir("check_eboot_with_the_60fps_patch", d -> {
            File game = gameFolder(d, "CUSA00900", "01.09", null);
            eq("patched_eboot", GameCheck.problem(game, false, null, SUPPORTED, GameCheck.imageSha256(game)).kind);
        });
        dir("check_skip_switch", d -> eq(null, GameCheck.problem(gameFolder(d, "CUSA03173", "01.00", null), true)));
        test("check_every_problem_is_explained", () -> {
            for (String kind : list("missing_update", "wrong_eboot", "patched_eboot", "other_title", "unreadable", "damaged_files"))
                contains(GameCheck.explain(new GameCheck.Found(kind, "CUSA03173", "01.00")), "CUSA");
        });
        // tests/test_prepare.py's SELF cases, as far as the check sees them
        dir("check_self_truncated_encrypted_missing_bad_index", d -> {
            byte[] truncated = selfFixture(); truncated = Arrays.copyOf(truncated, truncated.length - 1);
            eq("unreadable", GameCheck.problem(gameFolder(d, "CUSA03173", "01.09", truncated), false).kind);
            for (long flag : new long[]{2, 8}) {
                byte[] data = selfFixture(); ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN).putLong(32, 0x800 | flag);
                eq("unreadable", GameCheck.problem(gameFolder(d, "CUSA03173", "01.09", data), false).kind);
            }
            byte[] missing = selfFixture(); ByteBuffer.wrap(missing).order(ByteOrder.LITTLE_ENDIAN).putLong(32, 0);
            eq("unreadable", GameCheck.problem(gameFolder(d, "CUSA03173", "01.09", missing), false).kind);
            byte[] index = selfFixture(); ByteBuffer.wrap(index).order(ByteOrder.LITTLE_ENDIAN).putLong(32, 0x100800);
            eq("unreadable", GameCheck.problem(gameFolder(d, "CUSA03173", "01.09", index), false).kind);
        });
    }

    // ---------------------------------------------------------------- real PKG (optional)

    static void realTests() {
        String toolPath = System.getenv().getOrDefault("BB_HOST_PKG_TOOL", "out/host/pkg-tool/ps4-pkg-tool");
        String modPkg = System.getenv("BB_TEST_MOD_PKG"), gameDir = System.getenv("BB_TEST_GAME");
        File tool = new File(toolPath);
        test("real_pkg_mod_install_and_apply", () -> {
            if (!tool.isFile() || modPkg == null || !new File(modPkg).isFile() || gameDir == null || !new File(gameDir).isDirectory())
                skip("needs the host ps4-pkg-tool, BB_TEST_MOD_PKG and BB_TEST_GAME");
            File root = temp();
            try {
                File mods = new File(root, "mods"), game = new File(gameDir);
                List<String> progress = new ArrayList<>();
                PkgInstaller inst = new PkgInstaller(tool, null);
                PkgInstaller.ModResult r = inst.installMod(new File(modPkg), "Vietnamese", mods, game, (d, t, l) -> progress.add("progress " + d + " " + t + " " + l), NEVER);
                yes(r.ok, String.valueOf(r.problem)); eq("Vietnamese", r.name); yes(r.mod.files > 300, "files " + r.mod.files);
                eq(list("Vietnamese"), names(mods));
                yes(progress.get(progress.size() - 1).startsWith("progress 0 0 Checking"), progress.toString());
                boolean extracting = false; for (String l : progress) extracting |= l.endsWith(" Extracting");
                yes(extracting);
                List<String> log = new ArrayList<>();
                File out = new File(root, "mod-game");
                File overlay = new File(new ModManager(mods, null).apply(game, out, log::add));
                eq(FsUtil.resolve(out), overlay.getParentFile(), String.join("\n", log));
                File msgDir = new File(mods, "Vietnamese/dvdroot_ps4/msg");
                int checked = 0;
                for (File mf : walk(msgDir)) {
                    File link = new File(overlay, "dvdroot_ps4/msg/" + ModLoader.relative(msgDir, mf));
                    eq(FsUtil.resolve(mf), FsUtil.resolve(link)); ++checked;
                }
                yes(checked > 0);
                eq(FsUtil.resolve(new File(game, "eboot.bin")), FsUtil.resolve(new File(overlay, "eboot.bin")));
            } finally { delete(root); }
        });
    }
    static List<File> walk(File dir) {
        List<File> out = new ArrayList<>();
        File[] list = dir.listFiles();
        if (list != null) for (File f : list) { if (f.isDirectory()) out.addAll(walk(f)); else out.add(f); }
        return out;
    }

    public static void main(String[] args) {
        modsAndroidTests(); modsPyTests(); sfoTests(); installPkgTests(); installGameTests(); gameCheckTests(); realTests();
        System.exit(T.finish());
    }
}
