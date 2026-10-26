package org.matonos.linux.stub;

import android.content.Context;
import android.content.pm.*;
import java.io.*;
import java.util.*;
import org.matonos.linuxhost.stubgen.*;

/** One signed package graph for launch, disposable deployments and apply_extra. */
final class DeclaredPackages {
    interface Repair {void repair(String ref)throws Exception;}
    static final class Entry {
        final PackageInfo pkg;final DeploymentRecord record;final String mount;
        Entry(PackageInfo pkg,DeploymentRecord record,String mount){this.pkg=pkg;this.record=record;this.mount=mount;}
    }
    static List<Entry> resolve(Context context,Repair repair)throws Exception {
        PackageManager pm=context.getPackageManager();
        PackageInfo own=pm.getPackageInfo(context.getPackageName(),PackageManager.GET_SIGNING_CERTIFICATES);
        if(own.signingInfo==null||own.signingInfo.hasMultipleSigners()||own.signingInfo.getApkContentsSigners().length!=1)throw new SecurityException("Invalid package signer");
        Signature expected=own.signingInfo.getApkContentsSigners()[0];
        DeploymentRecord root=DeploymentRecord.installed(own.splitNames,own.applicationInfo.splitSourceDirs);
        Map<String,PackageInfo> packages=new HashMap<>();packages.put(root.ref,own);
        List<PackageGraph.Entry> graph=PackageGraph.resolve(root,ref->{
            String name=DeployManifest.packageForPublic(ref);PackageInfo pkg;
            try{pkg=pm.getPackageInfo(name,PackageManager.GET_SIGNING_CERTIFICATES);}
            catch(PackageManager.NameNotFoundException missing){
                if(repair==null)throw missing;repair.repair(ref);
                pkg=pm.getPackageInfo(name,PackageManager.GET_SIGNING_CERTIFICATES);
            }
            if(pkg.signingInfo==null||pkg.signingInfo.hasMultipleSigners()||pkg.signingInfo.getApkContentsSigners().length!=1
                    ||!expected.equals(pkg.signingInfo.getApkContentsSigners()[0]))throw new SecurityException("Untrusted declared package: "+name);
            packages.put(ref,pkg);
            return DeploymentRecord.installed(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);
        });
        List<Entry> result=new ArrayList<>();
        for(PackageGraph.Entry entry:graph)result.add(new Entry(packages.get(entry.record.ref),entry.record,entry.mount));
        return result;
    }
    static List<String> arguments(List<Entry> packages,File cache,boolean rootExtra)throws Exception {
        return arguments(packages,cache,rootExtra,null);
    }
    static List<String> arguments(List<Entry> packages,File cache,boolean rootExtra,File deployment)throws Exception {
        List<String> args=new ArrayList<>();
        Map<String,String> launchTargets=rootExtra?PackageMounts.launchTargets(records(packages)):null;
        for(int i=0;i<packages.size();i++) {
            Entry entry=packages.get(i);PackageInfo pkg=entry.pkg;
            String branch=entry.record.ref.startsWith("runtime/")?entry.record.ref.substring(entry.record.ref.lastIndexOf('/')+1):"";
            String target=rootExtra?launchTargets.get(entry.record.ref):PackageMounts.launchTarget(entry.record.ref,entry.mount);
            // The signed Flatpak deployment remains available for metadata and
            // dependency resolution, but unrelated staged packages do not
            // overlay a path in this app's filesystem.
            if(target==null)continue;
            args.addAll(ErofsLaunch.arguments(new File(pkg.applicationInfo.sourceDir),pkg.splitNames,pkg.applicationInfo.splitSourceDirs,cache,target,branch));
            if(deployment!=null) {
                File installation=i==0?deployment:new File(deployment,"system");
                File files=new File(installation,entry.record.ref+"/"+entry.record.commit+"/files");
                args.addAll(PackLaunch.backingArguments(target,files));
            }
        }
        return args;
    }
    private static List<DeploymentRecord> records(List<Entry> packages) {
        List<DeploymentRecord> result=new ArrayList<>(packages.size());
        for(Entry entry:packages)result.add(entry.record);
        return result;
    }
    private DeclaredPackages(){}
}
