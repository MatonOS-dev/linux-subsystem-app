package org.matonos.linux.stub;

import android.content.Context;
import org.matonos.linux.WorkDirs;
import android.util.Log;
import org.json.JSONObject;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/** Downloads and verifies an authorized Flatpak transaction in subsystem-private data.
 * Flatpak checks signatures while pulling; OSTree fsck checks content before packs
 * are produced. Extra-data execution remains in the generated package's own UID. */
public final class InstallTransaction {
    private static final String TAG = "MatonInstall";
    private static final java.util.regex.Pattern OPERATION_ID =
            java.util.regex.Pattern.compile("[A-Za-z0-9_-]{1,64}");
    private static final java.util.regex.Pattern INDEX_SPLIT =
            java.util.regex.Pattern.compile("index_[0-9a-f]{64}");
    // Flatpak/OSTree content is addressed by digest. A shared cache plus a
    // per-snapshot monitor lets the next job reuse completed object downloads.
    private static final Map<String,SnapshotLock> PULL_LOCKS=new java.util.HashMap<>();
    private static final class SnapshotLock { int users; }
    private static synchronized SnapshotLock retainPullLock(String key) {
        SnapshotLock lock=PULL_LOCKS.get(key);if(lock==null){lock=new SnapshotLock();PULL_LOCKS.put(key,lock);}lock.users++;return lock;
    }
    private static synchronized void releasePullLock(String key,SnapshotLock lock) {
        if(--lock.users==0)PULL_LOCKS.remove(key,lock);
    }

    /** Notified exactly once, on the worker thread, after the worker is done. */
    public interface Completion { void workerFinished(); }

    public interface Callback {
        void onProgress(String operationId, String phase, int percent, long bytesDone, long bytesTotal, String line);
        void onComplete(String operationId, String manifest);
        void onFailed(String operationId, String error);
    }

    private final Context context;
    private final Callback callback;
    private final String operationId;
    private final String appRef;
    private final String remote;
    private final AtomicBoolean cancelled = new AtomicBoolean();
    private final java.util.Set<Process> processes=java.util.concurrent.ConcurrentHashMap.newKeySet();
    private AutoCloseable cacheLease;
    private int lastReportedPercent = -1;
    private long lastReportedAt;
    private long plannedBytes;
    private boolean plannedBytesKnown=true;
    private final java.util.concurrent.atomic.AtomicLong completedBytes=new java.util.concurrent.atomic.AtomicLong();
    private final Object progressLock=new Object();
    private final Map<String,Integer> refProgress=new java.util.HashMap<>();
    private final Map<String,Long> refPartialBytes=new java.util.HashMap<>();
    private int downloadRefCount;
    private final java.util.concurrent.atomic.AtomicLong verifiedPayloadBytes=new java.util.concurrent.atomic.AtomicLong();
    private long plannedPayloadBytes;
    private boolean plannedPayloadBytesKnown=true;

    private InstallTransaction(Context context, Callback callback,
            String operationId, String appRef, String remote) {
        this.context = context;
        this.callback = callback;
        this.operationId = operationId;
        this.appRef = appRef;
        this.remote = remote;
    }

    /** Validates the requested transaction before any staging is touched. */
    public static InstallTransaction create(Context context, Callback callback,
            String operationId, String appRef, String remote) {
        if (operationId == null || !OPERATION_ID.matcher(operationId).matches())
            throw new IllegalArgumentException("Invalid operation ID");
        if (!DeployManifest.validRef(appRef))
            throw new IllegalArgumentException("Invalid application reference");
        try {org.matonos.linuxhost.stubgen.RemoteOrigin.parse(remote);}
        catch(java.io.IOException invalid){throw new IllegalArgumentException(invalid);}
        if (callback == null) throw new IllegalArgumentException("A completion callback is required");
        return new InstallTransaction(context, callback, operationId, appRef, remote);
    }

    String appRef() { return appRef; }

    String operation() { return operationId; }

    public void start(Completion completion) {
        Thread worker = new Thread(() -> {
            try { install(); }
            finally {
                AutoCloseable lease; synchronized(this){lease=cacheLease;cacheLease=null;}
                if(lease!=null)try{lease.close();}catch(Exception ignored){}
                completion.workerFinished();
            }
        }, "maton-install");
        worker.setDaemon(true);
        worker.start();
    }

    /** Stops this operation's worker. Descendants are reaped with the compositor. */
    public void cancel(String operation) {
        if (operationId.equals(operation)) cancel();
    }

    /** Stops this operation's worker regardless of how it is addressed. */
    public void cancel() {
        cancelled.set(true);
        for(Process child:processes)child.destroyForcibly();
    }

