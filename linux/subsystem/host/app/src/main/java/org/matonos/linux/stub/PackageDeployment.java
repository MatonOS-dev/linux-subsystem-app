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
        ApplicationInfo app=context.getApplicationInfo();
        DeploymentRecord record=DeploymentRecord.installed(app.splitNames,app.splitSourceDirs);
        if(!record.ref.equals(appRef))throw new IOException("Installed deployment ref mismatch");
        record.materialize(root,"/app");
        ApplicationInfo identity=context.getPackageManager().getApplicationInfo(context.getPackageName(),PackageManager.GET_META_DATA);
        String map=identity.metaData==null?null:identity.metaData.getString(StubGenerator.RUNTIME_PACKAGES_META);
        java.util.ArrayDeque<String> pending=new java.util.ArrayDeque<>();
        if(map!=null&&!map.isEmpty())java.util.Collections.addAll(pending,map.split(";"));
        java.util.Map<String,String> seen=new java.util.HashMap<>();
        while(!pending.isEmpty()) {
            if(seen.size()>128)throw new IOException("Too many declared packages");
            String row=pending.removeFirst();
            String[] fields=row.split(">",3);if(fields.length!=3)throw new IOException("Invalid runtime record");
            String previous=seen.putIfAbsent(fields[1],row);
            if(previous!=null){if(!previous.equals(row))throw new IOException("Conflicting declared package mount");continue;}
            PackageInfo pkg=context.getPackageManager().getPackageInfo(fields[1],PackageManager.GET_SIGNING_CERTIFICATES);
            PackageInfo own=context.getPackageManager().getPackageInfo(context.getPackageName(),PackageManager.GET_SIGNING_CERTIFICATES);
            if(pkg.signingInfo==null||own.signingInfo==null||pkg.signingInfo.hasMultipleSigners()||own.signingInfo.hasMultipleSigners()
                    ||pkg.signingInfo.getApkContentsSigners().length!=1||own.signingInfo.getApkContentsSigners().length!=1
                    ||!pkg.signingInfo.getApkContentsSigners()[0].equals(own.signingInfo.getApkContentsSigners()[0]))
                throw new SecurityException("Untrusted runtime deployment");
            DeploymentRecord runtime=DeploymentRecord.installed(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);
            if(!runtime.ref.equals(fields[0]))throw new IOException("Runtime deployment ref mismatch");
            runtime.materialize(new File(root,"system"),fields[2]);
            ApplicationInfo dependency=context.getPackageManager().getApplicationInfo(fields[1],PackageManager.GET_META_DATA);
            String children=dependency.metaData==null?null:dependency.metaData.getString(StubGenerator.RUNTIME_PACKAGES_META);
            if(children!=null&&!children.isEmpty())java.util.Collections.addAll(pending,children.split(";"));
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
