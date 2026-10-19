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

/** The loose-file mod loader: Java port of scripts/mods.py (no Android classes, so it is tested on a desktop JDK).
 *  Mod folders are merged over the game folder as a tree of symbolic links, so the original game and mod files are
 *  never changed. */
final class ModLoader {
    /** A problem with the mods (Python's ValueError): the message is shown to the player. */
    static final class ModError extends IOException { ModError(String message) { super(message); } }

    /** Top-level folders of the game's dvdroot_ps4: a mod made of these is a dvdroot_ps4 itself (adhoc: the debug
     *  menu's fonts, which the retail game does not ship). */
    static final Set<String> GAME_FOLDERS = new HashSet<>(Arrays.asList("action", "adhoc", "chr", "event", "facegen",
        "font", "map", "menu", "movie", "msg", "mtd", "obj", "other", "param", "paramdef", "parts", "remo", "script",
        "sfx", "shader", "sound"));
    /** The retail releases' folder names (GameCheck.SUPPORTED_TITLES), as a mod's wrapper folder. */
    static final List<String> SERIALS = Collections.unmodifiableList(Arrays.asList("CUSA03173", "CUSA00900", "CUSA00207",
        "CUSA00208", "CUSA00299", "CUSA01363", "CUSA03179", "CUSA03014", "CUSA03023"));
    private static final List<String> DOC_SUFFIXES = Arrays.asList(".txt", ".md", ".jpg", ".png", ".ini");
    private static final int MAX_DEPTH = 64;

    private ModLoader() {}

    /** Where a mod's files are (`root`) and the game path they go to (`prefix`: "" or "dvdroot_ps4"). */
    static final class Layout {
        final File root; final String prefix;
        Layout(File root, String prefix) { this.root = root; this.prefix = prefix; }
    }
    /** (name, folder) of one mod to merge. */
    static final class Layer {
        final String name; final File folder;
        Layer(String name, File folder) { this.name = name; this.folder = folder; }
    }
    /** One file a mod replaces or adds: its game path (posix, from the game folder) and the file itself. */
    static final class ModFile {
        final String relative; final File source;
        ModFile(String relative, File source) { this.relative = relative; this.source = source; }
        String[] parts() { return relative.split("/"); }
    }

    /** Python's Path.suffix. */
    private static String suffix(String name) {
        int i = name.lastIndexOf('.');
        return i > 0 && i < name.length() - 1 ? name.substring(i) : "";
    }

    /** (root, prefix): where a mod's files are and the game path they go to, or null.
     *
     *  Accepted layouts: <mod>/dvdroot_ps4, <mod>/app0/dvdroot_ps4, <mod>/<serial>/dvdroot_ps4 (CUSA03173, CUSA00900,
     *  ...), one wrapper folder around any of these (an archive extracted into a folder of its name), and the game's
     *  folders without dvdroot_ps4 (<mod>/chr, <mod>/parts, ...). */
    static Layout contentRoot(File folder) { return contentRoot(folder, 0); }
    private static Layout contentRoot(File folder, int depth) {
        if (!folder.isDirectory() || depth > MAX_DEPTH) return null;
        List<String> wrappers = new ArrayList<>();
        wrappers.add("");
        wrappers.add("app0");
        wrappers.addAll(SERIALS);
        for (String wrapper : wrappers) {
            File base = wrapper.isEmpty() ? folder : FsUtil.child(folder, wrapper);
            if (FsUtil.child(base, "dvdroot_ps4").isDirectory()) return new Layout(base, "");
        }
        List<File> folders = new ArrayList<>();
        boolean otherFiles = false;
        for (String name : FsUtil.names(folder)) {
            if (name.startsWith(".")) continue;
            File entry = new File(folder, name);
            if (entry.isDirectory()) folders.add(entry);
            else if (entry.isFile() && !DOC_SUFFIXES.contains(FsUtil.casefold(suffix(name)))) otherFiles = true;
        }
        boolean allGame = !folders.isEmpty();
        for (File f : folders) if (!GAME_FOLDERS.contains(FsUtil.casefold(f.getName()))) allGame = false;
        if (allGame) return new Layout(folder, "dvdroot_ps4");
        if (folders.size() == 1 && !otherFiles) return contentRoot(folders.get(0), depth + 1);
        return null;
    }

    /** Named mods: folders of `root` with an accepted layout (contentRoot), by name. */
    static List<String> discover(File root) {
        List<String> out = new ArrayList<>();
        if (!root.isDirectory()) return out;
        for (String name : FsUtil.names(root)) {
            File entry = new File(root, name);
            if (entry.isDirectory() && contentRoot(entry) != null) out.add(name);
        }
        Collections.sort(out, FsUtil.FOLDED);
        return out;
    }

