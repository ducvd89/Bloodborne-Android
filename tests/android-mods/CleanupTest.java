package com.ducvd89.bloodborne;

import static com.ducvd89.bloodborne.T.*;

import java.io.File;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** Host-JDK tests of RuntimeCleanup, the removal of the former Linux runtime on an app update. The old rootfs held
 *  absolute symlinks that point out of the app's folder: nothing outside may be touched. */
public final class CleanupTest {
    /** An old install: the rootfs with links out, the glibc bundle, drivers, and the data that must stay. */
    static final class Old {
        final File root = Files.createTempDirectory("bbclean").toFile(), files, outside;
        Old() throws Exception {
            files = new File(root, "files"); outside = new File(root, "outside");
            write(new File(outside, "precious.txt"), "keep");
            write(new File(outside, "dir/inner.txt"), "keep");
            write(new File(files, "rootfs/usr/bin/Xvfb"), "x");
            write(new File(files, "rootfs/tmp/sock"), "x");
            Files.createSymbolicLink(new File(files, "rootfs/etc-link").toPath(), outside.toPath());          // absolute, out of the app
            Files.createSymbolicLink(new File(files, "rootfs/usr/bin/sh").toPath(), new File("/system/bin/sh").toPath());
            Files.createSymbolicLink(new File(files, "rootfs/loop").toPath(), new File("loop").toPath());     // dangling/self
            write(new File(files, "bbport/arm64/bin/bb-probe"), "x");
            write(new File(files, "bbport/arm64/patch-parts/cheats.bin"), "x");
            write(new File(files, "bbport/scripts/prepare.py"), "x");
            write(new File(files, "bbport/run-thor.sh"), "x");
            write(new File(files, "bbport/mods.sh"), "x");
            write(new File(files, "bbport/run-common.sh"), "x");
            write(new File(files, "bbport/lsfg-vk.toml"), "x");
            Files.createSymbolicLink(new File(files, "bbport/rootfs").toPath(), new File(files, "rootfs/tmp").toPath());
            write(new File(files, "drivers/.catalog.json"), "{}");
            write(new File(files, "drivers/.catalog-android.json"), "{}");
            write(new File(files, "drivers/winlator-turnip/icd.json"), "{}");
            write(new File(files, "drivers/winlator-turnip/libvulkan_freedreno.so"), "x");
            write(new File(files, "drivers/turnip-android/driver.lib"), "libvulkan_freedreno.so\n");
            write(new File(files, "drivers/turnip-android/libvulkan_freedreno.so"), "x");
            write(new File(files, "drivers/hmi-only/driver.lib"), "vulkan.ad07XX.so\n");
            write(new File(files, "drivers/mixed/icd.json"), "{}");   // an Android driver that also has an icd.json
            write(new File(files, "drivers/mixed/driver.lib"), "lib.so\n");
            Files.createSymbolicLink(new File(files, "drivers/linked").toPath(), outside.toPath()); // a link: never followed
            write(new File(files, "bbport/user/1000/CUSA03173/savedata.bin"), "SAVE");
            write(new File(files, "bbport/bbport.ini"), "upscaler=fsr3\n");
            write(new File(files, "bbport/mods.json"), "{}");
            write(new File(files, "bbport/game/eboot.bin"), "g");
            write(new File(files, "bbport/data/boot-linked.bin"), "d");
            write(new File(files, "bbport/Lossless.dll"), "dll");
            write(new File(files, "bbport/bionic/bin/bb-probe"), "new");
            write(new File(files, "bbport/audio.fifo.note"), "x");
            write(new File(files, "home/bbport/.fex-emu/Config.json"), "{}");
        }
        void close() { delete(root); }
    }
    interface Body { void run(Old old) throws Exception; }
    static void with(Body body) throws Exception { Old old = new Old(); try { body.run(old); } finally { old.close(); } }
    static boolean present(Old old, String relative) { return new File(old.files, relative).exists() || Files.isSymbolicLink(new File(old.files, relative).toPath()); }

