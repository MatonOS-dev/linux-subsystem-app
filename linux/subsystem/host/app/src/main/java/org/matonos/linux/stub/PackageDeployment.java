package org.matonos.linux.stub;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
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
    static final class CompatI386 {
        final File files;final java.util.List<String> arguments;
        CompatI386(File files,java.util.List<String> arguments){this.files=files;this.arguments=arguments;}
    }
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
        // Deployments are reconstructed for each launch, but Flatpak's
        // per-user overrides are user state and must survive that rebuild.
        Path durableOverrides=new File(context.getDataDir(),
                "home/.local/share/flatpak/overrides").toPath();
        Files.createDirectories(durableOverrides);
        Files.createSymbolicLink(root.toPath().resolve("overrides"),durableOverrides);
        return root;
    }
    /** Add the subsystem's bundled GL driver as a normal Flatpak runtime extension deployment. */
    static File prepareGlExtension(File systemRoot,File archive,String branch) throws Exception {
        return prepareBundledExtension(systemRoot,archive,branch,"assets/gl-maton",
                "runtime/org.freedesktop.Platform.GL.maton/x86_64/",
                "org.freedesktop.Platform.GL.maton",metadata(branch));
    }
    /** Add the i386 GL payload as a separate Flatpak GL32 extension deployment. */
    static File prepareGl32Extension(File systemRoot,File archive,String branch) throws Exception {
        return prepareBundledExtension(systemRoot,archive,branch,"assets/gl-maton-i386",
                "runtime/org.freedesktop.Platform.GL32.maton/x86_64/",
                "org.freedesktop.Platform.GL32.maton",gl32Metadata(branch));
    }
    private static File prepareBundledExtension(File systemRoot,File archive,String branch,
            String assetPrefix,String refPrefix,String extensionName,String extensionMetadata) throws Exception {
        String[] identity;byte[] commitObject;
        try(ZipFile zip=new ZipFile(archive)) {
            identity=asset(zip,assetPrefix+"/ref.txt").trim().split("\\r?\\n");
            if(identity.length!=2||!identity[0].startsWith(refPrefix)
                    ||!identity[1].matches("[0-9a-f]{64}"))
                throw new IOException("Invalid bundled Flatpak extension ref");
            commitObject=findCommitObject(assetBytes(zip,assetPrefix+"/objects.ostree"),identity[1]);
        }
        String ref=refPrefix+branch;
        Path root=systemRoot.toPath(),branchRoot=root.resolve(ref);
        Path version=branchRoot.resolve(identity[1]);Files.createDirectories(version);
        Files.write(version.resolve("metadata"),extensionMetadata.getBytes(StandardCharsets.UTF_8));
        Files.write(version.resolve("deploy"),DeployRecord.extension("flathub",identity[1],branch));
        Files.write(version.resolve(".ref"),new byte[0]);Files.createDirectory(version.resolve("files"));
        link(branchRoot.resolve("active"),Path.of(identity[1]));
        link(root.resolve("runtime/"+extensionName+"/current"),Path.of("x86_64",branch));
        writeCommitObject(root,identity[1],commitObject);
        return version.resolve("files").toFile();
    }
    static CompatI386 prepareCompatI386(Context context,File systemRoot,File cache,String branch) throws Exception {
        String ref="runtime/org.freedesktop.Platform.Compat.i386/x86_64/"+branch;
        PackageManager pm=context.getPackageManager();PackageInfo pkg;
        try{pkg=pm.getPackageInfo(DeployManifest.packageForPublic(ref),PackageManager.GET_SIGNING_CERTIFICATES);}
        catch(PackageManager.NameNotFoundException absent){return null;}
        // Flathub's runtime extension APKs retain their repository signing key;
        // unlike generated app stubs, they are not re-signed with the host key.
        // PackageManager's initiating-package field is set by the installer
        // session and identifies the subsystem app that performed the verified
        // Flatpak install.
        android.content.pm.InstallSourceInfo source=pm.getInstallSourceInfo(pkg.packageName);
        if(source==null||!"org.matonos.linux".equals(source.getInitiatingPackageName()))
            throw new SecurityException("i386 compatibility extension was not installed by the subsystem");
        org.matonos.linuxhost.stubgen.DeploymentRecord record=
                org.matonos.linuxhost.stubgen.DeploymentRecord.installed(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);
        if(!record.ref.equals(ref)||!new String(record.metadata,StandardCharsets.UTF_8).contains("name = org.freedesktop.Platform.Compat.i386"))
            throw new IOException("Installed i386 compatibility extension ref mismatch");
        String mount="/usr/lib/i386-linux-gnu";
        // PackageDeployment.prepare() already materialized every declared
        // package from its signed record. Recreate the Flatpak-canonical
        // metadata in place; DeploymentRecord.materialize() is intentionally
        // a first-write operation and its files/ createDirectory would fail.
        Path version=systemRoot.toPath().resolve(ref).resolve(record.commit);
        Path files=version.resolve("files");
        if(!Files.isDirectory(files,LinkOption.NOFOLLOW_LINKS)||Files.isSymbolicLink(files))
            throw new IOException("Missing materialized i386 compatibility extension");
        Files.write(version.resolve("metadata"),compatMetadata(branch).getBytes(StandardCharsets.UTF_8));
        Files.write(version.resolve("deploy"),DeployRecord.extension(record.remote,record.commit,branch));
        File apk=new File(pkg.applicationInfo.sourceDir);
        java.util.List<String> args=new java.util.ArrayList<>(org.matonos.linuxhost.stubgen.PackLaunch.arguments(
                apk,pkg.splitNames,pkg.applicationInfo.splitSourceDirs,cache,mount));
        args.addAll(org.matonos.linuxhost.stubgen.PackLaunch.backingArguments(mount,files.toFile()));
        return new CompatI386(files.toFile(),args);
    }
    static String withI386ExtensionPoints(String metadata,String branch) {
        StringBuilder result=new StringBuilder(metadata);
        if(!metadata.contains("[Extension org.freedesktop.Platform.Compat.i386]"))
            result.append("\n[Extension org.freedesktop.Platform.Compat.i386]\ndirectory=lib/i386-linux-gnu\nversion=")
                    .append(branch).append("\nadd-ld-path=.\nno-autodownload=true\n");
        if(!metadata.contains("[Extension org.freedesktop.Platform.GL32]"))
            result.append("\n[Extension org.freedesktop.Platform.GL32]\nversions=").append(branch)
                    .append(";1.4\nversion=").append(branch)
                    .append("\ndirectory=lib/i386-linux-gnu/GL\nsubdirectories=true\nadd-ld-path=lib\n")
                    .append("merge-dirs=vulkan/icd.d;glvnd/egl_vendor.d\nno-autodownload=true\n")
                    .append("enable-if=active-gl-driver\ndownload-if=active-gl-driver\n");
        return result.toString();
    }
    static void enableI386Runtime(File systemRoot,String branch) throws IOException {
        Path runtime=systemRoot.toPath().resolve("runtime/org.freedesktop.Platform/x86_64/"+branch);
        Path active=runtime.resolve("active"),metadata=active.resolve("metadata");
        if(!Files.isRegularFile(metadata))throw new IOException("Missing x86_64 Platform metadata for i386 extensions");
        String original=new String(org.matonos.linuxhost.stubgen.PlaceholderFiles.readAllBytes(metadata.toFile()),StandardCharsets.UTF_8);
        Files.write(metadata,withI386ExtensionPoints(original,branch).getBytes(StandardCharsets.UTF_8));
        Path loaderLink=active.resolve("files/lib/ld-linux.so.2");
        Files.createDirectories(loaderLink.getParent());
        if(!Files.exists(loaderLink,LinkOption.NOFOLLOW_LINKS))
            Files.createSymbolicLink(loaderLink,Path.of("/usr/lib/i386-linux-gnu/ld-linux.so.2"));
    }
    static String compatMetadata(String branch) {
        return "[Runtime]\nname=org.freedesktop.Platform.Compat.i386\n\n[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/"+branch+"\n";
    }
    static String gl32Metadata(String branch) {
        return "[Runtime]\nname=org.freedesktop.Platform.GL32.maton\nruntime=org.freedesktop.Platform/x86_64/"+branch
                +"\nsdk=org.freedesktop.Sdk/x86_64/"+branch+"\n\n[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/"+branch+"\n";
    }
    private static void writeCommitObject(Path root,String commit,byte[] bytes) throws IOException {
        Path object=root.resolve("repo/objects").resolve(commit.substring(0,2)).resolve(commit.substring(2)+".commit");
        Files.createDirectories(object.getParent());Files.write(object,bytes);
        Files.createDirectories(root.resolve("repo/refs/heads"));Files.createDirectories(root.resolve("repo/refs/remotes"));
        Files.createDirectories(root.resolve("repo/tmp"));
        Files.write(root.resolve("repo/config"),"[core]\nrepo_version=1\nmode=bare-user-only\n".getBytes(StandardCharsets.UTF_8));
    }
    /** Same text as gpu-bridge-common/flatpak/metadata.in for the given runtime branch. */
    static String metadata(String branch) {
        return "[Runtime]\nname=org.freedesktop.Platform.GL.maton\nruntime=org.freedesktop.Platform/x86_64/"+branch
                +"\nsdk=org.freedesktop.Sdk/x86_64/"+branch+"\n\n[ExtensionOf]\nref=runtime/org.freedesktop.Platform/x86_64/"+branch+"\n";
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
