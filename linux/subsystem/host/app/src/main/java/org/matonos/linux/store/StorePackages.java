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

/** Flatpak resolves and downloads into a private installation. Android commits belong to the store. */
public final class StorePackages {
    public static void install(Context context,StoreFlatpak catalogue,Map<String,Object> app,String operation,IInstallCallback callback) throws Exception {
        String ref=(String)app.get("ref"),remote=(String)app.get("origin");
        File work=Files.createTempDirectory(WorkDirs.directory(context,"store-installs").toPath(),"operation-").toFile();
        try(AutoCloseable lease=WorkDirs.keep(work)) {
            StoreFlatpak staging=new StoreFlatpak(context,work);
            catalogue.copyRemotesTo(staging);
            callback.progress(operation,"Flatpak download and verification",0,0,-1);
            // Flatpak selects dependencies, related extensions and subpaths, and verifies signatures.
            List<String> refs=new ArrayList<>();Map<String,String> origins=new LinkedHashMap<>();
            Map<String,JSONObject> manifests=new LinkedHashMap<>();Map<String,String> metadata=new LinkedHashMap<>();
            for(String line:staging.helper("--stage",staging.installation.getAbsolutePath(),remote,ref).split("\n")) {
                if(line.isBlank())continue;String[] fields=line.split("\t",-1);
                if(fields.length!=4||!DeployManifest.validRef(fields[0])||!fields[2].matches("[0-9a-f]{64}"))
                    throw new IOException("Invalid Flatpak staged transaction row: "+line);
                String item=fields[0];refs.add(item);origins.put(item,fields[1]);
                List<String> paths=new ArrayList<>();
                if(!fields[3].isEmpty())for(String path:fields[3].split(","))
                    paths.add(java.net.URLDecoder.decode(path,java.nio.charset.StandardCharsets.UTF_8));
                JSONObject identity=new JSONObject().put("appRef",item).put("appCommit",fields[2]).put("subpaths",new JSONArray(paths));
                File deployment=DeployManifest.stagedDeploymentPath(staging.installation,identity);
                Files.createDirectories(deployment.getParentFile().toPath());
                List<String> checkout=new ArrayList<>(Arrays.asList("checkout","--user-mode"));
                for(String path:paths)checkout.add("--subpath="+path);
                checkout.add(fields[2]);checkout.add(deployment.getAbsolutePath());
                StagedPacks.command(context,work,new File(staging.installation,"repo"),()->false).run(checkout.toArray(new String[0]));
                byte[] text=PlaceholderFiles.readAllBytes(new File(deployment,"metadata"));
                DeployManifest.recordVerifiedPull(staging.installation,item,fields[1],fields[2],paths,text);
                manifests.put(item,identity);metadata.put(item,new String(text,java.nio.charset.StandardCharsets.UTF_8));
            }
            if(!refs.contains(ref))throw new IOException("Flatpak did not stage requested ref "+ref);
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
            Map<String,CommitChain.Followup> extraData=new LinkedHashMap<>();
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
                            StubGenerator.permissionsForMetadata(metadata.get(item)),pkg,base,objects,required,mounts,launchable,mount,StoreExtraData.required(metadata.get(item)));
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
                    packages.add(new CommitChain.Package(pkg,required,plan.write.toArray(new String[0]),plan.remove.toArray(new String[0])));
                    artifacts.put(pkg,artifactsDir);
                    if(StoreExtraData.required(metadata.get(item))) {
                        String normalImage=erofs;
                        extraData.put(pkg,()->StoreExtraData.prepare(context,staging,item,origins.get(item),identity,
                                metadata.get(item),artifactsDir,normalImage,revision));
                    }
                }
            }finally{pool.shutdownNow();}
            callback.progress(operation,"Waiting for Software Centre",95,-1,-1);
            CommitChain.run(packages,new StoreCommitInstaller(operation,callback,artifacts) {
                @Override public CommitChain.Followup followup(CommitChain.Package item) {return extraData.get(item.name);}
            });
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