    public static void main(String[] args) {
        test("cleanup: the Linux runtime goes, links are not followed", () -> with(old -> {
            yes(RuntimeCleanup.pending(old.files));
            List<String> log = new ArrayList<>();
            eq(0, RuntimeCleanup.run(old.files, log::add), log.toString());
            for (String gone : new String[]{"rootfs", "bbport/arm64", "bbport/scripts", "bbport/run-thor.sh", "bbport/mods.sh",
                    "bbport/run-common.sh", "bbport/lsfg-vk.toml", "bbport/rootfs", "drivers/.catalog.json", "drivers/winlator-turnip"})
                no(present(old, gone), gone + " should be removed");
            // What the links pointed to is intact.
            eq("keep", text(new File(old.outside, "precious.txt")));
            eq("keep", text(new File(old.outside, "dir/inner.txt")));
            no(RuntimeCleanup.pending(old.files), "nothing pending afterwards");
        }));
        test("cleanup: saves, settings, mods.json, game, data, Bionic drivers and the new runtime stay", () -> with(old -> {
            RuntimeCleanup.run(old.files, s -> {});
            for (String kept : new String[]{"bbport/user/1000/CUSA03173/savedata.bin", "bbport/bbport.ini", "bbport/mods.json",
                    "bbport/game/eboot.bin", "bbport/data/boot-linked.bin", "bbport/Lossless.dll", "bbport/bionic/bin/bb-probe",
                    "drivers/.catalog-android.json", "drivers/turnip-android/driver.lib", "drivers/turnip-android/libvulkan_freedreno.so",
                    "drivers/hmi-only/driver.lib", "drivers/mixed/icd.json", "drivers/mixed/driver.lib", "home/bbport/.fex-emu/Config.json"})
                yes(present(old, kept), kept + " must stay");
            // A link in drivers/ is not a driver folder: neither followed nor listed as Linux.
            yes(Files.isSymbolicLink(new File(old.files, "drivers/linked").toPath()));
            yes(new File(old.outside, "precious.txt").isFile());
        }));
        test("cleanup: idempotent, and nothing pending on a fresh install", () -> with(old -> {
            RuntimeCleanup.run(old.files, s -> {});
            eq(0, RuntimeCleanup.run(old.files, s -> {}));
            File fresh = new File(old.root, "fresh");
            fresh.mkdirs();
            no(RuntimeCleanup.pending(fresh));
            eq(0, RuntimeCleanup.run(fresh, s -> {}));
        }));
        test("cleanup: a link in the middle of a path is not followed out of the app's folder", () -> with(old -> {
            // bbport itself replaced by a link to another folder (not something the app does): its legacy
            // paths are skipped, not deleted through the link.
            File elsewhere = new File(old.root, "elsewhere");
            write(new File(elsewhere, "arm64/keep.txt"), "keep");
            write(new File(elsewhere, "scripts/keep.py"), "keep");
            File files = new File(old.root, "files2");
            files.mkdirs();
            Files.createSymbolicLink(new File(files, "bbport").toPath(), elsewhere.toPath());
            List<String> log = new ArrayList<>();
            RuntimeCleanup.run(files, log::add);
            yes(new File(elsewhere, "arm64/keep.txt").isFile(), log.toString());
            yes(new File(elsewhere, "scripts/keep.py").isFile(), log.toString());
        }));
        test("cleanup: read-only folders are removed", () -> with(old -> {
            File ro = new File(old.files, "rootfs/usr/ro");
            write(new File(ro, "f"), "x");
            yes(ro.setWritable(false));
            RuntimeCleanup.run(old.files, s -> {});
            no(present(old, "rootfs"));
        }));
        test("cleanup: the list never names a protected path", () -> {
            for (String legacy : RuntimeCleanup.LEGACY)
                for (String keep : RuntimeCleanup.PROTECTED)
                    no(legacy.equals(keep) || legacy.startsWith(keep + "/"), legacy);
        });
        System.exit(T.finish());
    }
}
