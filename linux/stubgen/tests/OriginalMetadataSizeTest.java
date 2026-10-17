import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;
import org.matonos.linuxhost.stubgen.*;

/** Optional read-only real-app measurement: REPO REF OUTPUT. */
public final class OriginalMetadataSizeTest {
 public static void main(String[] args)throws Exception {
  File repo=new File(args[0]),out=new File(args[2]);out.mkdirs();
  OstreePacks original=PackSeedTest.read(repo,args[1]);
  ObjectPacks.initial(original.entries,out,ObjectPacks.DEFAULT_PACK_SIZE);
  ObjectPacks.writeIndex(new File(out,"original.index"),original.entries);
  original.writeOriginal(new File(out,"original.ostree"));
  File zipFile=new File(out,"added-index-assets.zip");
  try(ZipOutputStream zip=new ZipOutputStream(new FileOutputStream(zipFile))) {
   for(String name:List.of("original.index","original.ostree")) {
    zip.putNextEntry(new ZipEntry("assets/matonos/"+name));zip.write(Files.readAllBytes(new File(out,name).toPath()));zip.closeEntry();
   }
  }
  // An empty ZIP is 22 bytes: the difference is the added entries and their directory records.
  System.out.println("v3.5 real app="+args[1]+" entries="+original.entries.size()+" original.index="+new File(out,"original.index").length()+" original.ostree="+new File(out,"original.ostree").length()+" compressed index ZIP growth="+(zipFile.length()-22));
 }
}
