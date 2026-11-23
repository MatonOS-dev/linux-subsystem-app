package org.matonos.linux.store;

import android.content.Context;
import android.content.pm.*;
import android.os.ParcelFileDescriptor;
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
    public static void install(Context context,StoreFlatpak catalogue,Map<String,Object> app,String operation,IInstallCallback callback,SourceOperations.Operation cancellation) throws Exception {
        install(context,catalogue,app,null,null,false,operation,callback,cancellation);
    }
    public static void installLocalFile(Context context,StoreFlatpak catalogue,ParcelFileDescriptor input,String displayName,boolean allowUnsigned,String operation,IInstallCallback callback,SourceOperations.Operation cancellation) throws Exception {
        install(context,catalogue,null,input,displayName,allowUnsigned,operation,callback,cancellation);
    }
    private static void install(Context context,StoreFlatpak catalogue,Map<String,Object> app,ParcelFileDescriptor input,String displayName,boolean allowUnsigned,String operation,IInstallCallback callback,SourceOperations.Operation cancellation) throws Exception {
        boolean bundle=input!=null;
        String ref=bundle?null:(String)app.get("ref"),remote=bundle?null:(String)app.get("origin");
        File work=Files.createTempDirectory(WorkDirs.directory(context,"store-installs").toPath(),"operation-").toFile();
        try(AutoCloseable lease=WorkDirs.keep(work)) {
            cancellation.check();StoreFlatpak staging=new StoreFlatpak(context,work);staging.cancellation=cancellation;
            catalogue.copyRemotesTo(staging);
            File bundleFile=null;
            if(bundle) {
                if(displayName==null||displayName.isBlank())throw new IllegalArgumentException("Local bundle display name is required");
                bundleFile=new File(work,"local.flatpak");long total=0;byte[] buffer=new byte[65536];
                try(InputStream source=new FileInputStream(input.getFileDescriptor());OutputStream destination=Files.newOutputStream(bundleFile.toPath())) {
                    for(int count;(count=source.read(buffer))!=-1;) {
                        cancellation.check();
                        try { total=Math.addExact(total,(long)count); }
                        catch(ArithmeticException overflow) { throw new IOException("Local bundle byte count overflow displayName="+displayName,overflow); }
                        destination.write(buffer,0,count);
                    }
                } catch(Exception error) {Log.e("FlatpakSource","Copy local bundle displayName="+displayName+" bytes="+total+" op="+operation+" failed",error);throw error;}
                if(total==0)throw new IOException("Local Flatpak bundle is empty displayName="+displayName);
            }
            callback.progress(operation,"Flatpak download and verification",0,0,-1);
            // Flatpak selects dependencies, related extensions and subpaths, and verifies signatures.
            List<String> refs=new ArrayList<>();Map<String,String> origins=new LinkedHashMap<>();
            Map<String,JSONObject> manifests=new LinkedHashMap<>();Map<String,String> metadata=new LinkedHashMap<>();
            String staged=bundle?staging.helper("--stage-bundle",staging.installation.getAbsolutePath(),bundleFile.getAbsolutePath(),Boolean.toString(allowUnsigned)):
                    staging.helper("--stage",staging.installation.getAbsolutePath(),remote,ref);
            String[] stagedLines=staged.split("\n");
            if(bundle) {
                String[] result=stagedLines.length==0?new String[0]:stagedLines[0].split("\t",-1);
                if(result.length!=3||!result[0].equals("BUNDLE")||!DeployManifest.validRef(result[2])||!result[1].equals("SIGNED")&&!result[1].equals("UNSIGNED"))
                    throw new IOException("Invalid Flatpak bundle signature result displayName="+displayName+" firstRow="+(stagedLines.length==0?"<empty>":stagedLines[0]));
                ref=result[2];boolean signed=result[1].equals("SIGNED");
                if(!signed&&!allowUnsigned)throw new IOException("Unsigned Flatpak bundle refused without explicit allowUnsigned displayName="+displayName);
                callback.progress(operation,"Local bundle signature: "+(signed?"signed":"unsigned"),0,0,-1);
            }
            int firstRow=bundle?1:0;
            for(int row=firstRow;row<stagedLines.length;row++) {
                String line=stagedLines[row];
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
                StagedPacks.command(context,work,new File(staging.installation,"repo"),cancellation).run(checkout.toArray(new String[0]));
                byte[] text=PlaceholderFiles.readAllBytes(new File(deployment,"metadata"));
                DeployManifest.recordVerifiedPull(staging.installation,item,fields[1],fields[2],paths,text);
                manifests.put(item,identity);metadata.put(item,new String(text,java.nio.charset.StandardCharsets.UTF_8));
            }
            if(!refs.contains(ref))throw new IOException("Flatpak did not stage requested ref "+ref);
            Map<String,String> packageIds=new HashMap<>();
            Map<String,String> splitRefs=new HashMap<>();
            for(String item:refs) {
                String[] identity=item.split("/",-1);
                String previous=packageIds.putIfAbsent(
                        DeployManifest.packageForPublic(item),identity[1]);
                if(previous!=null&&!previous.equals(identity[1]))
                    throw new IOException("Android package name collision between Flatpak app ids "+previous+" and "+identity[1]);
                String split=DeployManifest.splitForPublic(item),other=splitRefs.putIfAbsent(
                        DeployManifest.packageForPublic(item)+"/"+split,item);
                if(other!=null&&!other.equals(item))throw new IOException("Android split name collision between Flatpak refs "+other+" and "+item);
            }
            StagedPacks.verify(context,staging.installation,cancellation);
            // These edges describe Android mount carriers AFTER Flatpak has resolved the transaction.
            // Extensions are leaves; each parent carries its selected extensions, and the app carries
            // the whole transaction. No parent edge is inferred from ExtensionOf (which would cycle).
            Map<String,List<String>> dependencies=new LinkedHashMap<>();
            for(String item:refs)dependencies.put(item,new ArrayList<>());
            for(String item:refs)if(!item.equals(ref))dependencies.get(ref).add(item);
            for(String parent:refs)if(!parent.equals(ref))for(String child:refs)
                if(!child.equals(parent)&&parent.equals(PackageMounts.mountParent(child,metadata.get(child),metadata)))dependencies.get(parent).add(child);
            JSONArray rows=new JSONArray();
            for(String item:refs)rows.put(new JSONObject(manifests.get(item).toString()).put("ref",item)
                    .put("commit",manifests.get(item).getString("appCommit"))
                    .put("origin",(bundle?staging:catalogue).origin(origins.get(item)))
                    .put("mount",item.equals(ref)?"/app":PackageMounts.mountPoint(item,metadata.get(item),metadata)));
            Map<String,CommitChain.Followup> extraData=new LinkedHashMap<>();
            Map<String,File> artifacts=new LinkedHashMap<>();List<CommitChain.Package> packages=new ArrayList<>();
            ForkJoinPool pool=new ForkJoinPool(Runtime.getRuntime().availableProcessors());
            try {
                // One Android package owns every branch of a Flatpak id (e.g. GL.default 25.08 and 25.08-extra);
                // each branch is its own deployment split inside that package.
                Map<String,List<String>> groups=new LinkedHashMap<>();
                for(String item:refs)groups.computeIfAbsent(DeployManifest.packageForPublic(item),key->new ArrayList<>()).add(item);
                int index=0;
                for(Map.Entry<String,List<String>> group:groups.entrySet()) {
                    cancellation.check();String pkg=group.getKey();List<String> members=group.getValue();
                    // The base describes the requested app, else the last branch: Flatpak binds branches in order, last wins.
                    String primary=members.contains(ref)?ref:members.get(members.size()-1);
                    Map<String,String> mountsOf=new LinkedHashMap<>();
                    for(String item:members)mountsOf.put(item,item.equals(ref)?"/app":PackageMounts.mountPoint(item,metadata.get(item),metadata));
                    String mount=mountsOf.get(primary);
                    for(String item:members)if(!mountsOf.get(item).equals(mount))
                        throw new IOException("Flatpak branches of "+pkg+" mount at different paths: "+primary+" at "+mount+", "+item+" at "+mountsOf.get(item));
                    String extraItem=null;
                    for(String item:members)if(StoreExtraData.required(metadata.get(item))) {
                        if(extraItem!=null)throw new IOException("More than one extra-data branch in "+pkg+": "+extraItem+" and "+item);
                        extraItem=item;
                    }
                    File artifactsDir=new File(work,"package-"+(index++));Files.createDirectories(artifactsDir.toPath());
                    PackageInfo prior=null;try {prior=context.getPackageManager().getPackageInfo(pkg,PackageManager.GET_SIGNING_CERTIFICATES);}
                    catch(PackageManager.NameNotFoundException absent) { }
                    boolean priorHasState=false;
                    if(prior!=null) {
                        if(!FlatpakSourceService.trusted(prior))throw new IOException("Existing package has another signer: "+pkg);
                        List<DeploymentRecord> priorRecords;
                        try{priorRecords=DeploymentRecord.installedAll(prior.splitNames,prior.applicationInfo.splitSourceDirs);priorHasState=true;}
                        catch(IOException missingState){Log.w("FlatpakSource","Existing package has no installed Flatpak state package="+pkg+"; treating as empty",missingState);priorRecords=Collections.emptyList();}
                        for(DeploymentRecord installed:priorRecords)for(String item:members) {
                            String installedId=installed.ref.split("/",-1)[1];
                            if(!installedId.equals(manifests.get(item).getString("appRef").split("/",-1)[1]))
                                throw new IOException("Android package name collision: "+pkg+" already belongs to Flatpak app id "+installedId);
                            if(installed.ref.equals(item)&&!installed.remote.equals(catalogue.origin(origins.get(item))))
                                throw new IOException("Existing package has another origin: "+pkg+"; uninstall before changing origin");
                            if(!installed.ref.equals(item)&&DeployManifest.splitForPublic(installed.ref).equals(DeployManifest.splitForPublic(item)))
                                throw new IOException("Android split name collision between Flatpak refs "+installed.ref+" and "+item);
                        }
                    }
                    File scratch=new File(artifactsDir,"signing");Files.createDirectories(scratch.toPath());
                    boolean full=prior==null||!priorHasState||prior.getLongVersionCode()!=StubGenerator.STUB_VERSION_CODE;
                    int revision=PackSessionPlan.nextRevision(prior==null?0:prior.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,
                            prior==null?0:prior.baseRevisionCode,prior==null?null:prior.splitRevisionCodes);
                    List<String> desired=new ArrayList<>(),replacements=new ArrayList<>();
                    List<String> required=new ArrayList<>();Map<String,String> mounts=new TreeMap<>();
                    File primaryObjects=null;
                    for(int j=0;j<members.size();j++) {
                        cancellation.check();String item=members.get(j);
                        JSONObject manifest=new JSONObject(manifests.get(item).toString()).put("origin",(bundle?staging:catalogue).origin(origins.get(item)))
                                .put("mount",mount).put("dependencies",new JSONArray(dependencies.get(item))).put("deployments",rows);
                        // StoreExtraData expects the extra-data branch's objects at <artifacts>/objects.
                        File objects=new File(artifactsDir,item.equals(extraItem)?"objects":"objects-"+j);
                        // StagedPacks uses a private pack-repo in installation.getParentFile(); clear it per deployment.
                        File packRepo=new File(work,"pack-repo");if(packRepo.exists())WorkDirs.remove(packRepo);
                        StagedPacks.build(context,staging.installation,manifest,objects,cancellation,prior,pool);
                        String deploymentSplit=DeployManifest.splitForPublic(item);desired.add(deploymentSplit);replacements.add(deploymentSplit);
                        StubGenerator.generateDeploymentSplit(scratch,pkg,item,objects,new File(objects,"deployment.erofs"),
                                new File(artifactsDir,deploymentSplit+".apk"),revision);
                        if(item.equals(primary))primaryObjects=objects;
                        for(String dep:dependencies.get(item)) {
                            String depPkg=DeployManifest.packageForPublic(dep);
                            if(depPkg.equals(pkg))throw new IOException("Flatpak ref "+item+" depends on its own Android package via "+dep);
                            if(!required.contains(depPkg))required.add(depPkg);
                            mounts.put(dep,depPkg+">"+PackageMounts.mountPoint(dep,metadata.get(dep),metadata));
                        }
                    }
                    boolean launchable=members.contains(ref);String appId=primary.split("/")[1];JSONObject identity=manifests.get(primary);
                    LauncherData launcher=LauncherData.resolve(DeployManifest.stagedDeployment(staging.installation,identity),appId,
                            new File(DeployManifest.stagedDeployment(staging.installation,identity),"files/extra"),null);
                    byte[] icon=launcher.icon==null?fallbackIcon():LauncherData.insetIcon(launcher.icon);
                    File base=new File(artifactsDir,"base.apk");
                    StubGenerator.generatePackage(scratch,primary,origins.get(primary),launcher.label,icon,
                            StubGenerator.permissionsForMetadata(metadata.get(primary)),pkg,base,primaryObjects,required,mounts,launchable,mount,extraItem!=null);
                    String launchSplit=null;
                    if(launchable) {
                        launchSplit=StubGenerator.launcherSplitName(primary,launcher.label,launcher.mimeTypes,icon,launcher.symbolicIcon);desired.add(launchSplit);replacements.add(launchSplit);
                        StubGenerator.generateLauncher(scratch,pkg,primary,launcher.label,launcher.mimeTypes,icon,launcher.symbolicIcon,new File(artifactsDir,launchSplit+".apk"),revision);
                    }
                    List<String> previous=prior==null||prior.splitNames==null?Collections.emptyList():Arrays.asList(prior.splitNames);
                    List<String> retained=new ArrayList<>();
                    if(prior!=null)for(String split:previous)if(split.matches("[A-Za-z][A-Za-z0-9_]*(?:\\.[A-Za-z][A-Za-z0-9_]*)+")||split.matches("launcher_[0-9a-f]{64}"))retained.add(split);
                    for(String split:retained)if(!desired.contains(split))desired.add(split);
                    List<String> available=new ArrayList<>(replacements);
                    if(full&&prior!=null)for(String split:retained)if(!replacements.contains(split)) {
                        for(int j=0;j<prior.splitNames.length&&j<prior.applicationInfo.splitSourceDirs.length;j++)if(split.equals(prior.splitNames[j])) {
                            Files.copy(new File(prior.applicationInfo.splitSourceDirs[j]).toPath(),new File(artifactsDir,split+".apk").toPath());
                            available.add(split);break;
                        }
                    }
                    PackSessionPlan plan=PackSessionPlan.create(prior!=null,prior==null?0:prior.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,previous,desired,
                            available,full,replacements);
                    packages.add(new CommitChain.Package(pkg,required,plan.write.toArray(new String[0]),plan.remove.toArray(new String[0])));
                    artifacts.put(pkg,artifactsDir);
                    if(extraItem!=null) {
                        String extra=extraItem;File extraArtifacts=artifactsDir;
                        extraData.put(pkg,()->StoreExtraData.prepare(context,staging,extra,origins.get(extra),manifests.get(extra),
                                metadata.get(extra),extraArtifacts,revision,cancellation));
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
