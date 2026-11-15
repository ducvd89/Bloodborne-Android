package com.ducvd89.bloodborne;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONObject;

/** Vulkan drivers the player installs from a zip, beside the phone's own driver.
 *  Each one lives in files/drivers/<id>/: the zip's libraries, driver.json (its name and version) and
 *  driver.lib (the file name of the library to load). bbport.ini gpu_driver=<id> picks one ("system": none,
 *  the phone's stock driver; "auto", or empty: the driver bundled in the APK for the GPU, AutoDriver, which
 *  installs it here through install() too).
 *
 *  They are Android builds (adrenotools-style zips: meta.json with libraryName, and the .so, such as our
 *  Turnip for Android); run-bionic.sh passes the library to libbbgpu as BB_VULKAN_DRIVER, which dlopens it.
 *  Linux (glibc) drivers, which the former Linux runtime used (icd.json, no driver.lib), are not loadable
 *  and are not listed: RuntimeCleanup deletes them when the app is updated. */
final class DriverManager {
    /** An installed driver: its folder name, what its zip calls it, its version (or "") and the zip's
     *  file name (or "", when unknown): the download list matches releases by it. */
    static final class Driver {
        final String id, name, version, zip;
        Driver(String id,String name,String version,String zip) { this.id=id; this.name=name; this.version=version; this.zip=zip; }
        String title() { return version.isEmpty() ? name : name+" "+version; }
    }

    private static final long MAX_SIZE=1024L*1024*1024;
    private final File dir;

    /** files: the app's files directory (its drivers/ folder). */
    DriverManager(File files) { dir=new File(files,"drivers"); }

    List<Driver> list() {
        List<Driver> out=new ArrayList<>();
        File[] folders=dir.listFiles();
        if (folders==null) return out;
        java.util.Arrays.sort(folders);
        for (File folder:folders) {
            if (folder.getName().startsWith(".") || !new File(folder,"driver.lib").isFile()) continue;
            String name=folder.getName(), version="", zip="";
            try {
                JSONObject about=new JSONObject(new String(Files.readAllBytes(new File(folder,"driver.json").toPath()),StandardCharsets.UTF_8));
                name=about.optString("name",name); version=about.optString("version",""); zip=about.optString("zip","");
            } catch (Exception ignored) {}
            out.add(new Driver(folder.getName(),name,version,zip));
        }
        return out;
    }
    Driver find(String id) {
        if (id==null || id.isEmpty()) return null;
        for (Driver d:list()) if (d.id.equals(id)) return d;
        return null;
    }
    void delete(String id) throws IOException { if (valid(id)) deleteTree(new File(dir,id)); }
    /** Whether the driver's folder still has the library driver.lib names (an interrupted delete or a cleaned
     *  cache can leave less). */
    boolean intact(String id) {
        if (!valid(id)) return false;
        try {
            String library=new String(Files.readAllBytes(new File(new File(dir,id),"driver.lib").toPath()),StandardCharsets.UTF_8).trim();
            return androidLibraryName(library) && new File(new File(dir,id),library).isFile();
        } catch (IOException e) { return false; }
    }

    /** Unpacks a driver zip (named zipName, for the driver's name when it has none), checks it and
     *  installs it. Returns the new driver; an IOException's message says what is wrong with the zip. */
    synchronized Driver install(InputStream in,String zipName) throws IOException {
        return installAndroid(in,zipName);
    }

    /** Library names of an Android driver zip: vulkan.ad07XX.so, libvulkan_freedreno.so, ... */
    static boolean androidLibraryName(String name) { return name.matches("[A-Za-z0-9._+-]+\\.so(\\.[0-9]+)*"); }