    private void install() {
        try {
            File operation=operationDirectory();
            InstalledPackages installed=new InstalledPackages(context);
            Scratch local=new Scratch(operation,remote,false);
            java.util.Map<String,Scratch> probes=new java.util.LinkedHashMap<>();
            java.util.LinkedHashSet<String> known=new java.util.LinkedHashSet<>();known.add(remote);known.add("flathub");known.addAll(installed.origins());
            org.matonos.linuxhost.stubgen.DependencyPlan planner=new org.matonos.linuxhost.stubgen.DependencyPlan(remote,installed.origins(),
                    new org.matonos.linuxhost.stubgen.DependencyPlan.Backend() {
                public org.matonos.linuxhost.stubgen.DeploymentRecord installed(String ref)throws Exception{return installed.installed(ref);}
                public String recordedOrigin(String ref)throws Exception{return installed.recordedOrigin(ref);}
                public org.matonos.linuxhost.stubgen.DependencyPlan.Info recordedInfo(org.matonos.linuxhost.stubgen.DeploymentRecord record)throws Exception{return local.requirements(record);}
                private Scratch probe(String origin)throws Exception {
                    checkCancelled();Scratch scratch=probes.get(origin);
                    if(scratch==null){scratch=new Scratch(new File(operation,"probe-"+probes.size()),origin);probes.put(origin,scratch);}
                    return scratch;
                }
                public java.util.List<String> related(String ref,String origin,org.matonos.linuxhost.stubgen.DependencyPlan.Info info)throws Exception {
                    if(!info.mayHaveRelated)return java.util.Collections.emptyList();
                    java.util.Set<String> related=new java.util.LinkedHashSet<>();
                    for(String candidate:known)related.addAll(probe(candidate).related(ref,info.metadata));
                    return new ArrayList<>(related);
                }
                public String choose(String ref,java.util.List<String> origins)throws Exception {
                    return org.matonos.linux.ProviderChoice.choose(context,ref,origins,()->cancelled.get());
                }
                public org.matonos.linuxhost.stubgen.DependencyPlan.Info inspect(String ref,String origin)throws Exception {
                    return probe(origin).inspect(ref);
                }
            });
            report("planning",5,-1,-1,"Checking installed packages and Flatpak metadata");
            java.util.List<org.matonos.linuxhost.stubgen.DependencyPlan.Node> plan=planner.build(appRef,remote,new File(operation,"repair").exists());
            java.util.Map<String,String> metadata=new java.util.HashMap<>();
            for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan)metadata.put(node.ref,new String(node.info.metadata,StandardCharsets.UTF_8));
            java.util.Map<String,String> installedMetadata=new java.util.HashMap<>();
            for(org.matonos.linuxhost.stubgen.DeploymentRecord record:installed.records.values())
                installedMetadata.put(record.ref,new String(record.metadata,StandardCharsets.UTF_8));
            metadata=PackageMounts.metadataWithInstalled(metadata,installedMetadata);
            java.util.Map<String,org.matonos.linuxhost.stubgen.DependencyPlan.Node> nodesByRef=new java.util.HashMap<>();
            for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan)nodesByRef.put(node.ref,node);
            // Only the app's runtime closure is mounted into bwrap. ExtensionOf
            // parents remain staged for Flatpak's apply_extra metadata, but
            // receive a private staging target and never claim an app target.
            java.util.Set<String> launchRefs=new java.util.LinkedHashSet<>();
            java.util.ArrayDeque<String> pendingLaunch=new java.util.ArrayDeque<>();pendingLaunch.add(appRef);
            while(!pendingLaunch.isEmpty()) {
                String ref=pendingLaunch.removeFirst();if(!launchRefs.add(ref))continue;
                org.matonos.linuxhost.stubgen.DependencyPlan.Node node=nodesByRef.get(ref);
                if(node==null)throw new java.io.IOException("Launch dependency is missing from the package plan: "+ref);
                pendingLaunch.addAll(node.dependencies);
            }
            java.util.Map<String,String> mounts=new java.util.HashMap<>();
            java.util.Map<String,String> plannedTargets=new java.util.LinkedHashMap<>();
            for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan) {
                String mount;
                if(!launchRefs.contains(node.ref))mount=PackageMounts.stagingTarget(node.ref);
                else if(node.ref.equals(appRef)) {
                    org.matonos.linuxhost.stubgen.DeploymentRecord previous=installed.records.get(node.ref);
                    mount=previous!=null?previous.mount:(appRef.startsWith("app/")?"/app":"/usr");
                } else mount=PackageMounts.mountPoint(node.ref,metadata.get(node.ref),metadata);
                mounts.put(node.ref,mount);
                plannedTargets.put(node.ref,launchRefs.contains(node.ref)
                        ?PackageMounts.launchTarget(node.ref,mount):mount);
            }
            PackageMounts.validateUniqueTargets(plannedTargets);
            String appCommit=null;
            plannedBytes=0;plannedBytesKnown=true;
            for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan)if(!node.reused) {
                synchronized(progressLock){refProgress.put(node.ref,0);downloadRefCount++;}
                long size=downloadSize(node.info.metadata);if(size<0)plannedBytesKnown=false;else plannedBytes+=size;
                long payload=metadataSize(node.info.metadata,"xa.installed-size");if(payload<0)plannedPayloadBytesKnown=false;else plannedPayloadBytes+=payload;
            }
            // Provider choices finish before pulls. Distinct per-ref roots let
            // independent runtimes/extensions pull concurrently without sharing
            // mutable Flatpak installation state.
            org.matonos.linuxhost.stubgen.PlanExecutor.Outcome<Void> pulled=
                    org.matonos.linuxhost.stubgen.PlanExecutor.execute(plan,
                            Runtime.getRuntime().availableProcessors(),node->{
                checkCancelled();if(node.reused)return null;
                int index=plan.indexOf(node);String location=node.ref.equals(appRef)?"":"package-"+index;
                Scratch scratch=new Scratch(location.isEmpty()?operation:new File(operation,location),node.origin);
                scratch.seed(installed,node.ref);
                String pullKey=node.origin+"\n"+node.ref+"\n"+node.info.commit;
                SnapshotLock pullLock=retainPullLock(pullKey);
                try { synchronized(pullLock) { checkCancelled();scratch.pull(node); } }
                finally { releasePullLock(pullKey,pullLock); }
                return null;
            });
            if(!pulled.completed.containsKey(appRef)) {
                Throwable error=pulled.failed.get(appRef);
                if(error==null)for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan)
                    if(pulled.failed.containsKey(node.ref)){error=pulled.failed.get(node.ref);break;}
                if(error instanceof Exception)throw (Exception)error;
                throw new java.io.IOException("Requested ref could not be staged because a dependency failed",error);
            }
            org.json.JSONArray rows=InstallHandoff.completedRows(plan,pulled.completed.keySet(),metadata,mounts,appRef);
            for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan)
                if(node.ref.equals(appRef)&&pulled.completed.containsKey(node.ref))appCommit=node.info.commit;
            if(appCommit==null)throw new java.io.IOException("Plan has no requested ref");
            long verified=plannedPayloadBytesKnown?verifiedPayloadBytes.get():-1;
            long verifyTotal=plannedPayloadBytesKnown?plannedPayloadBytes:-1;
            report("verify",82,verified,verifyTotal,"Package verification complete");
            report("staging",82,-1,-1,"Packages downloaded; preparing install packs");
            callback.onComplete(operationId,new JSONObject().put("version",1).put("appRef",appRef).put("appCommit",appCommit)
                    .put("origin",remote).put("deployments",rows).toString());
        }catch(Throwable error) {
            Log.e(TAG,"Install worker failed for "+appRef,error);
            fail(error.getMessage()==null?error.toString():error.getMessage());
        }
    }

    private void checkCancelled()throws java.io.IOException {if(cancelled.get())throw new java.io.IOException("Install cancelled");}

    /** One origin per scratch, for both read-only discovery and single-ref pulls. */
    private final class Scratch {
        final File root,installation,home,cache,tmp,run;
        final org.matonos.linuxhost.stubgen.RemoteOrigin origin;
        final java.util.List<String> prefix=new ArrayList<>();
        Scratch(File root,String encoded)throws Exception {this(root,encoded,true);}
        Scratch(File root,String encoded,boolean configure)throws Exception {
            this.root=root;origin=org.matonos.linuxhost.stubgen.RemoteOrigin.parse(encoded);
            for(String name:new String[]{"installation","home","cache","tmp","run"}) {
                File dir=new File(root,name);if(!dir.isDirectory()&&!dir.mkdirs())throw new java.io.IOException("Cannot create private scratch");
            }
            markStoreRoot(new File(root,"installation"));
            installation=requireDirectory(root,"installation");home=requireDirectory(root,"home");
            File sharedCache=new File(WorkDirs.root(context),"flatpak-cache");
            synchronized(InstallTransaction.this) { if(cacheLease==null)cacheLease=WorkDirs.keep(sharedCache); }
            Files.createDirectories(sharedCache.toPath());
            cache=sharedCache.getCanonicalFile();tmp=requireDirectory(root,"tmp");run=requireDirectory(root,"run");
            String dns=LoaderCommand.resolverAddress();File resolv=new File(run,"install-resolv.conf");LoaderCommand.writeResolv(resolv,dns);
            prefix.add(LoaderCommand.loader(context).getAbsolutePath());prefix.add("--parent-pid");prefix.add(Integer.toString(android.os.Process.myPid()));
            LoaderCommand.addDns(prefix,dns);LoaderCommand.addResolvMaps(prefix,resolv);
            File descriptor=new File(root,"origin.flatpakrepo");
            if(!origin.url.isEmpty())Files.write(descriptor.toPath(),origin.flatpakrepo().getBytes(StandardCharsets.UTF_8));
            if(configure) {
                if(origin.url.isEmpty()) {
                    try(java.io.InputStream in=context.getAssets().open("flatpak/remotes.d/flathub.flatpakrepo")) {
                        Files.copy(in,descriptor.toPath(),java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                    }
                }
                cli("remote-add","--system",origin.name,descriptor.getAbsolutePath());
            }
            String tags=android.os.LocaleList.getDefault().toLanguageTags();
            java.util.List<String> localeTags=new ArrayList<>();
            if(tags!=null&&!tags.isEmpty())java.util.Collections.addAll(localeTags,tags.split(","));
            cli("config","--system","--set","languages",FlatpakLanguages.configuration(localeTags));
        }
        byte[] command(java.util.List<String> tail,boolean absentAllowed)throws Exception {
            checkCancelled();java.util.List<String> command=new ArrayList<>(prefix);command.addAll(tail);
            File error=File.createTempFile("flatpak-",".log",tmp);
            ProcessBuilder builder=new ProcessBuilder(command).directory(home).redirectError(error);
            Map<String,String> env=builder.environment();env.clear();
            String nativeDir=LoaderCommand.nativeLibraryDir(context).getAbsolutePath();
            env.put("PATH",nativeDir+":/system/bin");env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");env.put("LC_ALL","C");
            env.put("HOME",home.getAbsolutePath());env.put("TMPDIR",tmp.getAbsolutePath());env.put("XDG_RUNTIME_DIR",run.getAbsolutePath());
            env.put("XDG_DATA_HOME",new File(home,".local/share").getAbsolutePath());env.put("XDG_CONFIG_HOME",new File(home,".config").getAbsolutePath());env.put("XDG_CACHE_HOME",cache.getAbsolutePath());
            env.put("XDG_DATA_HOME",new File(home,".local/share").getAbsolutePath());env.put("XDG_CONFIG_HOME",new File(home,".config").getAbsolutePath());env.put("XDG_CACHE_HOME",cache.getAbsolutePath());
            env.put("FLATPAK_SYSTEM_DIR",installation.getAbsolutePath());env.put("FLATPAK_USER_DIR",installation.getAbsolutePath());env.put("FLATPAK_SYSTEM_CACHE_DIR",cache.getAbsolutePath());env.put("FLATPAK_DOWNLOAD_TMPDIR",tmp.getAbsolutePath());
            for(String key:new String[]{"SSL_CERT_DIR","CURL_CA_BUNDLE","G_TLS_CA_PATH"})env.put(key,"/apex/com.android.conscrypt/cacerts");
            Process child=builder.start();processes.add(child);if(cancelled.get())child.destroyForcibly();
            try {
                byte[] output;
                try(java.io.InputStream in=child.getInputStream()){output=FlatpakOutput.readAll(in);}
                int status=child.waitFor();checkCancelled();
                if(status==3&&absentAllowed)return null;
                if(status!=0)throw new java.io.IOException("Flatpak operation failed: "+new String(Files.readAllBytes(error.toPath()),StandardCharsets.UTF_8));
                return output;
            }finally{processes.remove(child);child.destroyForcibly();error.delete();}
        }
        byte[] cli(String... args)throws Exception {
            java.util.List<String> tail=new ArrayList<>();tail.add(LoaderCommand.tool(context,LoaderCommand.FLATPAK_CLI).getAbsolutePath());java.util.Collections.addAll(tail,args);
            return command(tail,false);
        }
        org.matonos.linuxhost.stubgen.DependencyPlan.Info inspect(String ref)throws Exception {
            String helper=new File(context.getApplicationInfo().nativeLibraryDir,"libmatonos-flatpak-plan.so").getAbsolutePath();
            byte[] output=command(java.util.Arrays.asList(helper,installation.getAbsolutePath(),origin.name,ref),true);
            if(output==null)return null;
            try(java.io.DataInputStream in=new java.io.DataInputStream(new java.io.ByteArrayInputStream(output))) {
                String commit=field(in);byte[] metadata=blob(in);String runtime=field(in),parent=field(in);
                int extensions=in.readInt();if(extensions<0||extensions>1)throw new java.io.IOException("Invalid extension flag");
                int count=in.readInt();if(count<0)throw new java.io.IOException("Invalid related-ref count");
                java.util.List<String> related=new ArrayList<>();for(int i=0;i<count;i++)related.add(field(in));
                if(in.read()!=-1)throw new java.io.IOException("Trailing Flatpak plan bytes");
                return new org.matonos.linuxhost.stubgen.DependencyPlan.Info(commit,metadata,runtime,parent,related,extensions==1);
            }
        }
        org.matonos.linuxhost.stubgen.DependencyPlan.Info requirements(org.matonos.linuxhost.stubgen.DeploymentRecord record)throws Exception {
            File file=File.createTempFile("requirements-",".ini",tmp);Files.write(file.toPath(),record.metadata);
            try {
                String helper=new File(context.getApplicationInfo().nativeLibraryDir,"libmatonos-flatpak-plan.so").getAbsolutePath();
                byte[] bytes=command(java.util.Arrays.asList(helper,"--requirements",file.getAbsolutePath(),record.ref),false);
                try(java.io.DataInputStream in=new java.io.DataInputStream(new java.io.ByteArrayInputStream(bytes))) {
                    String runtime=field(in),parent=field(in);int related=in.readInt();if(related<0||related>1||in.read()!=-1)throw new java.io.IOException("Invalid Flatpak requirements");
                    return new org.matonos.linuxhost.stubgen.DependencyPlan.Info(record.commit,record.metadata,runtime,parent,java.util.Collections.emptyList(),related==1);
                }
            }finally{file.delete();}
        }
        java.util.List<String> related(String ref,byte[] metadata)throws Exception {
            File file=File.createTempFile("related-",".ini",tmp);Files.write(file.toPath(),metadata);
            try {
                String helper=new File(context.getApplicationInfo().nativeLibraryDir,"libmatonos-flatpak-plan.so").getAbsolutePath();
                byte[] bytes=command(java.util.Arrays.asList(helper,installation.getAbsolutePath(),origin.name,ref,file.getAbsolutePath()),false);
                try(java.io.DataInputStream in=new java.io.DataInputStream(new java.io.ByteArrayInputStream(bytes))) {
                    int count=in.readInt();if(count<0)throw new java.io.IOException("Invalid related-ref count");
                    java.util.List<String> refs=new ArrayList<>();for(int i=0;i<count;i++)refs.add(field(in));
                    if(in.read()!=-1)throw new java.io.IOException("Trailing related-ref data");return refs;
                }
            }finally{file.delete();}
        }
        final java.util.Set<String> deltaBases=new java.util.HashSet<>();
        void seed(InstalledPackages installed,String ref)throws Exception {
            if(installed.installed(ref)==null)return;
            android.content.pm.PackageInfo pkg=context.getPackageManager().getPackageInfo(
                    DeployManifest.packageForPublic(ref),0);
            android.os.IBinder imageLease=new android.os.Binder();
            File imageRoot=new File(ErofsLaunch.mountInstalledImage(pkg.splitNames,pkg.applicationInfo.splitSourceDirs,imageLease));
            String imagePrefix=ref.startsWith("runtime/")?ref.substring(ref.lastIndexOf('/')+1):"";
            File currentIndex=File.createTempFile("installed-image-",".index",root);
            java.util.List<org.matonos.linuxhost.stubgen.ObjectPacks.Entry> entries;
            try {
                Files.write(currentIndex.toPath(),org.matonos.linuxhost.stubgen.PackLaunch.installedIndexAsset(
                        pkg.splitNames,pkg.applicationInfo.splitSourceDirs,"objects.index"));
                entries=org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(currentIndex);
            } finally {currentIndex.delete();}
            long required=org.matonos.linuxhost.stubgen.PackSeed.bytes(entries,
                    new android.os.StatFs(root.getAbsolutePath()).getBlockSizeLong()),available=root.getUsableSpace();
            report("staging",-1,-1,-1,"Update base temporary bytes: "+required+"; available: "+available);
            if(required>available)throw new java.io.IOException("Insufficient space for installed update base: "+required+" bytes required, "+available+" available");
            try {
                org.matonos.linuxhost.stubgen.PackSeed.seedFromImage(entries,imageRoot,imagePrefix,new File(installation,"repo"),()->cancelled.get());
                org.matonos.linuxhost.stubgen.DeploymentRecord record=org.matonos.linuxhost.stubgen.DeploymentRecord.installed(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);
                File originalIndex=File.createTempFile("installed-original-",".index",root);
                java.util.List<org.matonos.linuxhost.stubgen.ObjectPacks.Entry> originalEntries;
                try {
                    Files.write(originalIndex.toPath(),org.matonos.linuxhost.stubgen.PackLaunch.installedIndexAsset(
                            pkg.splitNames,pkg.applicationInfo.splitSourceDirs,"original.index"));
                    originalEntries=org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(originalIndex);
                } finally {originalIndex.delete();}
                long originalBytes=org.matonos.linuxhost.stubgen.PackSeed.bytes(originalEntries,new android.os.StatFs(root.getAbsolutePath()).getBlockSizeLong());
                report("staging",-1,-1,-1,"Original update base payload budget: "+originalBytes);
                if(originalBytes>root.getUsableSpace())throw new java.io.IOException("Insufficient space for original update base");
                org.matonos.linuxhost.stubgen.PackSeed.seedFromImage(originalEntries,imageRoot,imagePrefix,"/files",
                        new File(installation,"repo"),()->cancelled.get());
                imageLease.pingBinder(); // Hold the mounted-image lease through both seed passes.
                for(int i=0;i<pkg.splitNames.length;i++)if(INDEX_SPLIT.matcher(pkg.splitNames[i]).matches()) {
                    try(java.util.zip.ZipFile zip=new java.util.zip.ZipFile(pkg.applicationInfo.splitSourceDirs[i])) {
                        java.util.zip.ZipEntry metadata=zip.getEntry("assets/matonos/original.ostree");
                        if(metadata==null)throw new java.io.IOException("Installed index lacks original metadata");
                        org.matonos.linuxhost.stubgen.OstreePacks.restoreOriginal(zip.getInputStream(metadata),new File(installation,"repo"),record.commit,record.commitObject,
                                StagedPacks.command(context,root,new File(installation,"repo"),()->cancelled.get()));
                        deltaBases.add(ref);
                    }
                }
            } catch(Exception error) {
                if(cancelled.get())throw error;
                report("staging",-1,-1,-1,"Static delta base unavailable; using per-object pull: "+error.getMessage());
            }

        }
        void pull(org.matonos.linuxhost.stubgen.DependencyPlan.Node node)throws Exception {
            long size=downloadSize(node.info.metadata), total=plannedBytes;
            if(total<=0||!plannedBytesKnown)total=-1;
            report("download",5,-1,-1,"Downloading "+node.ref+" from "+origin.display());
            long pullStarted=System.nanoTime();
            InstallProgress parser=new InstallProgress();
                boolean isolatedDeploy=hasExtraData(node.info.metadata);
                java.util.List<String> install=new ArrayList<>(java.util.Arrays.asList("install","--system","--assumeyes","--no-deps","--no-related",deltaBases.contains(node.ref)?"--ostree-verbose":"--no-static-deltas"));
                if(isolatedDeploy)install.add("--no-deploy");
                // Explicit installs get every subpath; pass the ones Flatpak chose (Locale: device languages).
                for(String subpath:node.subpaths)install.add("--subpath="+subpath);
                install.add(origin.name);install.add(node.ref);
                cliProgress(node.ref,parser,total,size,install.toArray(new String[0]));
            if(!isolatedDeploy) {
                JSONObject manifest=DeployManifest.build(installation,node.ref,origin.name,operationId);
                if(!node.info.commit.equals(manifest.getString("appCommit"))) {
                    // install has no --commit option; update can bind the downloaded
                    // deployment to the exact snapshot inspected before any pull.
                    cliProgress(node.ref,parser,total,size,"update","--system","--assumeyes","--no-deps","--no-related",deltaBases.contains(node.ref)?"--ostree-verbose":"--no-static-deltas","--commit="+node.info.commit,node.ref);
                    manifest=DeployManifest.build(installation,node.ref,origin.name,operationId);
                }
            }
            if(isolatedDeploy) {
                org.json.JSONArray paths=new org.json.JSONArray();for(String path:node.subpaths)paths.put(path);
                File deployment=DeployManifest.stagedDeploymentPath(installation,
                        new JSONObject().put("appRef",node.ref).put("appCommit",node.info.commit).put("subpaths",paths));
                buildExtraDataImage(node,installation);
                // --no-deploy leaves the verified commit in repo/ without a
                // Flatpak deployment record. Materialize that exact commit for
                // the normal manifest and pack pipeline after the stub has
                // returned its separately validated extra-data image.
                File parent=deployment.getParentFile();
                if(!parent.isDirectory()&&!parent.mkdirs())throw new java.io.IOException("Cannot create verified deployment parent");
                if(deployment.exists())throw new java.io.IOException("Verified deployment path already exists");
                java.util.List<String> checkout=new ArrayList<>();checkout.add("checkout");checkout.add("--user-mode");
                for(String subpath:node.subpaths)checkout.add("--subpath="+subpath);
                checkout.add(node.info.commit);checkout.add(deployment.getAbsolutePath());
                StagedPacks.command(context,root,new File(installation,"repo"),()->cancelled.get())
                        .run(checkout.toArray(new String[0]));
                DeployManifest.recordVerifiedPull(installation,node.ref,origin.name,node.info.commit,
                        node.subpaths,node.info.metadata);
            }
            JSONObject manifest=DeployManifest.build(installation,node.ref,origin.name,operationId);
            Log.i(TAG,"phase=pull ref="+node.ref+" ms="+
                    java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(System.nanoTime()-pullStarted)+" bytes="+size);
            DeployManifest.verifyPlanned(installation,manifest,node.ref,node.info.commit,node.info.metadata,node.subpaths);
            StagedPacks.verify(context,installation,()->cancelled.get());
            if(isolatedDeploy)cleanupExtraDataInstall(DeployManifest.packageForPublic(node.ref));
            long payload=metadataSize(node.info.metadata,"xa.installed-size");if(payload>0)verifiedPayloadBytes.addAndGet(payload);
        }
        private boolean hasExtraData(byte[] metadata) {
            String text=new String(metadata,StandardCharsets.UTF_8);
            return java.util.regex.Pattern.compile("(?m)^\\s*\\[Extra Data\\]\\s*$").matcher(text).find();
        }
        private void buildExtraDataImage(org.matonos.linuxhost.stubgen.DependencyPlan.Node node,File installation)throws Exception {
            String packageName=DeployManifest.packageForPublic(node.ref);
            File apk=new File(installation,"extra-data-stub.apk");
            File work=new File(root,"extra-stub-work");if(!work.isDirectory()&&!work.mkdirs())throw new java.io.IOException("Cannot create temporary stub work directory");
            boolean launchable=node.ref.startsWith("app/")&&PackageMounts.extensionParent(new String(node.info.metadata,StandardCharsets.UTF_8))==null;
            java.util.List<String> requiredPackages=new ArrayList<>();
            java.util.Map<String,String> dependencies=new java.util.TreeMap<>();
            for(String dependency:node.dependencies) {
                String dependencyPackage=DeployManifest.packageForPublic(dependency);
                requiredPackages.add(dependencyPackage);
                dependencies.put(dependency,dependencyPackage+">/usr");
            }
            org.matonos.linuxhost.stubgen.StubGenerator.generatePackage(work,node.ref,origin.name,node.ref,null,
                    org.matonos.linuxhost.stubgen.StubGenerator.permissionsForMetadata(new String(node.info.metadata,StandardCharsets.UTF_8)),packageName,apk,null,requiredPackages,
                    dependencies,launchable,launchable?"/app":"/usr",true);
            if(!hasExtraDataService(packageName))installTemporaryBase(packageName,apk);
                android.os.ParcelFileDescriptor[] pipe=android.os.ParcelFileDescriptor.createPipe();
                File image=new File(installation,"extra-data-image.erofs");
                try(android.os.ParcelFileDescriptor read=pipe[0];android.os.ParcelFileDescriptor write=pipe[1]) {
                    android.content.Intent request=new android.content.Intent("org.matonos.linuxhost.EXTRA_DATA_INSTALL")
                            .setComponent(new android.content.ComponentName(packageName,"org.matonos.linux.stub.ExtraDataInstallService"));
                    java.util.concurrent.CompletableFuture<android.os.IBinder> bound=new java.util.concurrent.CompletableFuture<>();
                    android.content.ServiceConnection connection=new android.content.ServiceConnection() {
                        @Override public void onServiceConnected(android.content.ComponentName name,android.os.IBinder binder){bound.complete(binder);}
                        @Override public void onServiceDisconnected(android.content.ComponentName name){}
                        @Override public void onNullBinding(android.content.ComponentName name){bound.completeExceptionally(new java.io.IOException("Stub extra-data service returned no binder"));}
                    };
                    if(!context.bindService(request,connection,Context.BIND_AUTO_CREATE|Context.BIND_IMPORTANT))
                        throw new java.io.IOException("Cannot bind stub extra-data service");
                    try {
                    android.os.Parcel data=android.os.Parcel.obtain();
                    try {
                        data.writeFileDescriptor(write.getFileDescriptor());
                        data.writeString(node.ref);data.writeString(origin.name);
                        data.writeString(origin.url.isEmpty()?"https://dl.flathub.org/repo/flathub.flatpakrepo":origin.url);
                        data.writeString(String.join("\n",node.subpaths));
                        data.writeString(node.info.runtime.isEmpty()||node.info.runtime.startsWith("runtime/")?node.info.runtime:"runtime/"+node.info.runtime);
                        if(!bound.get(2,java.util.concurrent.TimeUnit.MINUTES).transact(1,data,null,0))
                            throw new java.io.IOException("Stub extra-data service rejected the request");
                    } finally {data.recycle();}
                    write.close();
                    try(java.io.DataInputStream in=new java.io.DataInputStream(new android.os.ParcelFileDescriptor.AutoCloseInputStream(read));
                            java.io.OutputStream out=Files.newOutputStream(image.toPath())) {
                        int status=in.readInt();
                        if(status!=0)throw new java.io.IOException("Stub Flatpak deploy failed: "+in.readUTF());
                        long length=in.readLong();if(length<=0)throw new java.io.IOException("Invalid stub EROFS image length");
                        byte[] buffer=new byte[65536];long left=length;while(left>0){int count=in.read(buffer,0,(int)Math.min(buffer.length,left));if(count<0)throw new java.io.EOFException("Truncated stub EROFS image");out.write(buffer,0,count);left-=count;}
                    }
                    } finally {context.unbindService(connection);}
                }
                Log.i(TAG,"Stub returned extra-data EROFS for "+node.ref+" bytes="+image.length());
        }
        private boolean hasExtraDataService(String packageName) {
            try {
                context.getPackageManager().getServiceInfo(new android.content.ComponentName(packageName,
                        "org.matonos.linux.stub.ExtraDataInstallService"),0);
                return true;
            } catch(android.content.pm.PackageManager.NameNotFoundException absent) { return false; }
        }
        private void cleanupExtraDataInstall(String packageName)throws Exception {
            android.content.Intent request=new android.content.Intent("org.matonos.linuxhost.EXTRA_DATA_INSTALL")
                    .setComponent(new android.content.ComponentName(packageName,"org.matonos.linux.stub.ExtraDataInstallService"));
            java.util.concurrent.CompletableFuture<android.os.IBinder> bound=new java.util.concurrent.CompletableFuture<>();
            android.content.ServiceConnection connection=new android.content.ServiceConnection() {
                @Override public void onServiceConnected(android.content.ComponentName name,android.os.IBinder binder){bound.complete(binder);}
                @Override public void onServiceDisconnected(android.content.ComponentName name){}
                @Override public void onNullBinding(android.content.ComponentName name){bound.completeExceptionally(new java.io.IOException("Stub extra-data service returned no binder"));}
            };
            if(!context.bindService(request,connection,Context.BIND_AUTO_CREATE|Context.BIND_IMPORTANT))
                throw new java.io.IOException("Cannot bind stub extra-data service for cleanup");
            try {
                android.os.IBinder binder=bound.get(2,java.util.concurrent.TimeUnit.MINUTES);
                android.os.Parcel data=android.os.Parcel.obtain();
                try {
                    if(!binder.transact(2,data,null,0))throw new java.io.IOException("Stub extra-data service rejected install cleanup");
                } finally {data.recycle();}
            } finally {context.unbindService(connection);}
        }
        private void installTemporaryBase(String packageName,File apk)throws Exception {
            String action=context.getPackageName()+".EXTRA_STUB."+java.util.UUID.randomUUID();
            java.util.concurrent.CountDownLatch done=new java.util.concurrent.CountDownLatch(1);
            java.util.concurrent.atomic.AtomicReference<android.content.Intent> response=new java.util.concurrent.atomic.AtomicReference<>();
            android.content.BroadcastReceiver receiver=new android.content.BroadcastReceiver(){
                @Override public void onReceive(Context context,android.content.Intent intent){response.set(intent);done.countDown();}
            };
            if(android.os.Build.VERSION.SDK_INT>=33)context.registerReceiver(receiver,new android.content.IntentFilter(action),Context.RECEIVER_NOT_EXPORTED);
            else context.registerReceiver(receiver,new android.content.IntentFilter(action));
            try {
                android.content.pm.PackageInstaller.SessionParams params=new android.content.pm.PackageInstaller.SessionParams(android.content.pm.PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                params.setAppPackageName(packageName);
                params.setRequireUserAction(android.content.pm.PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
                android.content.pm.PackageInstaller installer=context.getPackageManager().getPackageInstaller();
                int id=installer.createSession(params);
                try(android.content.pm.PackageInstaller.Session session=installer.openSession(id)) {
                    try(java.io.InputStream in=org.matonos.linuxhost.stubgen.PlaceholderFiles.open(apk);java.io.OutputStream out=session.openWrite("base.apk",0,apk.length())) {
                        byte[] buffer=new byte[65536];int count;while((count=in.read(buffer))!=-1)out.write(buffer,0,count);session.fsync(out);
                    }
                    android.content.Intent result=new android.content.Intent(action).setPackage(context.getPackageName());
                    android.app.PendingIntent pending=android.app.PendingIntent.getBroadcast(context,id,result,android.app.PendingIntent.FLAG_UPDATE_CURRENT|android.app.PendingIntent.FLAG_MUTABLE);
                    session.commit(pending.getIntentSender());
                }
                if(!done.await(10,java.util.concurrent.TimeUnit.MINUTES))throw new java.io.IOException("Temporary stub install timed out");
                android.content.Intent result=response.get();
                if(result==null||result.getIntExtra(android.content.pm.PackageInstaller.EXTRA_STATUS,android.content.pm.PackageInstaller.STATUS_FAILURE)!=android.content.pm.PackageInstaller.STATUS_SUCCESS)
                    throw new java.io.IOException("Temporary stub install failed: "+(result==null?"no result":result.getStringExtra(android.content.pm.PackageInstaller.EXTRA_STATUS_MESSAGE)));
            } finally {try{context.unregisterReceiver(receiver);}catch(Exception ignored){}}
        }
        private void cliProgress(String ref,InstallProgress parser,long total,long packageBytes,String... args)throws Exception {
            java.util.List<String> tail=new ArrayList<>();tail.add(LoaderCommand.tool(context,LoaderCommand.FLATPAK_CLI).getAbsolutePath());java.util.Collections.addAll(tail,args);
            commandProgress(ref,tail,parser,total,packageBytes);
        }
        private void commandProgress(String ref,java.util.List<String> tail,InstallProgress parser,long total,long packageBytes)throws Exception {
            checkCancelled();java.util.List<String> command=new ArrayList<>(prefix);command.addAll(tail);
            File error=File.createTempFile("flatpak-",".log",tmp);
            ProcessBuilder builder=new ProcessBuilder(command).directory(home).redirectError(error);
            Map<String,String> env=builder.environment();env.clear();String nativeDir=LoaderCommand.nativeLibraryDir(context).getAbsolutePath();
            env.put("PATH",nativeDir+":/system/bin");env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");env.put("LC_ALL","C");
            env.put("HOME",home.getAbsolutePath());env.put("TMPDIR",tmp.getAbsolutePath());env.put("XDG_RUNTIME_DIR",run.getAbsolutePath());
            env.put("FLATPAK_SYSTEM_DIR",installation.getAbsolutePath());env.put("FLATPAK_USER_DIR",installation.getAbsolutePath());
            env.put("FLATPAK_SYSTEM_CACHE_DIR",cache.getAbsolutePath());env.put("FLATPAK_DOWNLOAD_TMPDIR",tmp.getAbsolutePath());
            for(String key:new String[]{"SSL_CERT_DIR","CURL_CA_BUNDLE","G_TLS_CA_PATH"})env.put(key,"/apex/com.android.conscrypt/cacerts");
            Process child=builder.start();processes.add(child);if(cancelled.get())child.destroyForcibly();
            try(java.io.BufferedReader reader=new java.io.BufferedReader(new InputStreamReader(child.getInputStream(),StandardCharsets.UTF_8))) {
                String line;long lastSent=0;int lastPercent=-1;
                while((line=reader.readLine())!=null) {checkCancelled();int p=InstallProgress.parsePercent(line);if(p>=0&&(p!=lastPercent||System.currentTimeMillis()-lastSent>=250)) {
                    int phasePercent=updateProgress(ref,p,packageBytes,total);
                    long done=total>0?aggregateBytes():-1;
                    report("download",5+(phasePercent*70/100),done,total,line);lastPercent=p;lastSent=System.currentTimeMillis();}}
                int status=child.waitFor();checkCancelled();if(status!=0)throw new java.io.IOException("Flatpak operation failed: "+readErrorLog(error));
                completeProgress(ref,packageBytes);
            } finally {processes.remove(child);child.destroyForcibly();error.delete();}
        }
    }
    private int updateProgress(String ref,int percent,long packageBytes,long total) {
        synchronized(progressLock) {
            refProgress.put(ref,percent);
            if(total>0)refPartialBytes.put(ref,Math.max(1,packageBytes)*percent/100);
            if(total>0)return (int)Math.min(100,aggregateBytesLocked()*100/total);
            long sum=0;for(int value:refProgress.values())sum+=value;
            return (int)(sum/Math.max(1,downloadRefCount));
        }
    }
    private void completeProgress(String ref,long packageBytes) {
        synchronized(progressLock) {
            completedBytes.addAndGet(Math.max(1,packageBytes));
            refPartialBytes.remove(ref);refProgress.put(ref,100);
        }
    }
    private long aggregateBytes() { synchronized(progressLock){return aggregateBytesLocked();} }
    private long aggregateBytesLocked() {
        long value=completedBytes.get();for(long partial:refPartialBytes.values())value+=partial;return value;
    }
    /** Count allocated blocks once per inode, including hardlinked checkouts. */
    /** Measurement only: parallel package builds remove staging files while this walks, so entries that
     *  vanish mid-walk are skipped instead of failing the install. */
    private static void walkPresent(File root,java.util.function.Consumer<java.nio.file.Path> visit)throws Exception {
        Files.walkFileTree(root.toPath(),new java.nio.file.SimpleFileVisitor<java.nio.file.Path>() {
            @Override public java.nio.file.FileVisitResult preVisitDirectory(java.nio.file.Path dir,java.nio.file.attribute.BasicFileAttributes a){visit.accept(dir);return java.nio.file.FileVisitResult.CONTINUE;}
            @Override public java.nio.file.FileVisitResult visitFile(java.nio.file.Path file,java.nio.file.attribute.BasicFileAttributes a){visit.accept(file);return java.nio.file.FileVisitResult.CONTINUE;}
            @Override public java.nio.file.FileVisitResult visitFileFailed(java.nio.file.Path file,java.io.IOException e)throws java.io.IOException{
                if(e instanceof java.nio.file.NoSuchFileException)return java.nio.file.FileVisitResult.CONTINUE;throw e;}
            @Override public java.nio.file.FileVisitResult postVisitDirectory(java.nio.file.Path dir,java.io.IOException e)throws java.io.IOException{
                if(e==null||e instanceof java.nio.file.NoSuchFileException)return java.nio.file.FileVisitResult.CONTINUE;throw e;}
        });
    }
    public static long allocatedBytes(File root)throws Exception {
        java.util.Set<String> seen=new java.util.HashSet<>();
        long[] bytes={0};
        walkPresent(root,path->{
            try {
                android.system.StructStat stat=android.system.Os.lstat(path.toString());
                if(seen.add(stat.st_dev+":"+stat.st_ino))bytes[0]=Math.addExact(bytes[0],Math.multiplyExact(stat.st_blocks,512));
            } catch(android.system.ErrnoException gone) {
                if(gone.errno!=android.system.OsConstants.ENOENT)throw new java.io.UncheckedIOException(new java.io.IOException(gone));
            }
        });
        return bytes[0];
    }
    public static long treeBytes(File root)throws Exception {
        long[] bytes={0};
        walkPresent(root,path->{
            try{if(Files.isRegularFile(path,java.nio.file.LinkOption.NOFOLLOW_LINKS))bytes[0]=Math.addExact(bytes[0],Files.size(path));}
            catch(java.nio.file.NoSuchFileException gone){}
            catch(java.io.IOException e){throw new java.io.UncheckedIOException(e);}
        });
        return bytes[0];
    }
    private static byte[] blob(java.io.DataInputStream in)throws Exception {
        int count=in.readInt();if(count<0)throw new java.io.IOException("Invalid Flatpak field length");
        byte[] bytes=new byte[count];in.readFully(bytes);return bytes;
    }
    private static String field(java.io.DataInputStream in)throws Exception{return new String(blob(in),StandardCharsets.UTF_8);}

    public static JSONObject row(JSONObject manifest,String ref)throws Exception {
        org.json.JSONArray rows=manifest.getJSONArray("deployments");
        for(int i=0;i<rows.length();i++)if(ref.equals(rows.getJSONObject(i).getString("ref")))return rows.getJSONObject(i);
        throw new java.io.IOException("Missing planned package: "+ref);
    }
    /** Each Flatpak installation gets its own hardlink store (compat-proxy nearest
     *  .v3-store-root), so removing that installation tree frees its backing objects. */
    static void markStoreRoot(File dir)throws java.io.IOException {
        File marker=new File(dir,".v3-store-root");
        if(marker.isFile())return;
        java.nio.file.Files.createFile(marker.toPath(),
                java.nio.file.attribute.PosixFilePermissions.asFileAttribute(
                        java.nio.file.attribute.PosixFilePermissions.fromString("rw-------")));
    }
    public static File installationFor(Context context,String id,JSONObject row)throws Exception {
        String location=row.getString("location");
        if(!location.isEmpty()&&!location.matches("package-[0-9]{1,3}"))throw new java.io.IOException("Invalid planned scratch");
        return new File(location.isEmpty()?operation(context,id):new File(operation(context,id),location),"installation");
    }
    public static JSONObject packageManifest(JSONObject plan,String ref)throws Exception {
        return InstallHandoff.packageManifest(plan,ref);
    }

    public static File installation(Context context,String id) throws Exception {
        return new File(operation(context,id),"installation");
    }
    private static File operation(Context context,String id) throws Exception {
        if(id==null||!OPERATION_ID.matcher(id).matches())throw new IllegalArgumentException("Invalid operation ID");
        File parent=new File(WorkDirs.root(context),"flatpak-staging");
        return new File(parent,id);
    }
    public static void prepare(Context context,String id,boolean repair) throws Exception {
        File operation=operation(context,id);
        // A reused caller ID must never adopt a prior interrupted transaction.
        cleanup(context,id);
        for(String name:new String[]{"installation","home","cache","tmp","run"})
            if(!new File(operation,name).mkdirs())throw new java.io.IOException("Cannot create private staging "+name);
        markStoreRoot(operation);
        markStoreRoot(new File(operation,"installation"));
        if(repair)Files.write(new File(operation,"repair").toPath(),new byte[]{1});
    }
    public static void cleanup(Context context,String id) {
        try {
            java.nio.file.Files.walkFileTree(operation(context,id).toPath(),new java.nio.file.SimpleFileVisitor<java.nio.file.Path>() {
                @Override public java.nio.file.FileVisitResult visitFile(java.nio.file.Path p,java.nio.file.attribute.BasicFileAttributes a) throws java.io.IOException {Files.delete(p);return java.nio.file.FileVisitResult.CONTINUE;}
                @Override public java.nio.file.FileVisitResult postVisitDirectory(java.nio.file.Path p,java.io.IOException e) throws java.io.IOException {if(e!=null)throw e;Files.delete(p);return java.nio.file.FileVisitResult.CONTINUE;}
            });
        }catch(java.nio.file.NoSuchFileException absent) { }
        catch(Exception error){Log.w(TAG,"Cannot clean private staging",error);}
    }
    private File operationDirectory() throws Exception {
        File directory=operation(context,operationId);
        WorkDirs.require(directory);
        return directory;
    }

    private static File requireDirectory(File operation, String name) throws Exception {
        File directory = new File(operation, name).getCanonicalFile();
        if (!operation.getPath().equals(directory.getParent())
                || !directory.isDirectory() || Files.isSymbolicLink(directory.toPath()))
            throw new java.io.IOException("Missing staging directory " + name);
        android.system.StructStat stat = android.system.Os.lstat(directory.getPath());
        if (stat.st_uid != android.os.Process.myUid())
            throw new java.io.IOException("Unowned staging directory " + name);
        return directory;
    }

    /** Digits immediately before a {@code %} on this line, as the CLI reports it. */
    private static long downloadSize(byte[] metadata) { return metadataSize(metadata,"xa.download-size"); }

    private static long metadataSize(byte[] metadata,String key) {
        String text=new String(metadata,StandardCharsets.UTF_8);
        java.util.regex.Matcher matcher=java.util.regex.Pattern.compile("(?m)^"+java.util.regex.Pattern.quote(key)+"\\s*=\\s*(\\d+)\\s*$").matcher(text);
        if(!matcher.find())return -1;
        try{return Long.parseLong(matcher.group(1));}catch(NumberFormatException ignored){return -1;}
    }

    private static String readErrorLog(File file) throws Exception {
        byte[] bytes=org.matonos.linuxhost.stubgen.PlaceholderFiles.readAllBytes(file);
        return new String(bytes,StandardCharsets.UTF_8);
    }

    private synchronized void report(String phase, int percent, long bytesDone, long bytesTotal, String line) {
        try {
            long now=System.currentTimeMillis();
            if(percent>=0) {percent=Math.max(lastReportedPercent,Math.min(100,percent));if(percent==lastReportedPercent&&now-lastReportedAt<250)return;lastReportedPercent=percent;lastReportedAt=now;}
            String text = line == null ? "" : line;
            callback.onProgress(operationId, phase, percent,bytesDone,bytesTotal,
                    text);
        } catch (Exception error) { Log.w(TAG, "Cannot report install progress", error); }
    }

    private void fail(String message) {
        try { callback.onFailed(operationId, message); }
        catch (Exception error) { Log.w(TAG, "Cannot report install failure", error); }
    }
}
