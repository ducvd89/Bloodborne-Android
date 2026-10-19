package ro.cobrabm.fexdroid;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;

/** The mods folder as the app sees it: list and validate the mods, and build the overlay of the game folder the
 *  game starts from. Java port of tools/android/mods_android.py (list, validate, apply); no Android classes.
 *
 *  Only game version 1.09 data mods are supported: a mod that would put anything outside dvdroot_ps4 (eboot.bin,
 *  sce_module/, sce_sys/, param.sfo, executables) is invalid and never applied. */
final class ModManager {
    static final String NOT_A_MOD = "Not a Bloodborne mod: no dvdroot_ps4 or game folders inside";
    /** Entries whose presence anywhere in a mod makes it unsupported (compared case-insensitively). */
    private static final Set<String> SYSTEM_NAMES = new HashSet<>(Arrays.asList("eboot.bin", "param.sfo", "sce_module", "sce_sys"));
    /** Top-level files (outside dvdroot_ps4) that are PS4 executables/modules. */
    private static final String[] EXECUTABLE_SUFFIXES = {".self", ".elf", ".oelf", ".prx", ".sprx"};

    /** Files shared with another mod. */
    static final class Shared {
        final String mod; final int files;
        Shared(String mod, int files) { this.mod = mod; this.files = files; }
    }
    /** One mod folder of the list. */
    static final class Mod {
        String name; boolean enabled, valid; String problem; int files; long bytes;
        final List<Shared> overrides = new ArrayList<>(), overriddenBy = new ArrayList<>();
        Mod(String name, boolean enabled) { this.name = name; this.enabled = enabled; }
    }
    /** The mods folder's list. `error` is the text for the player (null when it could be read). */
    static final class Listing {
        String modsDir; boolean exists; List<Mod> mods = new ArrayList<>(); String error;
    }

    private final File modsDir, config;

    /** modsDir: the mods folder; config: mods.json of the desktop format ({"order": [...], "disabled": [...]}). */
    ModManager(File modsDir, File config) { this.modsDir = modsDir; this.config = config; }

    File modsDir() { return modsDir; }

    private static String describe(Exception e) {
        String type = e instanceof ModLoader.ModError ? "ValueError" : e.getClass().getSimpleName();
        String message = e instanceof ModLoader.ModError ? e.getMessage() : String.valueOf(e.getMessage());
        return type + ": " + message;
    }

    /** (order, disabled) from mods.json; ModError when it is malformed. */
    static ModLoader.Settings readConfig(File config) throws IOException {
        if (config == null || !config.isFile()) return new ModLoader.Settings(new ArrayList<String>(), new HashSet<String>());
        Object settings;
        String text = SafeWrite.readText(config); // NUL bytes only (a hard reset before it was synced): no mods.json
        if (text == null) return new ModLoader.Settings(new ArrayList<String>(), new HashSet<String>());
        try { settings = MiniJson.parse(text); }
        catch (MiniJson.ParseException e) { throw new ModLoader.ModError("mods.json: " + e.getMessage()); }
        if (!(settings instanceof Map)) throw new ModLoader.ModError("mods.json: expected an object");
        Map<?, ?> map = (Map<?, ?>)settings;
        List<String> order = ModLoader.strings(map.get("order"), map.containsKey("order"), "mods.json: \"order\" must be a list of folder names");
        List<String> disabled = ModLoader.strings(map.get("disabled"), map.containsKey("disabled"), "mods.json: \"disabled\" must be a list of folder names");
        return new ModLoader.Settings(order, new HashSet<>(disabled));
    }

    /** Names of all visible sub-folders of the mods folder. */
    private List<String> folders() {
        List<String> out = new ArrayList<>();
        if (!modsDir.isDirectory()) return out;
        for (String name : FsUtil.names(modsDir))
            if (new File(modsDir, name).isDirectory() && !name.startsWith(".")) out.add(name);
        Collections.sort(out, FsUtil.FOLDED);
        return out;
    }