    /** The configured mods.json: (order, disabled). */
    static final class Settings {
        final List<String> order; final Set<String> disabled;
        Settings(List<String> order, Set<String> disabled) { this.order = order; this.disabled = disabled; }
    }
    /** The enabled mods in load order (mods.json: "order", "disabled"); new folders are enabled automatically and
     *  appended in alphabetical order. ModError when the settings are malformed. */
    static List<String> selected(File root, File config) throws IOException {
        List<String> available = discover(root);
        if (config == null || !config.isFile()) return available;
        Object settings;
        String text = SafeWrite.readText(config); // NUL bytes only (a hard reset before it was synced): no mods.json
        if (text == null) return available;
        try { settings = MiniJson.parse(text); }
        catch (MiniJson.ParseException e) { throw new ModError(e.getMessage()); }
        if (!(settings instanceof Map)) throw new ModError("mods.json: expected an object");
        Map<?, ?> map = (Map<?, ?>)settings;
        List<String> disabledNames = strings(map.get("disabled"), map.containsKey("disabled"), "Disabled mods must be a list of folder names");
        Set<String> disabled = new HashSet<>(disabledNames);
        List<String> order = strings(map.get("order"), map.containsKey("order"), "Mod order must be a list of folder names");
        Set<String> out = new LinkedHashSet<>();
        for (String n : order) if (available.contains(n) && !disabled.contains(n)) out.add(n);
        for (String n : available) if (!disabled.contains(n)) out.add(n);
        return new ArrayList<>(out);
    }
    /** A JSON list of strings (the default [] when the key is missing), else ModError(message). */
    static List<String> strings(Object value, boolean present, String message) throws ModError {
        List<String> out = new ArrayList<>();
        if (!present) return out;
        if (!(value instanceof List)) throw new ModError(message);
        for (Object o : (List<?>)value) {
            if (!(o instanceof String)) throw new ModError(message);
            out.add((String)o);
        }
        return out;
    }

    /** The game path and source of every file a mod replaces or adds under dvdroot_ps4. ModError for a mod that
     *  cannot be merged (links, executables, system files). */
    static List<ModFile> modFiles(File folder) throws IOException {
        Layout layout = contentRoot(folder);
        if (layout == null)
            throw new ModError(folder.getPath() + ": expected dvdroot_ps4 (or chr/, parts/, ...) inside the mod folder");
        File root = FsUtil.resolve(layout.root);
        List<ModFile> out = new ArrayList<>();
        walk(root, root, layout.prefix, out);
        return out;
    }
    private static void walk(File root, File directory, String prefix, List<ModFile> out) throws IOException {
        List<String> dirs = new ArrayList<>(), files = new ArrayList<>();
        for (String name : FsUtil.names(directory)) {
            (new File(directory, name).isDirectory() ? dirs : files).add(name);
        }
        for (List<String> group : Arrays.asList(dirs, files))
            for (String name : group)
                if (FsUtil.isSymlink(new File(directory, name)))
                    throw new ModError("Mod symlinks are unsupported: " + new File(directory, name).getPath());
        for (String name : files) {
            File source = new File(directory, name);
            String below = relative(root, source);
            String relative = prefix.isEmpty() ? below : prefix + "/" + below;
            String first = relative.split("/")[0];
            // Readme/metadata stay outside the mounted game. This loader handles assets; executable patches use
            // the existing patch compiler, with address validation.
            if (!FsUtil.casefold(first).equals("dvdroot_ps4")) {
                String f = FsUtil.casefold(first);
                if (f.equals("eboot.bin") || f.equals("sce_module") || f.equals("sce_sys"))
                    throw new ModError("Executable/system replacement is unsupported: " + source.getPath());
                continue;
            }
            if (!source.isFile()) throw new ModError("Not a regular mod file: " + source.getPath());
            out.add(new ModFile(relative, source));
        }
        for (String name : dirs) walk(root, new File(directory, name), prefix, out);
    }
    /** `file` relative to `root` (posix). */
    static String relative(File root, File file) {
        String r = root.getPath(), f = file.getPath();
        String rest = f.startsWith(r) ? f.substring(r.length()) : f;
        while (rest.startsWith("/")) rest = rest.substring(1);
        return rest;
    }

    // ---- The overlay ----

