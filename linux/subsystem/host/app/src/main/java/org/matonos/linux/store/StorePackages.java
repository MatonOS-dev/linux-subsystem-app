package org.matonos.linux.store;

import android.content.Context;
import android.content.pm.*;
import android.util.Log;
import java.io.*;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.ForkJoinPool;
import org.json.*;
import org.matonos.linux.FlatpakSourceService;
import org.matonos.linux.WorkDirs;
import org.matonos.linux.stub.*;
import org.matonos.linuxhost.stubgen.*;
import org.matonos.store.source.IInstallCallback;

/** Flatpak resolves and deploys in a private installation. Android commits belong to the store. */
public final class StorePackages {
    public static void install(Context context,StoreFlatpak catalogue,Map<String,Object> app,String operation,IInstallCallback callback) throws Exception {
        String ref=(String)app.get("ref"),remote=(String)app.get("origin");
        File work=Files.createTempDirectory(WorkDirs.directory(context,"store-installs").toPath(),"operation-").toFile();
        try(AutoCloseable lease=WorkDirs.keep(work)) {
            StoreFlatpak staging=new StoreFlatpak(context,work);
            catalogue.copyRemotesTo(staging);
            callback.progress(operation,"Flatpak download and verification",0,0,-1);
            // Flatpak selects dependencies, related extensions and subpaths, and verifies signatures.
            staging.cli("install","--system","--noninteractive","--assumeyes",remote,ref);
            callback.progress(operation,"Preparing Android packages",80,-1,-1);
            List<String> refs=new ArrayList<>();Map<String,String> origins=new LinkedHashMap<>();
            for(String line:staging.helper("--installed",staging.installation.getAbsolutePath()).split("\n")) {
                if(line.isBlank())continue;String[] fields=line.split("\t",-1);
                if(fields.length!=2||!DeployManifest.validRef(fields[0]))throw new IOException("Invalid deployed ref row: "+line);
                refs.add(fields[0]);origins.put(fields[0],fields[1]);
            }
            if(!refs.contains(ref))throw new IOException("Flatpak did not deploy requested ref "+ref);
            Map<String,JSONObject> manifests=new LinkedHashMap<>();Map<String,String> metadata=new LinkedHashMap<>();
            for(String item:refs) {
                // Read the exact deployment identity and subpaths selected by Flatpak.
                String location=staging.cli("info","--system","--show-location",item).trim();
                File deployment=new File(location).getCanonicalFile();
                if(!deployment.toPath().startsWith(staging.installation.getCanonicalFile().toPath()))throw new IOException("Flatpak deployment escapes staging: "+location);
                String commit=staging.cli("info","--system","--show-commit",item).trim();
                DeployManifest.Metadata record=DeployManifest.parse(Files.readAllBytes(new File(deployment,"deploy").toPath()));
                if(!record.commit.equals(commit)||!record.origin.equals(origins.get(item)))throw new IOException("Deployment identity changed: "+item);
                JSONArray subpaths=new JSONArray(record.subpaths);
                JSONObject identity=new JSONObject().put("appRef",item).put("appCommit",commit).put("subpaths",subpaths);
                if(!DeployManifest.stagedDeployment(staging.installation,identity).getCanonicalFile().equals(deployment))throw new IOException("Unexpected deployment location for "+item);
                manifests.put(item,identity);metadata.put(item,DeployManifest.stagedMetadata(staging.installation,identity));
            }
            StagedPacks.verify(context,staging.installation,()->false);
            // These edges describe Android mount carriers AFTER Flatpak has resolved the transaction.
            // Extensions are leaves; each parent carries its selected extensions, and the app carries
            // the whole transaction. No parent edge is inferred from ExtensionOf (which would cycle).
            Map<String,List<String>> dependencies=new LinkedHashMap<>();
            for(String item:refs)dependencies.put(item,new ArrayList<>());
            for(String item:refs)if(!item.equals(ref))dependencies.get(ref).add(item);
            for(String parent:refs)if(!parent.equals(ref))for(String child:refs)
                if(!child.equals(parent)&&parent.equals(PackageMounts.extensionParent(metadata.get(child))))dependencies.get(parent).add(child);
            JSONArray rows=new JSONArray();
            for(String item:refs)rows.put(new JSONObject(manifests.get(item).toString()).put("ref",item)
                    .put("commit",manifests.get(item).getString("appCommit"))
                    .put("origin",catalogue.origin(origins.get(item)))
                    .put("mount",item.equals(ref)?"/app":PackageMounts.mountPoint(item,metadata.get(item),metadata)));
            Map<String,File> artifacts=new LinkedHashMap<>();List<CommitChain.Package> packages=new ArrayList<>();
            ForkJoinPool pool=new ForkJoinPool(Runtime.getRuntime().availableProcessors());
            try {
                for(int i=0;i<refs.size();i++) {
                    String item=refs.get(i),pkg=DeployManifest.packageForPublic(item);JSONObject identity=manifests.get(item);
                    String mount=item.equals(ref)?"/app":PackageMounts.mountPoint(item,metadata.get(item),metadata);
                    JSONObject manifest=new JSONObject(identity.toString()).put("origin",catalogue.origin(origins.get(item)))
                            .put("mount",mount).put("dependencies",new JSONArray(dependencies.get(item))).put("deployments",rows);
                    File artifactsDir=new File(work,"package-"+i),objects=new File(artifactsDir,"objects");Files.createDirectories(artifactsDir.toPath());
                    PackageInfo prior=null;try {prior=context.getPackageManager().getPackageInfo(pkg,PackageManager.GET_SIGNING_CERTIFICATES);}
                    catch(PackageManager.NameNotFoundException absent) { }
                    if(prior!=null) {
                        if(!FlatpakSourceService.trusted(prior))throw new IOException("Existing package has another signer: "+pkg);
                        DeploymentRecord installed=DeploymentRecord.installed(prior.splitNames,prior.applicationInfo.splitSourceDirs);
                        if(!installed.ref.equals(item)||!installed.remote.equals(catalogue.origin(origins.get(item))))
                            throw new IOException("Existing package has another origin or ref: "+pkg+"; uninstall before changing origin");
                    }
                    // StagedPacks uses a private pack-repo in installation.getParentFile(); clear it per package.
                    File packRepo=new File(work,"pack-repo");if(packRepo.exists())WorkDirs.remove(packRepo);
                    StagedPacks.build(context,staging.installation,manifest,objects,()->false,prior,pool);
                    File scratch=new File(artifactsDir,"signing");Files.createDirectories(scratch.toPath());
                    boolean launchable=item.equals(ref);String appId=item.split("/")[1];
                    LauncherData launcher=LauncherData.resolve(DeployManifest.stagedDeployment(staging.installation,identity),appId,
                            new File(DeployManifest.stagedDeployment(staging.installation,identity),"files/extra"),null);
                    byte[] icon=launcher.icon==null?fallbackIcon():LauncherData.insetIcon(launcher.icon);
                    List<String> required=new ArrayList<>();Map<String,String> mounts=new TreeMap<>();
                    for(String dep:dependencies.get(item)) {
                        String target=PackageMounts.mountPoint(dep,metadata.get(dep),metadata);
                        String depPkg=DeployManifest.packageForPublic(dep);required.add(depPkg);mounts.put(dep,depPkg+">"+target);
                    }
                    File base=new File(artifactsDir,"base.apk");
                    StubGenerator.generatePackage(scratch,item,origins.get(item),launcher.label,icon,
                            StubGenerator.permissionsForMetadata(metadata.get(item)),pkg,base,objects,required,mounts,launchable,mount);
                    List<String> previous=prior==null||prior.splitNames==null?Collections.emptyList():Arrays.asList(prior.splitNames);
                    boolean full=prior==null||prior.getLongVersionCode()!=StubGenerator.STUB_VERSION_CODE||!PackSessionPlan.sameBase(base,new File(prior.applicationInfo.sourceDir));
                    int revision=PackSessionPlan.nextRevision(prior==null?0:prior.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,
                            prior==null?0:prior.baseRevisionCode,prior==null?null:prior.splitRevisionCodes);
                    List<String> desired=new ArrayList<>();
                    String index=StubGenerator.indexSplitName(objects);desired.add(index);
                    StubGenerator.generateIndex(scratch,pkg,objects,new File(artifactsDir,index+".apk"),revision);
                    File image=new File(objects,"deployment.erofs");String erofs=StubGenerator.erofsSplitName(image);desired.add(erofs);
                    StubGenerator.generateErofsPack(scratch,pkg,image,new File(artifactsDir,erofs+".apk"),revision);
                    if(launchable) {
                        String launch=StubGenerator.launcherSplitName(launcher.label,launcher.mimeTypes,icon,launcher.symbolicIcon);desired.add(launch);
                        StubGenerator.generateLauncher(scratch,pkg,launcher.label,launcher.mimeTypes,icon,launcher.symbolicIcon,new File(artifactsDir,launch+".apk"),revision);
                    }
                    PackSessionPlan plan=PackSessionPlan.create(prior!=null,prior==null?0:prior.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,previous,desired,desired,full);
                    packages.add(new CommitChain.Package(pkg,required,plan.mode==PackSessionPlan.Mode.INHERIT,plan.write.toArray(new String[0]),plan.remove.toArray(new String[0])));
                    artifacts.put(pkg,artifactsDir);
                }
            }finally{pool.shutdownNow();}
            callback.progress(operation,"Waiting for Software Centre",95,-1,-1);
            CommitChain.run(packages,new StoreCommitInstaller(operation,callback,artifacts));
        }finally {
            try{WorkDirs.remove(work);}catch(Exception error){Log.e("FlatpakSource","cleanup op="+operation+" path="+work+" failed",error);}
        }
    }
    private static byte[] fallbackIcon() {
        android.graphics.Bitmap image=android.graphics.Bitmap.createBitmap(96,96,android.graphics.Bitmap.Config.ARGB_8888);image.eraseColor(0xff365f79);
        ByteArrayOutputStream output=new ByteArrayOutputStream();image.compress(android.graphics.Bitmap.CompressFormat.PNG,100,output);image.recycle();return output.toByteArray();
    }
    private StorePackages() { }
}