    /** The mods in load order, the folders that are not mods, and the disabled names. Same merge as
     *  ModLoader.selected: configured order first, newly found mods after it alphabetically. */
    private static final class Order {
        List<String> mods, others; Set<String> disabled;
    }
    private Order loadOrder() throws IOException {
        ModLoader.Settings settings = readConfig(config);
        List<String> available = new ArrayList<>();
        // Dot-folders (.staging-* of an install in progress) are never mods.
        for (String n : ModLoader.discover(modsDir)) if (!n.startsWith(".")) available.add(n);
        Set<String> ordered = new LinkedHashSet<>();
        for (String n : settings.order) if (available.contains(n)) ordered.add(n);
        for (String n : available) ordered.add(n);
        Order o = new Order();
        o.mods = new ArrayList<>(ordered);
        o.others = new ArrayList<>();
        for (String n : folders()) if (!available.contains(n)) o.others.add(n);
        o.disabled = settings.disabled;
        return o;
    }

    /** Why a mod folder cannot be used (null when it is a supported data mod). */
    static String problemOf(File folder) {
        ModLoader.Layout layout = ModLoader.contentRoot(folder);
        if (layout == null) return NOT_A_MOD;
        File root = FsUtil.resolve(layout.root), top = FsUtil.resolve(folder);
        return scan(folder, folder, root, top);
    }
    private static String scan(File folder, File directory, File root, File top) {
        List<String> dirs = new ArrayList<>(), files = new ArrayList<>();
        for (String name : FsUtil.names(directory)) (new File(directory, name).isDirectory() ? dirs : files).add(name);
        for (List<String> group : Arrays.asList(dirs, files)) {
            for (String name : group) {
                String shown = ModLoader.relative(folder, new File(directory, name));
                if (SYSTEM_NAMES.contains(FsUtil.casefold(name)))
                    return "Not a 1.09 data mod: contains " + shown + " (only files inside dvdroot_ps4 are supported)";
                if (FsUtil.isSymlink(new File(directory, name))) return "Symlinks are unsupported: " + shown;
            }
        }
        File here = FsUtil.resolve(directory);
        if (here.equals(root) || here.equals(top)) {
            for (String name : files) {
                String folded = FsUtil.casefold(name);
                for (String suffix : EXECUTABLE_SUFFIXES)
                    if (folded.endsWith(suffix)) return "Not a 1.09 data mod: contains executable " + name + " outside dvdroot_ps4";
            }
        }
        for (String name : dirs) {
            File sub = new File(directory, name);
            if (FsUtil.isSymlink(sub)) continue; // os.walk(followlinks=False): listed, not entered
            String found = scan(folder, sub, root, top);
            if (found != null) return found;
        }
        return null;
    }

    /** The mod's row without conflicts, and its game paths ({lower-cased path: size}). */
    private Mod inspect(String name, boolean enabled, Map<String, Long> keys) {
        Mod row = new Mod(name, enabled);
        try {
            row.problem = problemOf(new File(modsDir, name));
            if (row.problem == null) {
                for (ModLoader.ModFile f : ModLoader.modFiles(new File(modsDir, name)))
                    keys.put(FsUtil.casefold(f.relative), f.source.length());
                row.valid = true;
                row.files = keys.size();
                long total = 0;
                for (long size : keys.values()) total += size;
                row.bytes = total;
            }
        } catch (IOException e) {
            row.problem = e.getMessage() != null ? e.getMessage() : e.toString();
            keys.clear();
            row.valid = false; row.files = 0; row.bytes = 0;
        }
        return row;
    }

    private List<Mod> buildList() throws IOException {
        Order order = loadOrder();
        List<Mod> rows = new ArrayList<>();
        Map<String, Map<String, Long>> owned = new HashMap<>();
        for (String name : order.mods) {
            Map<String, Long> keys = new LinkedHashMap<>();
            Mod row = inspect(name, !order.disabled.contains(name), keys);
            rows.add(row);
            owned.put(name, row.enabled && row.valid ? keys : new HashMap<String, Long>());
        }
        for (int i = 0; i < rows.size(); ++i) {
            Mod row = rows.get(i);
            Map<String, Long> mine = owned.get(row.name);
            if (mine.isEmpty()) continue;
            for (int j = 0; j < i; ++j) shared(row.overrides, mine, rows.get(j), owned);
            for (int j = i + 1; j < rows.size(); ++j) shared(row.overriddenBy, mine, rows.get(j), owned);
        }
        for (String name : order.others) {
            Mod row = new Mod(name, false);
            row.problem = NOT_A_MOD;
            rows.add(row);
        }
        return rows;
    }
    private static void shared(List<Shared> into, Map<String, Long> mine, Mod other, Map<String, Map<String, Long>> owned) {
        int n = 0;
        Map<String, Long> theirs = owned.get(other.name);
        for (String key : mine.keySet()) if (theirs.containsKey(key)) ++n;
        if (n > 0) into.add(new Shared(other.name, n));
    }

