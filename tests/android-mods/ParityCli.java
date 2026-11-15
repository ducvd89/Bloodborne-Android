package com.ducvd89.bloodborne;

import java.io.File;
import java.io.PrintStream;
import java.util.List;

/** The Java port with the command line of tools/android/mods_android.py (list, validate, apply), for the parity check
 *  (parity.py): the same arguments, the same JSON on stdout, the folder to run as the last line of apply, the
 *  "Mods: ..." lines on stderr. */
public final class ParityCli {
    static String quote(String s) {
        if (s == null) return "null";
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            switch (c) {
                case '"': b.append("\\\""); break;
                case '\\': b.append("\\\\"); break;
                case '\n': b.append("\\n"); break;
                case '\r': b.append("\\r"); break;
                case '\t': b.append("\\t"); break;
                default: if (c < 0x20) b.append(String.format("\\u%04x", (int)c)); else b.append(c);
            }
        }
        return b.append('"').toString();
    }
    static String shared(List<ModManager.Shared> l) {
        StringBuilder b = new StringBuilder("[");
        for (ModManager.Shared s : l) { if (b.length() > 1) b.append(", "); b.append("{\"mod\": ").append(quote(s.mod)).append(", \"files\": ").append(s.files).append('}'); }
        return b.append(']').toString();
    }
    static String row(ModManager.Mod m) {
        return "{\"name\": " + quote(m.name) + ", \"enabled\": " + m.enabled + ", \"valid\": " + m.valid + ", \"problem\": " + quote(m.problem)
            + ", \"files\": " + m.files + ", \"bytes\": " + m.bytes + ", \"overrides\": " + shared(m.overrides)
            + ", \"overriddenBy\": " + shared(m.overriddenBy) + "}";
    }
    static String listing(ModManager.Listing l) {
        StringBuilder b = new StringBuilder("{\"modsDir\": " + quote(l.modsDir) + ", \"exists\": " + l.exists + ", \"mods\": [");
        for (ModManager.Mod m : l.mods) { if (!b.toString().endsWith("[")) b.append(", "); b.append(row(m)); }
        return b.append("], \"error\": ").append(quote(l.error)).append('}').toString();
    }

    static String bool(boolean b) { return b ? "true" : "false"; }

    public static void main(String[] args) throws Exception {
        PrintStream out = new PrintStream(System.out, true, "UTF-8"), err = new PrintStream(System.err, true, "UTF-8");
        String command = args[0];
        File modsDir = null, config = null, game = null, to = null, pkg = null, tool = null, dest = null; String name = null; boolean replace = false;
        for (int i = 1; i < args.length; i += 2) {
            if (args[i].equals("--replace")) { replace = true; --i; continue; }
            switch (args[i]) {
                case "--mods-dir": modsDir = new File(args[i + 1]); break;
                case "--config": config = new File(args[i + 1]); break;
                case "--game": game = new File(args[i + 1]); break;
                case "--out": to = new File(args[i + 1]); break;
                case "--name": name = args[i + 1]; break;
                case "--pkg": pkg = new File(args[i + 1]); break;
                case "--tool": tool = new File(args[i + 1]); break;
                case "--dest": dest = new File(args[i + 1]); break;
                default: throw new IllegalArgumentException(args[i]);
            }
        }
        PkgInstaller.Progress progress = (d, t, l) -> out.println("progress " + d + " " + t + " " + l);
        if (command.equals("check")) { // the game check: "ok" or "kind title version"
            GameCheck.Found f = GameCheck.problem(game, false);
            out.println(f == null ? "ok" : f.kind + " " + f.title + " " + f.version);
            return;
        }
        if (command.equals("install-pkg")) {
            PkgInstaller.ModResult r = new PkgInstaller(tool, err::println).installMod(pkg, name, modsDir, game == null || game.getPath().isEmpty() ? null : game, progress, () -> false);
            out.println("{\"ok\": " + bool(r.ok) + ", \"name\": " + quote(r.name) + ", \"problem\": " + quote(r.problem) + ", \"mod\": " + (r.mod == null ? "null" : row(r.mod)) + "}");
            return;
        }
        if (command.equals("install-game")) {
            PkgInstaller.GameResult r = new PkgInstaller(tool, err::println).installGame(pkg, dest, replace, progress, () -> false);
            out.println("{\"ok\": " + bool(r.ok) + ", \"gameDir\": " + quote(r.gameDir) + ", \"titleId\": " + quote(r.titleId) + ", \"category\": " + quote(r.category)
                + ", \"appVer\": " + quote(r.appVer) + ", \"ready\": " + bool(r.ready) + ", \"next\": " + quote(r.next) + ", \"problem\": " + quote(r.problem) + "}");
            return;
        }
        ModManager manager = new ModManager(modsDir, config);
        switch (command) {
            case "list": out.println(listing(manager.list())); break;
            case "validate": out.println(row(manager.validate(name))); break;
            case "apply": out.println(manager.apply(game, to, err::println)); break;
            default: throw new IllegalArgumentException(command);
        }
    }
}
