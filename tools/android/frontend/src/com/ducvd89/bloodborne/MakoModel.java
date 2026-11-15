package com.ducvd89.bloodborne;

import android.content.Context;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Locale;

/** The MAKO-FG V1.24 model files that Bloodframe 2.0's experimental MAKO-FG backend runs. The APK carries them
 *  unchanged in assets/mako-fg/ (tools/android/fetch_mako_model.sh pins them, build_frontend.sh packs them);
 *  ensure() copies them into bbport/mako-fg/, where libbbgpu finds the model folder next to lsfg.state
 *  (gpu/shim/bb_lsfg_state.h FindMakoModel). "MAKO FG by Eugenio Segala", CC BY-NC 4.0, non-commercial use only;
 *  the model's LICENSE goes with it. Every file is checked against the SHA-256 of Bloodframe's mako/release.json
 *  (the same pin); Bloodframe checks weights.bin and transport.spv again when it starts. */
final class MakoModel {
    static final String VERSION="1.24";
    static final String ATTRIBUTION="MAKO FG by Eugenio Segala";
    static final String LICENSE_URL="https://creativecommons.org/licenses/by-nc/4.0/";
    static final String SOURCE_URL="https://huggingface.co/eugeniosegala/MAKO-FG";
    /** Path under assets/mako-fg and bbport/mako-fg, SHA-256 (Bloodframe f4ac713 mako/release.json). */
    private static final String[][] FILES={
        {"LICENSE","e990112837dcd5af880065d292f22f394f87c69912f957eabc54dc0285c6a7fc"},
        {"model/weights.bin","08ed3acfefa71a6d29a208e9a64c73d6593bfc2e83eb0230fe4f8d62ff7ea781"},
        {"model/transport.spv","d2285a08b046fcb40ef63254589df3e1defa43e11e8f1026dd9a10c922db992d"},
        {"model/ops.spv","431ec6d0224111435a62e9e1235d844cc9180083e9e5c79461b47d46a2b950ea"},
        {"model/mixture.spv","f5395a370e03783273c68295c1b11215e8c0c7c978e07299b68d7e843d6c0e8e"},
    };
    /** The release's files are about 100 KB together; anything far larger is not them. */
    private static final long MAX_FILE=4L<<20;

    private MakoModel() {}

    /** bbport/mako-fg. */
    static File dir(File base) { return new File(base,"mako-fg"); }

    /** All files present in bbport/mako-fg with the pinned checksums. Reads about 100 KB. */
    static boolean installed(File base) {
        File dir=dir(base);
        for (String[] f:FILES) {
            File file=new File(dir,f[0]);
            try { if (!file.isFile() || !f[1].equalsIgnoreCase(sha256(file))) return false; }
            catch (IOException e) { return false; }
        }
        return true;
    }

    /** Copies the APK's model into bbport/mako-fg when it is not there with the pinned checksums (first start,
     *  an app update with another model, a damaged copy). Through bbport/mako-fg.tmp, then a rename. False when
     *  the APK has no model (a BB_SKIP_MAKO=1 build) or a copy fails; MAKO-FG then stays off. About 100 KB. */
    static boolean ensure(Context context,File base) {
        if (installed(base)) return true;
        File tmp=new File(base,"mako-fg.tmp"), dest=dir(base), old=new File(base,"mako-fg.old");
        FsUtil.deleteTree(tmp);
        try {
            byte[] buffer=new byte[1<<16];
            for (String[] f:FILES) {
                File target=new File(tmp,f[0]);
                File parent=target.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create "+parent);
                MessageDigest hash=digest();
                long done=0;
                try (InputStream in=context.getAssets().open("mako-fg/"+f[0]); FileOutputStream out=new FileOutputStream(target)) {
                    for (int n; (n=in.read(buffer))>0;) {
                        done+=n;
                        if (done>MAX_FILE) throw new IOException(f[0]+" is too large");
                        out.write(buffer,0,n); hash.update(buffer,0,n);
                    }
                    out.getFD().sync();
                }
                if (!hex(hash.digest()).equalsIgnoreCase(f[1])) throw new IOException(f[0]+" does not match its checksum");
            }
            FsUtil.deleteTree(old);
            if (dest.exists() && !dest.renameTo(old)) throw new IOException("Cannot replace "+dest);
            if (!tmp.renameTo(dest)) {
                if (old.exists()) old.renameTo(dest);
                throw new IOException("Cannot move the model into "+dest);
            }
            FsUtil.deleteTree(old);
            return true;
        } catch (IOException e) {
            android.util.Log.w("Bloodborne","MAKO-FG model: "+e.getMessage());
            return false;
        } finally {
            FsUtil.deleteTree(tmp);
        }
    }

    private static MessageDigest digest() throws IOException {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (java.security.NoSuchAlgorithmException e) { throw new IOException(e.toString()); }
    }

    private static String sha256(File file) throws IOException {
        if (file.length()>MAX_FILE) return "";
        MessageDigest hash=digest();
        byte[] buffer=new byte[1<<16];
        try (InputStream in=new FileInputStream(file)) {
            for (int n; (n=in.read(buffer))>0;) hash.update(buffer,0,n);
        }
        return hex(hash.digest());
    }

    private static String hex(byte[] bytes) {
        StringBuilder s=new StringBuilder();
        for (byte b:bytes) s.append(String.format(Locale.ROOT,"%02x",b&0xff));
        return s.toString();
    }
}
