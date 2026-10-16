import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.security.cert.*;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.*;
import java.util.zip.*;
import java.lang.reflect.*;
import org.matonos.linuxhost.stubgen.ObjectPacks;
import org.matonos.linuxhost.stubgen.PackLaunch;
import com.android.apksig.ApkVerifier;

/** Uses a disposable real signer; checks the signed ZIP's original indexed offsets. */
public final class PackCarrierTest {
 public static void main(String[] args)throws Exception {
  File dir=new File(args[0]);File payload=new File(dir,"payload");Files.write(payload.toPath(),new byte[8193]);
  ObjectPacks.Entry row=new ObjectPacks.Entry("/payload","a".repeat(64),"file",0100755);row.source=payload;
  ObjectPacks.initial(Collections.singletonList(row),dir,10000);
  Class<?> manifest=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator$BinaryManifest");
  Constructor<?> mc=manifest.getDeclaredConstructor(String.class,String.class,String.class,String.class,List.class);mc.setAccessible(true);
  Object encoder=mc.newInstance("flatpak.org.example.Test","","","flathub",Collections.emptyList());
  Field noActivities=manifest.getDeclaredField("noActivities");noActivities.setAccessible(true);noActivities.setBoolean(encoder,true);
  Field runtimeMount=manifest.getDeclaredField("packs");runtimeMount.setAccessible(true);runtimeMount.set(encoder,"/usr");
  Method encode=manifest.getDeclaredMethod("encode");encode.setAccessible(true);
  File runtimeManifest=new File(dir,"runtime-manifest.apk");
  try(ZipOutputStream z=new ZipOutputStream(new FileOutputStream(runtimeManifest))){z.putNextEntry(new ZipEntry("AndroidManifest.xml"));z.write((byte[])encode.invoke(encoder));z.closeEntry();}
  Process rmanifest=new ProcessBuilder(args[1],"dump","xmltree",runtimeManifest.getPath(),"--file","AndroidManifest.xml").redirectErrorStream(true).start();
  String rxml=new String(rmanifest.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
  if(rmanifest.waitFor()!=0||rxml.contains("E: activity")||rxml.contains("E: service")||!rxml.contains("org.matonos.linuxhost.FLATPAK_REF")||!rxml.contains("org.matonos.linuxhost.PACKS"))throw new AssertionError("Runtime manifest must be activity-free and carry its ref and pack metadata\n"+rxml);
  Field revision=manifest.getDeclaredField("revisionCode");revision.setAccessible(true);revision.setInt(encoder,42);
  Method pm=manifest.getDeclaredMethod("packManifest",String.class);pm.setAccessible(true);
  String split="pack_"+row.pack.substring(5,69);
  File unsigned=new File(dir,"pack-unsigned.apk"),signed=new File(dir,"pack-signed.apk");
  ObjectPacks.carrier(new File(dir,row.pack),unsigned,(byte[])pm.invoke(encoder,split));
  PrivateKey key=KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(Files.readAllBytes(new File(dir,"key.der").toPath())));
  X509Certificate cert=(X509Certificate)CertificateFactory.getInstance("X.509").generateCertificate(new FileInputStream(new File(dir,"cert.pem")));
  Class<?> material=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator$KeyMaterial");
  Constructor<?> kc=material.getDeclaredConstructor(PrivateKey.class,X509Certificate.class);kc.setAccessible(true);
  Method sign=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator").getDeclaredMethod("sign",File.class,File.class,material);sign.setAccessible(true);
  sign.invoke(null,unsigned,signed,kc.newInstance(key,cert));
  ApkVerifier.Result result=new ApkVerifier.Builder(signed).setMinCheckedPlatformVersion(30).build().verify();
  if(!result.isVerified())throw new AssertionError(result.getErrors().toString());
  try(RandomAccessFile f=new RandomAccessFile(signed,"r");ZipFile zip=new ZipFile(signed)) {
   f.seek(0);byte[] header=new byte[30];f.readFully(header);
   int names=(header[26]&255)|((header[27]&255)<<8),extra=(header[28]&255)|((header[29]&255)<<8);
   if(30+names+extra!=row.offset || row.offset%4096!=0)throw new AssertionError("Signer moved indexed payload");
   if(zip.getEntry(row.checksum).getMethod()!=ZipEntry.STORED)throw new AssertionError("Compressed payload");
   byte[] bytes=new byte[(int)row.length];f.seek(row.offset);f.readFully(bytes);
   if(!Arrays.equals(bytes,Files.readAllBytes(payload.toPath())))throw new AssertionError("Payload slice changed");
  }
  Process p=new ProcessBuilder(args[1],"dump","xmltree",signed.getPath(),"--file","AndroidManifest.xml").redirectErrorStream(true).start();
  String xml=new String(p.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
  if(p.waitFor()!=0 || !xml.contains("split=\""+split+"\"") || !xml.contains("hasCode(0x0101000c)=false") || !xml.contains("revisionCode(0x010104d5)=42"))throw new AssertionError(xml);
  File index=new File(dir,"objects.index");ObjectPacks.writeIndex(index,Collections.singletonList(row));
  List<ObjectPacks.Entry> decoded=ObjectPacks.readIndex(index);
  if(decoded.size()!=1 || !decoded.get(0).pack.equals(row.pack) || decoded.get(0).offset!=row.offset)throw new AssertionError("Index round trip");
  File installedDir=new File(dir,"installed-carriers"),updatedDir=new File(dir,"updated-carriers");installedDir.mkdirs();
  Files.deleteIfExists(new File(installedDir,row.pack).toPath());
  Files.createSymbolicLink(new File(installedDir,row.pack).toPath(),signed.toPath().toAbsolutePath());
  ObjectPacks.Entry fresh=new ObjectPacks.Entry(row.path,row.checksum,row.kind,row.mode);fresh.source=payload;
  ObjectPacks.update(Collections.singletonList(fresh),decoded,installedDir,updatedDir,0.5);
  if(!fresh.pack.equals(row.pack) || fresh.offset!=row.offset || !Arrays.equals(Files.readAllBytes(new File(updatedDir,row.pack).toPath()),Files.readAllBytes(signed.toPath())))throw new AssertionError("Installed APK carrier reuse");
  Files.write(new File(dir,"objects.ostree").toPath(),new byte[0]);
  String indexName=org.matonos.linuxhost.stubgen.StubGenerator.indexSplitName(dir);
  if(!indexName.matches("index_[0-9a-f]{64}"))throw new AssertionError(indexName);
  File indexUnsigned=new File(dir,"index-unsigned.apk"),base=new File(dir,"index-signed.apk");
  Method wi=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator").getDeclaredMethod("writeIndexUnsigned",File.class,String.class,File.class,int.class);wi.setAccessible(true);
  wi.invoke(null,indexUnsigned,"flatpak.org.example.Test",dir,43);
  sign.invoke(null,indexUnsigned,base,kc.newInstance(key,cert));
  if(!new ApkVerifier.Builder(base).setMinCheckedPlatformVersion(30).build().verify().isVerified())throw new AssertionError("Index signature");
  Process ip=new ProcessBuilder(args[1],"dump","xmltree",base.getPath(),"--file","AndroidManifest.xml").redirectErrorStream(true).start();
  String ix=new String(ip.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
  if(ip.waitFor()!=0 || !ix.contains("split=\""+indexName+"\"") || !ix.contains("revisionCode(0x010104d5)=43"))throw new AssertionError(ix);
  List<String> command=org.matonos.linuxhost.stubgen.PackLaunch.arguments(base,new String[]{split,indexName},new String[]{signed.getAbsolutePath(),base.getAbsolutePath()},new File(dir,"cache"));
  if(command.size()!=4||!command.get(0).equals("--pack-mount")||!command.get(1).equals("/app"))throw new AssertionError(command);
  if(!PackLaunch.arguments(base,new String[]{split,indexName},new String[]{signed.getAbsolutePath(),base.getAbsolutePath()},new File(dir,"cache"),"/usr").get(1).equals("/usr"))throw new AssertionError("custom runtime mount point");
  if(!Files.isSameFile(Path.of(command.get(3),row.pack),signed.toPath()))throw new AssertionError("Wrong carrier resolution");
  if(!Arrays.equals(Files.readAllBytes(Path.of(command.get(2))),Files.readAllBytes(index.toPath())))throw new AssertionError("Index changed");
  try {
   org.matonos.linuxhost.stubgen.PackLaunch.arguments(base,new String[]{"index_"+"b".repeat(64),"index_"+"c".repeat(64)},new String[]{base.getAbsolutePath(),base.getAbsolutePath()},new File(dir,"cache"));
   throw new AssertionError("Duplicate index accepted");
  } catch(IOException expected) { }
  try {
   org.matonos.linuxhost.stubgen.PackLaunch.arguments(base,null,null,new File(dir,"cache"));
   throw new AssertionError("Missing split accepted");
  } catch(IOException expected) { }
  System.out.println("PASS launcher resolves PackageManager split names/paths, index and custom mount arguments; missing split rejected");
  System.out.println("PASS signed code-free pack manifest, content hash name, STORED 4096 alignment and indexed slice");
 }
}
