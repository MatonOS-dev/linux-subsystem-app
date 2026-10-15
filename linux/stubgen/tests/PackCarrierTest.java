import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.security.cert.*;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.*;
import java.util.zip.*;
import java.lang.reflect.*;
import org.matonos.linuxhost.stubgen.ObjectPacks;
import com.android.apksig.ApkVerifier;

/** Uses a disposable real signer; checks the signed ZIP's original indexed offsets. */
public final class PackCarrierTest {
 public static void main(String[] args)throws Exception {
  File dir=new File(args[0]);File payload=new File(dir,"payload");Files.write(payload.toPath(),new byte[8193]);
  ObjectPacks.Entry row=new ObjectPacks.Entry("/payload","a".repeat(64),"file",0100755);row.source=payload;
  ObjectPacks.initial(Collections.singletonList(row),dir,10000);
  Class<?> desktop=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator$DesktopEntry");
  Constructor<?> dc=desktop.getDeclaredConstructor(String.class,List.class);dc.setAccessible(true);
  Class<?> manifest=Class.forName("org.matonos.linuxhost.stubgen.StubGenerator$BinaryManifest");
  Constructor<?> mc=manifest.getDeclaredConstructor(String.class,desktop,String.class,String.class,List.class);mc.setAccessible(true);
  Object encoder=mc.newInstance("flatpak.org.example.Test",dc.newInstance("",Collections.emptyList()),"","flathub",Collections.emptyList());
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
  if(p.waitFor()!=0 || !xml.contains("split=\""+split+"\"") || !xml.contains("hasCode(0x0101000c)=false"))throw new AssertionError(xml);
  System.out.println("PASS signed code-free pack manifest, content hash name, STORED 4096 alignment and indexed slice");
 }
}
