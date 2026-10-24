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
 private static long localHeaderOffset(RandomAccessFile file,String wanted)throws Exception {
  long end=file.length()-22;file.seek(end);byte[] eocd=new byte[22];file.readFully(eocd);
  if((eocd[0]&255)!=0x50||(eocd[1]&255)!=0x4b)throw new AssertionError("ZIP end record missing");
  int count=(eocd[10]&255)|((eocd[11]&255)<<8);
  long at=Integer.toUnsignedLong((eocd[16]&255)|((eocd[17]&255)<<8)|((eocd[18]&255)<<16)|(eocd[19]<<24));
  for(int i=0;i<count;i++) {
   file.seek(at);byte[] h=new byte[46];file.readFully(h);
   if((h[0]&255)!=0x50||(h[1]&255)!=0x4b||(h[2]&255)!=0x01||(h[3]&255)!=0x02)throw new AssertionError("ZIP central entry missing");
   int nameLength=(h[28]&255)|((h[29]&255)<<8),extraLength=(h[30]&255)|((h[31]&255)<<8),commentLength=(h[32]&255)|((h[33]&255)<<8);
   byte[] name=new byte[nameLength];file.readFully(name);
   long local=Integer.toUnsignedLong((h[42]&255)|((h[43]&255)<<8)|((h[44]&255)<<16)|(h[45]<<24));
   if(wanted.equals(new String(name,java.nio.charset.StandardCharsets.UTF_8)))return local;
   at+=46L+nameLength+extraLength+commentLength;
  }
  throw new AssertionError("ZIP entry missing: "+wanted);
 }
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
  if(rmanifest.waitFor()!=0||rxml.contains("E: activity")||!rxml.contains("org.matonos.linux.stub.ExtraService")||!rxml.contains("org.matonos.linuxhost.EXTRA_MOUNT")||!rxml.contains("org.matonos.linuxhost.FLATPAK_REF")||!rxml.contains("org.matonos.linuxhost.PACKS")||rxml.contains("uses-library"))throw new AssertionError("Runtime manifest must be activity-free, carry its ref, pack metadata and the extra service, and declare no uses-library\n"+rxml);
  Field revision=manifest.getDeclaredField("revisionCode");revision.setAccessible(true);revision.setInt(encoder,42);
  Method lm=manifest.getDeclaredMethod("launcherManifest",String.class,String.class,List.class,int.class);lm.setAccessible(true);
  String launcherSplit="launcher_"+"d".repeat(64);
  File launcherManifest=new File(dir,"launcher-manifest.apk");
  try(ZipOutputStream z=new ZipOutputStream(new FileOutputStream(launcherManifest))){z.putNextEntry(new ZipEntry("AndroidManifest.xml"));z.write((byte[])lm.invoke(encoder,launcherSplit,"Editor",Arrays.asList("text/plain","image/png"),0x7f020000));z.closeEntry();}
  Process lp=new ProcessBuilder(args[1],"dump","xmltree",launcherManifest.getPath(),"--file","AndroidManifest.xml").redirectErrorStream(true).start();
  String lxml=new String(lp.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
  if(lp.waitFor()!=0||!lxml.contains("E: activity-alias")||!lxml.contains("targetActivity(0x01010202)=\"org.matonos.linux.stub.StubActivity\"")||!lxml.contains("split=\""+launcherSplit+"\"")||!lxml.contains("label(0x01010001)=\"Editor\"")||!lxml.contains("icon(0x01010002)=@0x7f020000")||!lxml.contains("android.intent.category.LAUNCHER")||!lxml.contains("android.intent.action.VIEW")||!lxml.contains("mimeType(0x01010026)=\"text/plain\"")||!lxml.contains("mimeType(0x01010026)=\"image/png\""))throw new AssertionError("Launcher split manifest is wrong\n"+lxml);
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
  File erofsUnsigned=org.matonos.linuxhost.stubgen.StubGenerator.generateErofsPackUnsigned(
          dir,"flatpak.org.example.Test",payload,44);
  File erofsSigned=new File(dir,"erofs-signed.apk");sign.invoke(null,erofsUnsigned,erofsSigned,kc.newInstance(key,cert));
  ApkVerifier.Result erofsResult=new ApkVerifier.Builder(erofsSigned).setMinCheckedPlatformVersion(30).build().verify();
  if(!erofsResult.isVerified())throw new AssertionError(erofsResult.getErrors().toString());
  try(ZipFile zip=new ZipFile(erofsSigned);RandomAccessFile carrier=new RandomAccessFile(erofsSigned,"r")) {
   String erofsEntry="assets/matonos/deployment.erofs";
   ZipEntry image=zip.getEntry(erofsEntry),metadataEntry=zip.getEntry("assets/matonos/erofs-pack.json");
   if(image==null||metadataEntry==null||image.getMethod()!=ZipEntry.STORED)throw new AssertionError("Missing stored EROFS carrier metadata/image");
   byte[] metadataBytes;try(InputStream input=zip.getInputStream(metadataEntry)){metadataBytes=input.readAllBytes();}
   String metadataText=new String(metadataBytes,java.nio.charset.StandardCharsets.UTF_8);
   String imageHash=ObjectPacks.hex(MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(payload.toPath())));
   if(!metadataText.contains("\"image_sha256\":\""+imageHash+"\"")
           ||!metadataText.contains("\"image_size\":"+payload.length())
           ||!metadataText.contains("\"offset_rule\":\"zip-local-header-v1\"")
           ||!metadataText.contains("\"offset_entry\":\""+erofsEntry+"\"")
           ||!metadataText.contains("\"compression\":\"lz4hc\""))throw new AssertionError(metadataText);
   long local=localHeaderOffset(carrier,erofsEntry);carrier.seek(local);byte[] header=new byte[30];carrier.readFully(header);
   int nameLength=(header[26]&255)|((header[27]&255)<<8),extraLength=(header[28]&255)|((header[29]&255)<<8);
   long imageOffset=local+30+nameLength+extraLength;
   if(imageOffset%4096!=0||imageOffset<local||image.getSize()!=payload.length())throw new AssertionError("EROFS image entry alignment/local-header rule failed");
   if(!new ApkVerifier.Builder(erofsSigned).setMinCheckedPlatformVersion(30).build().verify().isVerified())throw new AssertionError("EROFS split signature");
  }
  Files.deleteIfExists(erofsUnsigned.toPath());
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
  // Multi-entry carrier: embedded index/metadata and a raster icon must not move the payload.
  java.util.Map<String,byte[]> entries=new java.util.LinkedHashMap<>();
  entries.put("AndroidManifest.xml",(byte[])lm.invoke(encoder,launcherSplit,"Editor",Collections.singletonList("text/plain"),0x7f020000));
  entries.put("assets/matonos/objects.index",new byte[]{1,2,3,4,5});
  entries.put("assets/matonos/objects.ostree",new byte[]{9,8,7});
  entries.put("res/drawable/foreground.png",new byte[]{(byte)0x89,0x50,0x4e,0x47,6,7,8});
  File multiUnsigned=new File(dir,"multi-unsigned.apk"),multi=new File(dir,"multi-signed.apk");
  ObjectPacks.carrier(new File(dir,row.pack),multiUnsigned,entries);
  sign.invoke(null,multiUnsigned,multi,kc.newInstance(key,cert));
  if(!new ApkVerifier.Builder(multi).setMinCheckedPlatformVersion(30).build().verify().isVerified())throw new AssertionError("Multi-entry carrier signature");
  try(RandomAccessFile f=new RandomAccessFile(multi,"r");ZipFile zip=new ZipFile(multi)) {
   f.seek(0);byte[] header=new byte[30];f.readFully(header);
   int names=(header[26]&255)|((header[27]&255)<<8),extra=(header[28]&255)|((header[29]&255)<<8);
   if(30+names+extra!=row.offset || row.offset%4096!=0)throw new AssertionError("Multi-entry carrier moved indexed payload");
   byte[] bytes=new byte[(int)row.length];f.seek(row.offset);f.readFully(bytes);
   if(!Arrays.equals(bytes,Files.readAllBytes(payload.toPath())))throw new AssertionError("Multi-entry payload slice changed");
   for(java.util.Map.Entry<String,byte[]> e:entries.entrySet())
    try(InputStream in=zip.getInputStream(zip.getEntry(e.getKey()))){if(in==null||!Arrays.equals(in.readAllBytes(),e.getValue()))throw new AssertionError("Carrier entry changed: "+e.getKey());}
  }
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
  byte[] commitObject=new byte[]{1,9,2};
  String commit=ObjectPacks.hex(MessageDigest.getInstance("SHA-256").digest(commitObject));
  org.matonos.linuxhost.stubgen.DeploymentRecord record=new org.matonos.linuxhost.stubgen.DeploymentRecord(
          "app/org.example.Test/x86_64/stable",commit,"flathub",new byte[]{1},new byte[]{2},commitObject);
  Files.write(new File(dir,"deployment.bin").toPath(),record.encode());
  Files.copy(index.toPath(),new File(dir,"original.index").toPath(),StandardCopyOption.REPLACE_EXISTING);
  Files.write(new File(dir,"original.ostree").toPath(),new byte[]{3,5,7});
  String identity=org.matonos.linuxhost.stubgen.StubGenerator.indexSplitName(dir);
  Files.write(new File(dir,"original.ostree").toPath(),new byte[]{3,5,8});
  if(identity.equals(org.matonos.linuxhost.stubgen.StubGenerator.indexSplitName(dir)))throw new AssertionError("Original metadata excluded from signed split identity");
  String indexName=org.matonos.linuxhost.stubgen.StubGenerator.indexSplitName(dir);
  if(!indexName.matches("index_[0-9a-f]{64}"))throw new AssertionError(indexName);
  File indexUnsigned=new File(dir,"index-unsigned.apk"),base=new File(dir,"index-signed.apk");
  Method wi=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator").getDeclaredMethod("writeIndexUnsigned",File.class,String.class,File.class,int.class);wi.setAccessible(true);
  wi.invoke(null,indexUnsigned,"flatpak.org.example.Test",dir,43);
  sign.invoke(null,indexUnsigned,base,kc.newInstance(key,cert));
  if(!new ApkVerifier.Builder(base).setMinCheckedPlatformVersion(30).build().verify().isVerified())throw new AssertionError("Index signature");
  org.matonos.linuxhost.stubgen.DeploymentRecord installedRecord=org.matonos.linuxhost.stubgen.DeploymentRecord.installed(
          new String[]{indexName},new String[]{base.getAbsolutePath()});
  if(!installedRecord.ref.equals(record.ref)||!Arrays.equals(installedRecord.commitObject,commitObject))throw new AssertionError("Signed index lost its deployment record");
  List<String> original=PackLaunch.originalArguments(new String[]{split,indexName},new String[]{signed.getAbsolutePath(),base.getAbsolutePath()},new File(dir,"original-cache"));
  if(!Arrays.equals(Files.readAllBytes(Path.of(original.get(2))),Files.readAllBytes(index.toPath())))throw new AssertionError("Original index changed");
  try(ZipFile zip=new ZipFile(base)){if(zip.getEntry("assets/matonos/original.ostree")==null)throw new AssertionError("Signed index lost original metadata");}
  Process ip=new ProcessBuilder(args[1],"dump","xmltree",base.getPath(),"--file","AndroidManifest.xml").redirectErrorStream(true).start();
  String ix=new String(ip.getInputStream().readAllBytes(),java.nio.charset.StandardCharsets.UTF_8);
  if(ip.waitFor()!=0 || !ix.contains("split=\""+indexName+"\"") || !ix.contains("revisionCode(0x010104d5)=43"))throw new AssertionError(ix);
  List<String> command=org.matonos.linuxhost.stubgen.PackLaunch.arguments(base,new String[]{split,indexName},new String[]{signed.getAbsolutePath(),base.getAbsolutePath()},new File(dir,"cache"));
  File deploymentFiles=new File(dir,"app/org.example.App/x86_64/stable/commit/files").getAbsoluteFile();
  List<String> backing=PackLaunch.backingArguments("/app",deploymentFiles);
  if(!backing.equals(Arrays.asList("--pack-backing","/app",deploymentFiles.toString())))throw new AssertionError(backing);
  if(!PackLaunch.backingArguments(deploymentFiles.toString(),deploymentFiles).get(2).equals(deploymentFiles.toString()))throw new AssertionError("Identity backing");
  try {PackLaunch.backingArguments("/app",new File("relative/files"));throw new AssertionError("Relative backing accepted");}
  catch(IOException expected){}
  if(command.size()!=4||!command.get(0).equals("--pack-mount")||!command.get(1).equals("/app"))throw new AssertionError(command);
  if(!PackLaunch.arguments(base,new String[]{split,indexName},new String[]{signed.getAbsolutePath(),base.getAbsolutePath()},new File(dir,"cache"),"/usr").get(1).equals("/usr"))throw new AssertionError("custom runtime mount point");
  if(!Files.isSameFile(Path.of(command.get(3),row.pack),signed.toPath()))throw new AssertionError("Wrong carrier resolution");
  if(!Arrays.equals(Files.readAllBytes(Path.of(command.get(2))),Files.readAllBytes(index.toPath())))throw new AssertionError("Index changed");
  File extensionArchive=new File(dir,"host-extension.apk");
  try(ZipOutputStream z=new ZipOutputStream(new FileOutputStream(extensionArchive))) {
   z.putNextEntry(new ZipEntry("assets/gl-maton/objects.index"));z.write(Files.readAllBytes(index.toPath()));z.closeEntry();
   z.putNextEntry(new ZipEntry("assets/gl-maton/"+row.pack));z.write(Files.readAllBytes(new File(dir,row.pack).toPath()));z.closeEntry();
  }
  List<String> extension=PackLaunch.argumentsFromArchive(extensionArchive,"assets/gl-maton",new File(dir,"extension-cache"),"/usr/lib/x86_64-linux-gnu/GL/maton");
  if(!extension.get(1).equals("/usr/lib/x86_64-linux-gnu/GL/maton")||!Files.isRegularFile(Path.of(extension.get(3),row.pack)))throw new AssertionError("APK extension pack was not mounted");
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
