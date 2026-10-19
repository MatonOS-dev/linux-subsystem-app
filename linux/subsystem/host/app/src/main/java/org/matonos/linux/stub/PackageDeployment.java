package org.matonos.linux.stub;

import android.content.Context;
import org.matonos.linux.WorkDirs;
import android.content.ComponentName;
import android.content.pm.*;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import org.matonos.linuxhost.stubgen.DeploymentRecord;
import org.matonos.linuxhost.stubgen.StubGenerator;

/** Rebuild Flatpak's disposable lookup metadata from PM's installed index APKs. */
final class PackageDeployment {
    static File prepare(Context context,String appRef) throws Exception {
        File root=new File(WorkDirs.root(context),"flatpak-launch");
        WorkDirs.keep(root); // Held until this one-launch process exits.
        // A process has one launch. A fresh tree also discards removed extensions.
        remove(root.toPath());Files.createDirectories(root.toPath());
        java.nio.file.Files.createFile(new File(root,".v3-store-root").toPath(),
                java.nio.file.attribute.PosixFilePermissions.asFileAttribute(
                        java.nio.file.attribute.PosixFilePermissions.fromString("rw-------")));
        java.util.List<DeclaredPackages.Entry> packages=DeclaredPackages.resolve(context,null);
        if(!packages.get(0).record.ref.equals(appRef))throw new IOException("Installed deployment ref mismatch");
        for(int i=0;i<packages.size();i++) {
            DeclaredPackages.Entry entry=packages.get(i);
            entry.record.materialize(i==0?root:new File(root,"system"),entry.mount);
        }
        return root;
    }
    /** Add the subsystem's bundled GL driver as a normal Flatpak runtime extension deployment. */
    static File prepareGlExtension(File systemRoot,File archive,String branch) throws Exception {
        String ref="runtime/org.freedesktop.Platform.GL.maton/x86_64/"+branch;
        String[] identity;
        byte[] metadata,deploy,commitObject;
        try(ZipFile zip=new ZipFile(archive)) {
            identity=asset(zip,"assets/gl-maton/ref.txt").trim().split("\\r?\\n");
            if(identity.length!=2||!identity[0].equals(ref)||!identity[1].matches("[0-9a-f]{64}"))
                throw new IOException("Invalid bundled Maton GL extension ref");
            metadata=assetBytes(zip,"assets/gl-maton/metadata");
            deploy=assetBytes(zip,"assets/gl-maton/deploy");
            if(deploy.length==0)throw new IOException("Empty bundled Maton GL deployment record");
            commitObject=findCommitObject(assetBytes(zip,"assets/gl-maton/objects.ostree"),identity[1]);
        }
        // Flatpak discovers this ref through the same active-ref layout and
        // metadata path as other installed runtime extensions. The pack index
        // is mounted onto this deployment's files directory below.
        Path root=systemRoot.toPath(),branchRoot=root.resolve(ref),version=branchRoot.resolve(identity[1]);
        Files.createDirectories(version);
        Files.write(version.resolve("metadata"),metadata);
        Files.write(version.resolve("deploy"),deploy);
        Files.write(version.resolve(".ref"),new byte[0]);
        Files.createDirectory(version.resolve("files"));
        link(branchRoot.resolve("active"),Path.of(identity[1]));
        link(root.resolve("runtime/org.freedesktop.Platform.GL.maton/current"),
                Path.of("x86_64",branch));
        Path repo=root.resolve("repo");Files.createDirectories(repo.resolve("objects"));
        Path object=repo.resolve("objects").resolve(identity[1].substring(0,2)).resolve(identity[1].substring(2)+".commit");
        Files.createDirectories(object.getParent());Files.write(object,commitObject);
        Files.createDirectories(repo.resolve("refs/heads"));Files.createDirectories(repo.resolve("refs/remotes"));
        Files.createDirectories(repo.resolve("tmp"));
        Files.write(repo.resolve("config"),"[core]\nrepo_version=1\nmode=bare-user-only\n".getBytes(StandardCharsets.UTF_8));
        return version.resolve("files").toFile();
    }
    private static String asset(ZipFile zip,String name) throws IOException {
        return new String(assetBytes(zip,name),StandardCharsets.UTF_8);
    }
    private static byte[] assetBytes(ZipFile zip,String name) throws IOException {
        ZipEntry entry=zip.getEntry(name);if(entry==null)throw new IOException("Missing bundled Maton GL extension asset: "+name);
        try(InputStream input=zip.getInputStream(entry)){return input.readAllBytes();}
    }
    private static byte[] findCommitObject(byte[] packed,String commit) throws IOException {
        try {
            ByteBuffer bytes=ByteBuffer.wrap(packed).order(ByteOrder.LITTLE_ENDIAN);
            byte[] magic=new byte[8];bytes.get(magic);
            if(!java.util.Arrays.equals(magic,"MTPKOBJ1".getBytes(StandardCharsets.US_ASCII)))
                throw new IOException("Invalid bundled Maton GL object pack");
            long count=Integer.toUnsignedLong(bytes.getInt());
            if(count>bytes.remaining()/40L)throw new IOException("Invalid bundled Maton GL object count");
            for(long i=0;i<count;i++) {
                byte[] checksum=new byte[32];bytes.get(checksum);long length=bytes.getLong();
                if(length<0||length>bytes.remaining())throw new IOException("Invalid bundled Maton GL object length");
                if(hex(checksum).equals(commit)) {
                    if(length>Integer.MAX_VALUE)throw new IOException("Maton GL commit object is too large");
                    byte[] object=new byte[(int)length];bytes.get(object);return object;
                }
                bytes.position(Math.toIntExact(bytes.position()+length));
            }
            throw new IOException("Bundled Maton GL commit object is missing");
        } catch(java.nio.BufferUnderflowException|ArithmeticException failure) {
            throw new IOException("Truncated bundled Maton GL object pack",failure);
        }
    }
    private static String hex(byte[] bytes) {
        StringBuilder value=new StringBuilder(bytes.length*2);
        for(byte b:bytes)value.append(String.format(java.util.Locale.ROOT,"%02x",b&0xff));
        return value.toString();
    }
    private static void link(Path link,Path target) throws IOException {
        Files.deleteIfExists(link);Files.createSymbolicLink(link,target);
    }
    private static void remove(Path root) throws IOException {
        if(!Files.exists(root,LinkOption.NOFOLLOW_LINKS))return;
        Files.walkFileTree(root,new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path p,java.nio.file.attribute.BasicFileAttributes a)throws IOException {Files.delete(p);return FileVisitResult.CONTINUE;}
            @Override public FileVisitResult postVisitDirectory(Path p,IOException e)throws IOException {if(e!=null)throw e;Files.delete(p);return FileVisitResult.CONTINUE;}
        });
    }
    private PackageDeployment(){}
}
