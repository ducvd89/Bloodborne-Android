package com.ducvd89.bloodborne;

import java.io.File;

/** Turns picker ids into file paths (no Android classes, so it is tested on a desktop JDK). */
final class DocPath {
    static final String EXTERNAL_STORAGE="com.android.externalstorage.documents";
    private DocPath() {}

    /** A storage volume folder as a path: "primary" is the phone's own storage, "XXXX-XXXX" an SD card or drive. */
    static File volume(File primary,String volume,String path) {
        File base="primary".equalsIgnoreCase(volume) ? primary : new File("/storage/"+volume);
        return path.isEmpty() ? base : new File(base,path);
    }
    /** A document id ("primary:Download/x.pkg", "1A2B-3C4D:x.pkg", "raw:/storage/emulated/0/x.pkg") of this
     *  provider as a path; null for other providers' ids (their files must be copied to be read). */
    static File document(String authority,String id,File primary) {
        if (id==null) return null;
        if (id.startsWith("raw:")) return id.length()>4 && id.charAt(4)=='/' ? new File(id.substring(4)) : null;
        if (!EXTERNAL_STORAGE.equals(authority)) return null;
        int colon=id.indexOf(':');
        return colon<0 ? null : volume(primary,id.substring(0,colon),id.substring(colon+1));
    }
}