    /** An adrenotools-style zip: meta.json (libraryName, name, driverVersion, ...) and the
     *  library. Every .so is unpacked flat into the driver's folder; driver.lib names the one to load. */
    private Driver installAndroid(InputStream in,String zipName) throws IOException {
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create "+dir);
        File staging=new File(dir,".installing");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("Cannot create "+staging);
        try {
            JSONObject meta=null;
            List<String> libraries=new ArrayList<>();
            long total=0;
            byte[] buffer=new byte[1<<16];
            try (ZipInputStream zip=new ZipInputStream(in)) {
                for (ZipEntry e; (e=zip.getNextEntry())!=null;) {
                    if (e.isDirectory()) continue;
                    String path=e.getName().replace('\\','/');
                    String name=path.substring(path.lastIndexOf('/')+1);
                    if (name.equals("meta.json")) {
                        java.io.ByteArrayOutputStream bytes=new java.io.ByteArrayOutputStream();
                        for (int n; (n=zip.read(buffer))>0;) { bytes.write(buffer,0,n); if (bytes.size()>1<<20) throw new IOException("meta.json is too large"); }
                        if (meta==null) meta=parseMeta(bytes.toString("UTF-8"));
                        continue;
                    }
                    if (!androidLibraryName(name) || libraries.contains(name)) continue;
                    try (FileOutputStream out=new FileOutputStream(new File(staging,name))) {
                        for (int n; (n=zip.read(buffer))>0;) {
                            total+=n;
                            if (total>MAX_SIZE) throw new IOException("The zip's libraries are larger than 1 GB");
                            out.write(buffer,0,n);
                        }
                    }
                    libraries.add(name);
                }
            }
            if (libraries.isEmpty()) throw new IOException("The zip has no driver library (a .so file).");
            String named=meta==null ? "" : meta.optString("libraryName","");
            named=named.substring(named.lastIndexOf('/')+1);
            String library=pickAndroidLibrary(libraries,named);
            if (library==null) throw new IOException(!named.isEmpty() ? "meta.json names "+named+", which is not in the zip. Found: "+String.join(", ",libraries)
                : "The zip has no meta.json with a libraryName, and no single Vulkan driver library. Found: "+String.join(", ",libraries));
            String problem=checkAndroidLibrary(new File(staging,library));
            if (problem!=null) throw new IOException(library+" "+problem);
            String name=meta!=null && !meta.optString("name").isEmpty() ? meta.optString("name") : zipName.replaceAll("(?i)\\.zip$","");
            String version=meta!=null ? meta.optString("driverVersion",meta.optString("packageVersion","")) : "";
            String id=uniqueId(name);
            File target=new File(dir,id);
            JSONObject about=new JSONObject();
            about.put("name",name); about.put("version",version); about.put("library",library); about.put("zip",zipName);
                        if (meta!=null) for (String key:new String[]{"vendor","description","minApi"}) if (meta.has(key)) about.put(key,meta.get(key));
            Files.write(new File(staging,"driver.json").toPath(),about.toString(2).getBytes(StandardCharsets.UTF_8));
            Files.write(new File(staging,"driver.lib").toPath(),(library+"\n").getBytes(StandardCharsets.UTF_8));
            if (!staging.renameTo(target)) throw new IOException("Cannot move the driver to "+target);
            return new Driver(id,name,version,zipName);
        } catch (org.json.JSONException ex) {
            throw new IOException(ex.toString());
        } finally {
            deleteTree(staging);
        }
    }
    private static JSONObject parseMeta(String text) throws IOException {
        try { return new JSONObject(text); } catch (org.json.JSONException ex) { throw new IOException("meta.json is not valid JSON"); }
    }
    /** The library meta.json's libraryName says, else the only libvulkan*.so / vulkan.*.so, else the only .so; null if unclear. */
    static String pickAndroidLibrary(List<String> libraries,String named) {
        if (!named.isEmpty()) return libraries.contains(named) ? named : null;
        String found=null;
        for (String name:libraries) if (name.startsWith("libvulkan") || name.startsWith("vulkan.")) { if (found!=null) return null; found=name; }
        return found!=null ? found : libraries.size()==1 ? libraries.get(0) : null;
    }

