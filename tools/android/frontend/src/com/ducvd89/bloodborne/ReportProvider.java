package com.ducvd89.bloodborne;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/** Hands the report zip to the share sheet as a content:// URI (a file:// URI cannot be shared on Android 7+).
 *  Serves read-only files that sit directly in the app's reports folder (files/reports) and nothing else.
 *  Not exported: only a URI grant that the app itself gives with the share intent opens it. */
public final class ReportProvider extends ContentProvider {
    static String authority(android.content.Context c) { return c.getPackageName()+".reports"; }
    static Uri uriOf(android.content.Context c,File zip) {
        return new Uri.Builder().scheme("content").authority(authority(c)).appendPath(zip.getName()).build();
    }

    /** The file of a URI: content://<authority>/<name> with a plain name inside the reports folder, else null. */
    private File fileOf(Uri uri) {
        if (getContext()==null || uri.getPathSegments().size()!=1) return null;
        String name=uri.getLastPathSegment();
        if (name==null || !name.startsWith(LogReport.PREFIX) || !name.endsWith(".zip") || name.indexOf('/')>=0 || name.contains("..")) return null;
        try {
            File dir=LogReport.reportsDir(getContext()).getCanonicalFile();
            File file=new File(dir,name).getCanonicalFile();
            return dir.equals(file.getParentFile()) && file.isFile() ? file : null;
        } catch (IOException e) { return null; }
    }

    @Override public boolean onCreate() { return true; }
    @Override public ParcelFileDescriptor openFile(Uri uri,String mode) throws FileNotFoundException {
        File file=fileOf(uri);
        if (file==null || !"r".equals(mode)) throw new FileNotFoundException(uri.toString());
        return ParcelFileDescriptor.open(file,ParcelFileDescriptor.MODE_READ_ONLY);
    }
    /** The name and size, which the receiving app shows and uses for the upload. */
    @Override public Cursor query(Uri uri,String[] projection,String selection,String[] args,String order) {
        File file=fileOf(uri);
        if (file==null) return null;
        String[] columns=projection!=null ? projection : new String[]{OpenableColumns.DISPLAY_NAME,OpenableColumns.SIZE};
        MatrixCursor cursor=new MatrixCursor(columns,1);
        Object[] row=new Object[columns.length];
        for (int i=0; i<columns.length; i++)
            row[i]=OpenableColumns.DISPLAY_NAME.equals(columns[i]) ? file.getName() : OpenableColumns.SIZE.equals(columns[i]) ? (Object)file.length() : null;
        cursor.addRow(row);
        return cursor;
    }
    @Override public String getType(Uri uri) { return fileOf(uri)==null ? null : "application/zip"; }
    @Override public Uri insert(Uri uri,ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri,String selection,String[] args) { throw new UnsupportedOperationException(); }
    @Override public int update(Uri uri,ContentValues values,String selection,String[] args) { throw new UnsupportedOperationException(); }
}
