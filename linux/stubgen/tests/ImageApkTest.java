import org.matonos.linuxhost.stubgen.StubGenerator;
import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.security.*;
import java.security.cert.*;
import java.security.spec.*;
import java.util.*;
import java.util.zip.*;
import com.android.apksig.*;

/** Host tests use a disposable key, never AndroidKeyStore or device keys. */
public final class ImageApkTest {
    static final String REF = "runtime/org.example.Platform/x86_64/stable";
    static void require(boolean b) { if (!b) throw new AssertionError(); }
    static Object manifest(String pkg, boolean runtime, boolean extra) throws Exception {
        Class<?> desktop = Class.forName(StubGenerator.class.getName()+"$DesktopEntry");
        Constructor<?> dc = desktop.getDeclaredConstructors()[0]; dc.setAccessible(true);
        Object entry = dc.newInstance("Example", Collections.emptyList());
        Class<?> cls = Class.forName(StubGenerator.class.getName()+"$BinaryManifest");
        Constructor<?> mc = cls.getDeclaredConstructors()[0]; mc.setAccessible(true);
        Object m = mc.newInstance(pkg, entry, "app/org.example.App/x86_64/stable", Collections.emptyList());
        Field dep = cls.getDeclaredField("runtime"); dep.setAccessible(true);
        if (!extra) dep.set(m, new StubGenerator.RuntimeDependency(REF, 7, String.join("", Collections.nCopies(64, "a"))));
        Field rt = cls.getDeclaredField("runtimeApk"); rt.setAccessible(true); rt.setBoolean(m,runtime);
        Field split = cls.getDeclaredField("split"); split.setAccessible(true); if (extra) split.set(m,"extra");
        return m;
    }
    static byte[] encode(Object m) throws Exception {
        Method e = m.getClass().getDeclaredMethod("encode"); e.setAccessible(true); return (byte[])e.invoke(m);
    }
    static void rejects(File apk, String name) throws Exception {
        try { StubGenerator.checkImageEntry(apk,name); throw new AssertionError("accepted malformed ZIP"); }
        catch (IOException expected) { }
    }
    static long central(RandomAccessFile f) throws Exception {
        f.seek(f.length()-6); long p = Integer.toUnsignedLong(Integer.reverseBytes(f.readInt())); return p;
    }
    static void le32(RandomAccessFile f,long pos,long v) throws Exception { f.seek(pos); f.writeInt(Integer.reverseBytes((int)v)); }
    public static void main(String[] args) throws Exception {
        File work = new File(args[0]); work.mkdirs();
        File image = new File(work,"test.erofs"); Files.write(image.toPath(),new byte[8192]);
        Method put = StubGenerator.class.getDeclaredMethod("putImage",ZipOutputStream.class,String.class,File.class); put.setAccessible(true);
        String[] names = {"matonos/code.erofs","matonos/runtime.erofs","matonos/extra.erofs"};
        for (int i=0;i<3;i++) {
            File apk = new File(work,"unsigned-"+i+".apk");
            byte[] xml = encode(manifest(i==1?StubGenerator.runtimePackage(REF):"flatpak.org.example.App",i==1,i==2));
            Files.write(new File(work,"manifest-"+i+".xml").toPath(),xml);
            try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(apk))) {
                put.invoke(null,zip,names[i],image);
                ZipEntry e = new ZipEntry("AndroidManifest.xml"); zip.putNextEntry(e); zip.write(xml); zip.closeEntry();
            }
            require(StubGenerator.checkImageEntry(apk,names[i])==4096);
            if (args.length > 1) {
                Process process = new ProcessBuilder(args[1], "dump", "xmltree", apk.getPath(), "--file", "AndroidManifest.xml").redirectErrorStream(true).start();
                ByteArrayOutputStream text = new ByteArrayOutputStream();
                byte[] bytes = new byte[4096]; int n;
                while ((n = process.getInputStream().read(bytes)) != -1) text.write(bytes,0,n);
                require(process.waitFor()==0);
                String tree = text.toString("UTF-8");
                require(tree.contains(i==1 ? "E: static-library" : i==0 ? "E: uses-static-library" : "split=\"extra\""));
                if (i != 2) require(tree.contains(REF) && tree.contains("version(0x01010519)=7"));
                if (i == 0) require(tree.contains("certDigest(0x01010548)="));
                else require(tree.contains("hasCode(0x0101000c)=false"));
            }
            PrivateKey key = KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(Files.readAllBytes(new File(work,"key.der").toPath())));
            X509Certificate cert;
            try (InputStream in = new FileInputStream(new File(work,"cert.pem"))) { cert=(X509Certificate)CertificateFactory.getInstance("X.509").generateCertificate(in); }
            File signed = new File(work,"signed-"+i+".apk");
            ApkSigner.SignerConfig signer = new ApkSigner.SignerConfig.Builder("test",key,Collections.singletonList(cert)).build();
            new ApkSigner.Builder(Collections.singletonList(signer)).setInputApk(apk).setOutputApk(signed).setMinSdkVersion(30)
                .setV1SigningEnabled(false).setV2SigningEnabled(true).setV3SigningEnabled(true).setV4SigningEnabled(false).setAlignmentPreserved(true).build().sign();
            require(new ApkVerifier.Builder(signed).setMinCheckedPlatformVersion(30).build().verify().isVerified());
            require(StubGenerator.checkImageEntry(signed,names[i])==4096);
            File bad=new File(work,"bad.apk"); Files.copy(apk.toPath(),bad.toPath(),StandardCopyOption.REPLACE_EXISTING);
            try(RandomAccessFile f=new RandomAccessFile(bad,"rw")) { f.seek(8); f.write(8); }
            rejects(bad,names[i]); // compression mismatch
            Files.copy(apk.toPath(),bad.toPath(),StandardCopyOption.REPLACE_EXISTING);
            try(RandomAccessFile f=new RandomAccessFile(bad,"rw")) { f.seek(6); f.write(8); }
            rejects(bad,names[i]); // descriptor
            Files.copy(apk.toPath(),bad.toPath(),StandardCopyOption.REPLACE_EXISTING);
            try(RandomAccessFile f=new RandomAccessFile(bad,"rw")) { le32(f,22,1); }
            rejects(bad,names[i]); // local vs central size
            Files.copy(apk.toPath(),bad.toPath(),StandardCopyOption.REPLACE_EXISTING);
            try(RandomAccessFile f=new RandomAccessFile(bad,"rw")) { le32(f,central(f)+42,1); }
            rejects(bad,names[i]); // local offset
        }
        File unaligned = new File(work,"unaligned.apk");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(unaligned))) {
            ZipEntry e = new ZipEntry(names[0]); CRC32 crc = new CRC32(); crc.update(new byte[]{0});
            e.setMethod(ZipEntry.STORED); e.setSize(1); e.setCompressedSize(1); e.setCrc(crc.getValue());
            zip.putNextEntry(e); zip.write(0); zip.closeEntry();
        }
        rejects(unaligned,names[0]);
        File duplicate = new File(work,"duplicate.apk");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(duplicate))) {
            put.invoke(null,zip,names[0],image);
            ZipEntry e = new ZipEntry(names[2]); zip.putNextEntry(e); zip.write(0); zip.closeEntry();
        }
        rejects(duplicate,names[0]);
        // Small ZIP64 fixture: sentinel sizes/offset with explicit 64-bit extras.
        File z64=new File(work,"zip64.apk");
        try(RandomAccessFile f=new RandomAccessFile(z64,"rw")) {
            f.setLength(0); byte[] name=names[0].getBytes("UTF-8");
            java.nio.ByteBuffer b=java.nio.ByteBuffer.allocate(4096+8+46+name.length+28+22).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            b.putInt(0x04034b50).putShort((short)45).putShort((short)0).putShort((short)0).putInt(0).putInt(0).putInt(-1).putInt(-1).putShort((short)name.length).putShort((short)(4096-30-name.length)).put(name);
            b.putShort((short)1).putShort((short)16).putLong(8).putLong(8);
            b.putShort((short)0x4d41).putShort((short)(4096-b.position()-2)); b.position(4096); b.putLong(0);
            int cd=b.position(); b.putInt(0x02014b50).putShort((short)45).putShort((short)45).putShort((short)0).putShort((short)0).putInt(0).putInt(0).putInt(-1).putInt(-1).putShort((short)name.length).putShort((short)28).putShort((short)0).putShort((short)0).putShort((short)0).putInt(0).putInt(-1).put(name);
            b.putShort((short)1).putShort((short)24).putLong(8).putLong(8).putLong(0);
            int cds=b.position()-cd; b.putInt(0x06054b50).putShort((short)0).putShort((short)0).putShort((short)1).putShort((short)1).putInt(cds).putInt(cd).putShort((short)0); f.write(b.array());
        }
        require(StubGenerator.checkImageEntry(z64,names[0])==4096);
        try(RandomAccessFile f=new RandomAccessFile(z64,"rw")) { f.seek(30+names[0].length()+2); f.write(8); }
        rejects(z64,names[0]);
        // ZIP64 end record + locator as well as per-entry ZIP64 extras.
        try (RandomAccessFile f = new RandomAccessFile(z64,"rw")) {
            // Restore the corrupted extra length before changing the end record.
            f.seek(30+names[0].length()+2); f.write(16);
            long oldEnd = f.length()-22;
            java.nio.ByteBuffer end = java.nio.ByteBuffer.allocate(98).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            long cd = 4104, cdSize = oldEnd-cd;
            end.putInt(0x06064b50).putLong(44).putShort((short)45).putShort((short)45).putInt(0).putInt(0).putLong(1).putLong(1).putLong(cdSize).putLong(cd);
            end.putInt(0x07064b50).putInt(0).putLong(oldEnd).putInt(1);
            end.putInt(0x06054b50).putShort((short)0).putShort((short)0).putShort((short)-1).putShort((short)-1).putInt(-1).putInt(-1).putShort((short)0);
            f.seek(oldEnd); f.write(end.array());
        }
        require(StubGenerator.checkImageEntry(z64,names[0])==4096);
        // No-image generation keeps the existing launcher/resources/DEX entries.
        Class<?> desktopClass=Class.forName(StubGenerator.class.getName()+"$DesktopEntry");
        Constructor<?> desktopConstructor=desktopClass.getDeclaredConstructors()[0]; desktopConstructor.setAccessible(true);
        Method legacy=StubGenerator.class.getDeclaredMethod("writeUnsigned",File.class,String.class,desktopClass,String.class,byte[].class,List.class,File.class,StubGenerator.RuntimeDependency.class);
        legacy.setAccessible(true);
        File old=new File(work,"legacy.apk");
        legacy.invoke(null,old,"flatpak.org.example.App",desktopConstructor.newInstance("Example",Collections.emptyList()),"app/org.example.App/x86_64/stable",new byte[]{1},Collections.emptyList(),null,null);
        try (ZipFile zip=new ZipFile(old)) { require(zip.getEntry("classes.dex")!=null && zip.getEntry("resources.arsc")!=null && zip.getEntry(names[0])==null); }
        rejects(old,names[0]);
        System.out.println("Image APK checks passed (signed alignment, three manifests, malformed headers, ZIP64)");
    }
}