    /** Null when the library can load in the game process: a 64-bit ARM Android shared library (it needs
     *  libc.so, not libc.so.6) that is a Vulkan driver. Else what is wrong with it. */
    String checkAndroidLibrary(File so) throws IOException {
        try (RandomAccessFile f=new RandomAccessFile(so,"r"); FileChannel channel=f.getChannel()) {
            if (channel.size()<64 || channel.size()>Integer.MAX_VALUE) return "is not a driver library";
            ByteBuffer b=channel.map(FileChannel.MapMode.READ_ONLY,0,channel.size()).order(ByteOrder.LITTLE_ENDIAN);
            if (b.getInt(0)!=0x464c457f) return "is not a Linux library (ELF)";
            if (b.get(4)!=2 || b.get(5)!=1) return "is not a 64-bit library";
            if (b.getShort(18)!=183) return "is not built for 64-bit ARM (AArch64)";
            if (b.getShort(16)!=3) return "is not a shared library";
            // PT_LOAD maps addresses to file offsets; PT_DYNAMIC has DT_NEEDED and the string table.
            long phoff=b.getLong(32); int size=b.getShort(54)&0xffff, count=b.getShort(56)&0xffff;
            List<long[]> loads=new ArrayList<>();
            long dynamic=-1, dynamicSize=0;
            for (int i=0;i<count;++i) {
                int p=(int)(phoff+(long)i*size);
                if (p<0 || p+56>b.limit()) return "is damaged (program headers)";
                int type=b.getInt(p);
                if (type==1) loads.add(new long[]{b.getLong(p+16),b.getLong(p+8),b.getLong(p+32)});
                if (type==2) { dynamic=b.getLong(p+8); dynamicSize=b.getLong(p+32); }
            }
            if (dynamic<0) return "has no dynamic section";
            long strtab=-1, strsz=0; List<Long> needed=new ArrayList<>();
            for (long d=dynamic; d+16<=dynamic+dynamicSize && d+16<=b.limit(); d+=16) {
                long tag=b.getLong((int)d), value=b.getLong((int)d+8);
                if (tag==0) break;
                if (tag==1) needed.add(value); else if (tag==5) strtab=value; else if (tag==10) strsz=value;
            }
            long strings=-1;
            for (long[] load:loads) if (strtab>=load[0] && strtab<load[0]+load[2]) strings=strtab-load[0]+load[1];
            if (strings<0 || strings+strsz>b.limit()) return "is damaged (string table)";
            List<String> libs=new ArrayList<>();
            for (long offset:needed) libs.add(cString(b,(int)(strings+offset)));
            if (libs.contains("libc.so.6")) return "is a Linux (glibc) build, such as Turnip for Winlator or Mobox. "
                +"The game needs a driver built for Android (an adrenotools-style zip with meta.json).";
            if (!libs.contains("libc.so")) return "is not an Android (Bionic) library";
            byte[] table=new byte[(int)strsz]; b.position((int)strings); b.get(table);
            String names=new String(table,StandardCharsets.ISO_8859_1);
            // Also an Android Vulkan HAL module (adrenotools packs exporting only "HMI").
            if (!names.contains("\0vk_icdGetInstanceProcAddr\0") && !names.contains("\0vkGetInstanceProcAddr\0")
                    && !names.contains("\0HMI\0"))
                return "is not a Vulkan driver (it exports none of vk_icdGetInstanceProcAddr, vkGetInstanceProcAddr and HMI)";
            return null; // its other libraries are the phone's own
        }
    }
    private static String cString(ByteBuffer b,int at) {
        int end=at; while (end<b.limit() && b.get(end)!=0) ++end;
        byte[] bytes=new byte[end-at]; for (int i=0;i<bytes.length;++i) bytes[i]=b.get(at+i);
        return new String(bytes,StandardCharsets.UTF_8);
    }

    /** Folder names run-bionic.sh accepts: letters, digits, '.', '_' and '-', not starting with '.'. */
    static boolean valid(String id) { return id!=null && id.matches("[A-Za-z0-9_-][A-Za-z0-9._-]*"); }
    private String uniqueId(String name) {
        String base=name.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9._-]+","-").replaceAll("^[.-]+|-+$","");
        if (base.isEmpty()) base="driver";
        if (base.length()>48) base=base.substring(0,48);
        String id=base;
        for (int i=2; new File(dir,id).exists(); ++i) id=base+"-"+i;
        return id;
    }
    private static void deleteTree(File file) throws IOException {
        if (file.isDirectory() && !Files.isSymbolicLink(file.toPath())) {
            File[] children=file.listFiles();
            if (children!=null) for (File child:children) deleteTree(child);
        }
        if (file.exists() && !file.delete()) throw new IOException("Cannot delete "+file);
    }
}
