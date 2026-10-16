package org.matonos.linux.stub;

import android.content.Context;
import android.content.pm.*;
import java.security.MessageDigest;
import java.util.*;
import java.io.IOException;
import org.matonos.linuxhost.stubgen.*;

/** PackageManager attests each snapshot; names and declaring packages are never caller input. */
final class InstalledPackages {
    final Map<String,DeploymentRecord> records=new LinkedHashMap<>();
    private final Context context;
    private final byte[] certificate;
    InstalledPackages(Context context)throws Exception {
        this.context=context;certificate=StubGenerator.getExistingSigningCertificate();
        for(PackageInfo pkg:context.getPackageManager().getInstalledPackages(PackageManager.GET_SIGNING_CERTIFICATES)) {
            if(pkg.packageName==null||!pkg.packageName.startsWith("flatpak.")||!trusted(pkg))continue;
            DeploymentRecord record=read(pkg);
            if(!pkg.packageName.equals(DeployManifest.packageForPublic(record.ref)))throw new IOException("Package name/ref mismatch");
            if(records.put(record.ref,record)!=null)throw new IOException("Duplicate installed ref");
        }
    }
    private boolean trusted(PackageInfo pkg) {
        if(certificate==null||pkg.signingInfo==null||pkg.signingInfo.hasMultipleSigners())return false;
        Signature[] signers=pkg.signingInfo.getApkContentsSigners();
        return signers!=null&&signers.length==1&&MessageDigest.isEqual(certificate,signers[0].toByteArray());
    }
    private DeploymentRecord read(PackageInfo pkg)throws Exception {
        if(!trusted(pkg))throw new SecurityException("Declared package is not signed by this subsystem");
        return DeploymentRecord.installed(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);
    }
    DeploymentRecord installed(String ref)throws Exception {
        try {
            PackageInfo pkg=context.getPackageManager().getPackageInfo(DeployManifest.packageForPublic(ref),PackageManager.GET_SIGNING_CERTIFICATES);
            DeploymentRecord record=read(pkg);
            if(!record.ref.equals(ref))throw new SecurityException("Installed package ref mismatch");
            return record;
        }catch(PackageManager.NameNotFoundException missing){return null;}
    }
    String recordedOrigin(String ref)throws Exception {
        String found=null;
        for(DeploymentRecord record:records.values())for(DeploymentRecord.Dependency dependency:record.dependencies)if(dependency.ref.equals(ref)) {
            if(found!=null&&!found.equals(dependency.remote))throw new IOException("Declaring packages disagree on origin for "+ref);
            found=dependency.remote;
        }
        return found;
    }
    Collection<String> origins() {
        Set<String> result=new LinkedHashSet<>();
        for(DeploymentRecord record:records.values()) {
            result.add(record.remote);
            for(DeploymentRecord.Dependency dependency:record.dependencies)result.add(dependency.remote);
        }
        return result;
    }
}
