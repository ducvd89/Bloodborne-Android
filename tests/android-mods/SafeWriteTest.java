package ro.cobrabm.fexdroid;
import java.io.File; import java.nio.file.*; import java.util.*;
public class SafeWriteTest {
  static void check(boolean c,String m){ if(!c) throw new AssertionError(m); System.out.println("ok: "+m); }
  public static void main(String[] a) throws Exception {
    File d=Files.createTempDirectory("sw").toFile(); File f=new File(d,"bbport.ini");
    SafeWrite.writeLines(f,Arrays.asList("gpu_driver=x","a=1"));
    check(new String(Files.readAllBytes(f.toPath())).equals("gpu_driver=x\na=1\n"),"write content");
    check(!new File(d,"bbport.ini.tmp").exists(),"no tmp left");
    SafeWrite.writeLines(f,Arrays.asList("b=2")); check(SafeWrite.readLines(f).equals(Arrays.asList("b=2")),"overwrite + readLines");
    Files.write(f.toPath(),new byte[1650]); check(SafeWrite.readText(f)==null,"all-NUL -> null");
    check(SafeWrite.readLines(f).isEmpty(),"all-NUL -> no lines");
    check(SafeWrite.repair(f)&&!f.exists(),"all-NUL repaired = deleted");
    byte[] mix=new byte[1650+8]; System.arraycopy("a=1\nb=2\n".getBytes(),0,mix,1650,8); Files.write(f.toPath(),mix);
    check(SafeWrite.readLines(f).equals(Arrays.asList("a=1","b=2")),"mixed NUL tolerated");
    check(SafeWrite.repair(f)&&new String(Files.readAllBytes(f.toPath())).equals("a=1\nb=2\n"),"mixed repaired");
    check(!SafeWrite.repair(f),"clean file untouched"); check(SafeWrite.readText(new File(d,"none"))==null,"missing -> null");
  }
}