    /** The mods folder's list; never throws (the problem goes to `error`). */
    Listing list() {
        Listing result = new Listing();
        result.modsDir = modsDir.getAbsoluteFile().getPath();
        result.exists = modsDir.isDirectory();
        try { result.mods = buildList(); }
        catch (Exception e) { result.error = describe(e); } // the app shows the message; never a stack trace
        return result;
    }

    /** One mod folder's row (enabled assumed); never throws. */
    Mod validate(String name) {
        try {
            if (name.indexOf('/') >= 0 || name.isEmpty() || name.equals(".") || name.equals("..") || !new File(modsDir, name).isDirectory())
                throw new ModLoader.ModError("No such mod folder: " + name);
            return inspect(name, true, new HashMap<String, Long>());
        } catch (Exception e) {
            Mod row = new Mod(name, true);
            row.problem = describe(e);
            return row;
        }
    }

    /** The row of a mod just made (as validate, throwing nothing). Used by the PKG install. */
    Mod inspectNew(String name) { return inspect(name, true, new HashMap<String, Long>()); }

    // ---- apply ----

    /** Empties `out` (stale overlays of killed processes) without following links. */
    static void clear(File out, File... protectedFolders) throws IOException {
        out = FsUtil.resolve(out);
        for (File folder : protectedFolders) {
            File other = FsUtil.resolve(folder);
            if (overlaps(out, other)) throw new ModLoader.ModError("Refusing to clear " + out.getPath() + ": it overlaps " + other.getPath());
        }
        if (!out.isDirectory()) return;
        File[] entries = out.listFiles();
        if (entries != null) for (File e : entries) FsUtil.deleteTree(e);
    }
    private static boolean overlaps(File a, File b) {
        String x = a.getPath(), y = b.getPath();
        return x.equals(y) || inside(x, y) || inside(y, x);
    }
    private static boolean inside(String parent, String child) {
        return child.startsWith(parent.endsWith("/") ? parent : parent + "/");
    }

    /** Builds the overlay of the game with the enabled valid mods and returns the folder to run: the overlay, or the
     *  game folder itself when there is no mod to apply or it could not be built (the game must start unmodded).
     *  `log` gets the "Mods: ..." lines. Never throws. */
    String apply(File game, File out, Consumer<String> log) {
        String result = game.getPath();
        try {
            clear(out, game, modsDir);
            Order order = loadOrder();
            List<ModLoader.Layer> layers = new ArrayList<>();
            List<String> names = new ArrayList<>();
            for (String name : order.mods) {
                if (order.disabled.contains(name)) { log.accept("Mods: " + name + ": disabled, skipped"); continue; }
                String problem = problemOf(new File(modsDir, name));
                if (problem != null) { log.accept("Mods: " + name + ": invalid, skipped (" + problem + ")"); continue; }
                layers.add(new ModLoader.Layer(name, new File(modsDir, name)));
                names.add(name);
            }
            File built = layers.isEmpty() ? game : ModLoader.buildOverlay(game, out, layers, log);
            log.accept("Mods: " + (names.isEmpty() ? "none" : names.size() + " applied: " + join(", ", names)));
            result = built.getPath();
        } catch (Exception e) {
            log.accept("Mods: failed, starting without mods: " + (e instanceof ModLoader.ModError ? e.getMessage() : String.valueOf(e.getMessage())));
        }
        return result;
    }
    private static String join(String sep, List<String> parts) {
        StringBuilder s = new StringBuilder();
        for (String p : parts) { if (s.length() > 0) s.append(sep); s.append(p); }
        return s.toString();
    }
}
