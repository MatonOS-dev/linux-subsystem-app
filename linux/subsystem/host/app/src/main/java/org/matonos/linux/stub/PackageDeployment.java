package org.matonos.linux.stub;

import android.content.Context;
import android.content.ComponentName;
import android.content.pm.*;
import java.io.*;
import java.nio.file.*;
import org.matonos.linuxhost.stubgen.DeploymentRecord;
import org.matonos.linuxhost.stubgen.StubGenerator;

/** Rebuild Flatpak's disposable lookup metadata from PM's installed index APKs. */
final class PackageDeployment {
    static File prepare(Context context,String appRef) throws Exception {
        File root=new File(context.getCacheDir().getCanonicalFile(),"flatpak-launch");
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
    private static void remove(Path root) throws IOException {
        if(!Files.exists(root,LinkOption.NOFOLLOW_LINKS))return;
        Files.walkFileTree(root,new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path p,java.nio.file.attribute.BasicFileAttributes a)throws IOException {Files.delete(p);return FileVisitResult.CONTINUE;}
            @Override public FileVisitResult postVisitDirectory(Path p,IOException e)throws IOException {if(e!=null)throw e;Files.delete(p);return FileVisitResult.CONTINUE;}
        });
    }
    private PackageDeployment(){}
}