    /** The entries of the overlay's folders by lower-cased name, kept in step with what is created (the Python
     *  listed the folder for every path part of every file). */
    private static final class Names {
        private final Map<File, Map<String, String>> byFolder = new HashMap<>();
        private Map<String, String> of(File folder) {
            Map<String, String> m = byFolder.get(folder);
            if (m == null) {
                m = new HashMap<>();
                for (String n : FsUtil.names(folder)) if (!m.containsKey(FsUtil.casefold(n))) m.put(FsUtil.casefold(n), n);
                byFolder.put(folder, m);
            }
            return m;
        }
        File child(File folder, String name) {
            String existing = of(folder).get(FsUtil.casefold(name));
            return new File(folder, existing != null ? existing : name);
        }
        void added(File folder, String name) {
            Map<String, String> m = byFolder.get(folder);
            if (m != null && !m.containsKey(FsUtil.casefold(name))) m.put(FsUtil.casefold(name), name);
        }
        void forget(File folder) { byFolder.remove(folder); }
    }

    private static void link(File link, File target) throws IOException {
        Files.createSymbolicLink(link.toPath(), target.toPath());
    }

    /** Materializes one folder level; never writes through a folder link. */
    private static void expand(File directory, Names names) throws IOException {
        if (FsUtil.isSymlink(directory)) {
            File target = FsUtil.resolveStrict(directory);
            if (!target.isDirectory()) throw new ModError("File/directory conflict at " + directory.getName());
            Files.delete(directory.toPath());
            if (!directory.mkdir()) throw new IOException("Cannot create " + directory);
            for (String entry : FsUtil.names(target)) link(new File(directory, entry), new File(target, entry));
            names.forget(directory);
        } else if (directory.exists() && !directory.isDirectory()) {
            throw new ModError("File/directory conflict at " + directory.getName());
        } else if (!directory.isDirectory() && !directory.mkdir()) {
            throw new IOException("Cannot create " + directory);
        }
    }

    /** Merges the mods (later ones replace earlier ones) over the game as a new folder of links in `out`, and
     *  returns it; the game folder itself (resolved) when no mod has a file. Nothing is left behind on failure.
     *  `log` gets the "Mods: ..." lines. */
    static File buildOverlay(File game, File out, List<Layer> mods, Consumer<String> log) throws IOException {
        game = FsUtil.resolveStrict(game);
        List<ModFile> replacements = new ArrayList<>();
        Map<String, String> owners = new HashMap<>();
        for (Layer layer : mods) {
            int count = 0;
            for (ModFile f : modFiles(layer.folder)) {
                String key = FsUtil.casefold(f.relative);
                if (owners.containsKey(key)) log.accept("Mods: " + f.relative + ": " + owners.get(key) + " -> " + layer.name);
                owners.put(key, layer.name);
                replacements.add(f);
                ++count;
            }
            log.accept("Mods: " + layer.name + ": " + count + " files");
        }
        if (replacements.isEmpty()) return game;
        out = FsUtil.resolve(out);
        if (!out.isDirectory() && !out.mkdirs()) throw new IOException("Cannot create " + out);
        File overlay = Files.createTempDirectory(out.toPath(), "mod-game-").toFile();
        try {
            for (String entry : FsUtil.names(game)) link(new File(overlay, entry), new File(game, entry));
            int replaced = 0, added = 0;
            Names names = new Names();
            String gamePath = game.getPath();
            for (ModFile f : replacements) {
                String[] parts = f.parts();
                File parent = overlay;
                // Each component takes the game's spelling when it exists in another case.
                for (int i = 0; i < parts.length - 1; ++i) {
                    File sub = names.child(parent, parts[i]);
                    boolean existed = sub.exists() || FsUtil.isSymlink(sub);
                    expand(sub, names);
                    if (!existed) names.added(parent, sub.getName());
                    parent = sub;
                }
                File destination = names.child(parent, parts[parts.length - 1]);
                if (FsUtil.isSymlink(destination)) {
                    String resolved = FsUtil.resolve(destination).getPath();
                    if (resolved.equals(gamePath) || resolved.startsWith(gamePath + File.separator)) ++replaced;
                    Files.delete(destination.toPath());
                } else if (destination.exists()) {
                    throw new ModError("File/directory conflict: " + f.relative);
                } else {
                    ++added;
                }
                link(destination, f.source);
                names.added(parent, destination.getName());
            }
            log.accept("Mods: " + replaced + " game files replaced, " + added + " added");
            return overlay;
        } catch (IOException | RuntimeException e) {
            FsUtil.deleteTree(overlay);
            throw e;
        }
    }
}
