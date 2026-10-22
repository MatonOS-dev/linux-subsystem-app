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
    private static final int MAX_LINE = 2048;

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
    private volatile Process process;
    private int lastReportedPercent = -1;
    private long lastReportedAt;
    private long plannedBytes;
    private boolean plannedBytesKnown=true;
    private long completedBytes;
    private int packageCount;
    private int packageIndex;
    private long verifiedPayloadBytes;
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
            finally { completion.workerFinished(); }
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
        Process child = process;
        if (child != null) child.destroyForcibly();
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
            java.util.Map<String,String> mounts=new java.util.HashMap<>();
            for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan) {
                String mount;
                if(node.ref.equals(appRef)) {
                    org.matonos.linuxhost.stubgen.DeploymentRecord previous=installed.records.get(node.ref);
                    mount=previous!=null?previous.mount:(appRef.startsWith("app/")?"/app":"/usr");
                } else mount=PackageMounts.mountPoint(node.ref,metadata.get(node.ref),metadata);
                mounts.put(node.ref,mount);
            }
            org.json.JSONArray rows=new org.json.JSONArray();String appCommit=null;
            packageCount=plan.size();plannedBytes=0;plannedBytesKnown=true;
            for(org.matonos.linuxhost.stubgen.DependencyPlan.Node node:plan)if(!node.reused) {
                long size=downloadSize(node.info.metadata);if(size<0)plannedBytesKnown=false;else plannedBytes+=size;
                long payload=metadataSize(node.info.metadata,"xa.installed-size");if(payload<0)plannedPayloadBytesKnown=false;else plannedPayloadBytes+=payload;
            }
            // All provider choices complete before the first content pull.
            for(int i=0;i<plan.size();i++) {
                checkCancelled();org.matonos.linuxhost.stubgen.DependencyPlan.Node node=plan.get(i);
                String location=node.ref.equals(appRef)?"":"package-"+i;
                if(!node.reused) {
                    packageIndex=i;
                    Scratch scratch=new Scratch(location.isEmpty()?operation:new File(operation,location),node.origin);
                    scratch.seed(installed,node.ref);
                    scratch.pull(node);
                }
                org.json.JSONArray dependencies=new org.json.JSONArray();for(String ref:node.dependencies)dependencies.put(ref);
                rows.put(new JSONObject().put("ref",node.ref).put("commit",node.info.commit).put("origin",node.origin)
                        .put("metadata",metadata.get(node.ref)).put("mount",mounts.get(node.ref)).put("location",location)
                        .put("reused",node.reused).put("dependencies",dependencies));
                if(node.ref.equals(appRef))appCommit=node.info.commit;
            }
            if(appCommit==null)throw new java.io.IOException("Plan has no requested ref");
            long verified=plannedPayloadBytesKnown?verifiedPayloadBytes:-1;
            long verifyTotal=plannedPayloadBytesKnown?plannedPayloadBytes:-1;
            report("verify",82,verified,verifyTotal,"Package verification complete");
            report("staging",82,-1,-1,"Packages downloaded; preparing install packs");
            DnsForwarder.release();
            callback.onComplete(operationId,new JSONObject().put("version",1).put("appRef",appRef).put("appCommit",appCommit)
                    .put("origin",remote).put("deployments",rows).toString());
        }catch(Throwable error) {
            Log.e(TAG,"Install worker failed for "+appRef,error);
            fail(error.getMessage()==null?error.toString():error.getMessage());
        }finally{DnsForwarder.release();}
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
            cache=requireDirectory(root,"cache");tmp=requireDirectory(root,"tmp");run=requireDirectory(root,"run");
            String dns=LoaderCommand.resolverAddress();File resolv=new File(run,"install-resolv.conf");LoaderCommand.writeResolv(resolv,dns);
            prefix.add(LoaderCommand.loader(context).getAbsolutePath());prefix.add("--parent-pid");prefix.add(Integer.toString(android.os.Process.myPid()));
            LoaderCommand.addDns(prefix,dns,WorkDirs.root(context));LoaderCommand.addResolvMaps(prefix,resolv);
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
            Process child=builder.start();process=child;
            try {
                byte[] output;
                try(java.io.InputStream in=child.getInputStream()){output=in.readNBytes(4*1024*1024+1);if(output.length>4*1024*1024)throw new java.io.IOException("Flatpak output exceeds limit");}
                int status=child.waitFor();checkCancelled();
                if(status==3&&absentAllowed)return null;
                if(status!=0)throw new java.io.IOException("Flatpak operation failed: "+new String(Files.readAllBytes(error.toPath()),StandardCharsets.UTF_8));
                return output;
            }finally{process=null;child.destroyForcibly();error.delete();}
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
                String commit=field(in,64);byte[] metadata=blob(in,128*1024);String runtime=field(in,1024);
                int extensions=in.readInt();if(extensions<0||extensions>1)throw new java.io.IOException("Invalid extension flag");
                int count=in.readInt();if(count<0||count>128)throw new java.io.IOException("Invalid related-ref count");
                java.util.List<String> related=new ArrayList<>();for(int i=0;i<count;i++)related.add(field(in,1024));
                if(in.read()!=-1)throw new java.io.IOException("Trailing Flatpak plan bytes");
                return new org.matonos.linuxhost.stubgen.DependencyPlan.Info(commit,metadata,runtime,related,extensions==1);
            }
        }
        org.matonos.linuxhost.stubgen.DependencyPlan.Info requirements(org.matonos.linuxhost.stubgen.DeploymentRecord record)throws Exception {
            File file=File.createTempFile("requirements-",".ini",tmp);Files.write(file.toPath(),record.metadata);
            try {
                String helper=new File(context.getApplicationInfo().nativeLibraryDir,"libmatonos-flatpak-plan.so").getAbsolutePath();
                byte[] bytes=command(java.util.Arrays.asList(helper,"--requirements",file.getAbsolutePath(),record.ref),false);
                try(java.io.DataInputStream in=new java.io.DataInputStream(new java.io.ByteArrayInputStream(bytes))) {
                    String runtime=field(in,1024);int related=in.readInt();if(related<0||related>1||in.read()!=-1)throw new java.io.IOException("Invalid Flatpak requirements");
                    return new org.matonos.linuxhost.stubgen.DependencyPlan.Info(record.commit,record.metadata,runtime,java.util.Collections.emptyList(),related==1);
                }
            }finally{file.delete();}
        }
        java.util.List<String> related(String ref,byte[] metadata)throws Exception {
            File file=File.createTempFile("related-",".ini",tmp);Files.write(file.toPath(),metadata);
            try {
                String helper=new File(context.getApplicationInfo().nativeLibraryDir,"libmatonos-flatpak-plan.so").getAbsolutePath();
                byte[] bytes=command(java.util.Arrays.asList(helper,installation.getAbsolutePath(),origin.name,ref,file.getAbsolutePath()),false);
                try(java.io.DataInputStream in=new java.io.DataInputStream(new java.io.ByteArrayInputStream(bytes))) {
                    int count=in.readInt();if(count<0||count>128)throw new java.io.IOException("Invalid related-ref count");
                    java.util.List<String> refs=new ArrayList<>();for(int i=0;i<count;i++)refs.add(field(in,1024));
                    if(in.read()!=-1)throw new java.io.IOException("Trailing related-ref data");return refs;
                }
            }finally{file.delete();}
        }
        final java.util.Set<String> deltaBases=new java.util.HashSet<>();
        void seed(InstalledPackages installed,String ref)throws Exception {
            if(installed.installed(ref)==null)return;
            android.content.pm.PackageInfo pkg=context.getPackageManager().getPackageInfo(
                    DeployManifest.packageForPublic(ref),0);
            java.util.List<String> args=org.matonos.linuxhost.stubgen.PackLaunch.arguments(
                    new File(pkg.applicationInfo.sourceDir),pkg.splitNames,pkg.applicationInfo.splitSourceDirs,new File(root,"seed"));
            java.util.List<org.matonos.linuxhost.stubgen.ObjectPacks.Entry> entries=
                    org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(new File(args.get(2)));
            long required=org.matonos.linuxhost.stubgen.PackSeed.bytes(entries,
                    new android.os.StatFs(root.getAbsolutePath()).getBlockSizeLong()),available=root.getUsableSpace();
            report("staging",-1,-1,-1,"Update base temporary bytes: "+required+"; available: "+available);
            if(required>available)throw new java.io.IOException("Insufficient space for installed update base: "+required+" bytes required, "+available+" available");
            try {
                org.matonos.linuxhost.stubgen.PackSeed.seed(entries,new File(args.get(3)),new File(installation,"repo"),()->cancelled.get());
                org.matonos.linuxhost.stubgen.DeploymentRecord record=org.matonos.linuxhost.stubgen.DeploymentRecord.installed(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);
                java.util.List<String> original=org.matonos.linuxhost.stubgen.PackLaunch.originalArguments(pkg.splitNames,pkg.applicationInfo.splitSourceDirs,new File(root,"seed-original"));
                java.util.List<org.matonos.linuxhost.stubgen.ObjectPacks.Entry> originalEntries=org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(new File(original.get(2)));
                long originalBytes=org.matonos.linuxhost.stubgen.PackSeed.bytes(originalEntries,new android.os.StatFs(root.getAbsolutePath()).getBlockSizeLong());
                report("staging",-1,-1,-1,"Original update base payload budget: "+originalBytes);
                if(originalBytes>root.getUsableSpace())throw new java.io.IOException("Insufficient space for original update base");
                org.matonos.linuxhost.stubgen.PackSeed.seed(originalEntries,new File(original.get(3)),new File(installation,"repo"),()->cancelled.get());
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
            InstallProgress parser=new InstallProgress();
                cliProgress(parser,total,size,"install","--system","--assumeyes","--no-deps","--no-related",deltaBases.contains(node.ref)?"--ostree-verbose":"--no-static-deltas",origin.name,node.ref);
            JSONObject manifest=DeployManifest.build(installation,node.ref,origin.name,operationId);
            if(!node.info.commit.equals(manifest.getString("appCommit"))) {
                // install has no --commit option; update can bind the downloaded
                // deployment to the exact snapshot inspected before any pull.
                cliProgress(parser,total,size,"update","--system","--assumeyes","--no-deps","--no-related",deltaBases.contains(node.ref)?"--ostree-verbose":"--no-static-deltas","--commit="+node.info.commit,node.ref);
                manifest=DeployManifest.build(installation,node.ref,origin.name,operationId);
            }
            if(!node.info.commit.equals(manifest.getString("appCommit"))||!java.util.Arrays.equals(node.info.metadata,
                    org.matonos.linuxhost.stubgen.PlaceholderFiles.readAllBytes(new File(DeployManifest.stagedDeployment(installation,manifest),"metadata"))))
                throw new java.io.IOException("Pulled deployment differs from planned Flatpak commit");
            StagedPacks.verify(context,installation,()->cancelled.get());
            long payload=metadataSize(node.info.metadata,"xa.installed-size");if(payload>0)verifiedPayloadBytes+=payload;
        }
        private void cliProgress(InstallProgress parser,long total,long packageBytes,String... args)throws Exception {
            java.util.List<String> tail=new ArrayList<>();tail.add(LoaderCommand.tool(context,LoaderCommand.FLATPAK_CLI).getAbsolutePath());java.util.Collections.addAll(tail,args);
            commandProgress(tail,parser,total,packageBytes);
        }
        private void commandProgress(java.util.List<String> tail,InstallProgress parser,long total,long packageBytes)throws Exception {
            checkCancelled();java.util.List<String> command=new ArrayList<>(prefix);command.addAll(tail);
            File error=File.createTempFile("flatpak-",".log",tmp);
            ProcessBuilder builder=new ProcessBuilder(command).directory(home).redirectError(error);
            Map<String,String> env=builder.environment();env.clear();String nativeDir=LoaderCommand.nativeLibraryDir(context).getAbsolutePath();
            env.put("PATH",nativeDir+":/system/bin");env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");env.put("LC_ALL","C");
            env.put("HOME",home.getAbsolutePath());env.put("TMPDIR",tmp.getAbsolutePath());env.put("XDG_RUNTIME_DIR",run.getAbsolutePath());
            env.put("FLATPAK_SYSTEM_DIR",installation.getAbsolutePath());env.put("FLATPAK_USER_DIR",installation.getAbsolutePath());
            env.put("FLATPAK_SYSTEM_CACHE_DIR",cache.getAbsolutePath());env.put("FLATPAK_DOWNLOAD_TMPDIR",tmp.getAbsolutePath());
            for(String key:new String[]{"SSL_CERT_DIR","CURL_CA_BUNDLE","G_TLS_CA_PATH"})env.put(key,"/apex/com.android.conscrypt/cacerts");
            Process child=builder.start();process=child;
            try(java.io.BufferedReader reader=new java.io.BufferedReader(new InputStreamReader(child.getInputStream(),StandardCharsets.UTF_8))) {
                String line;long lastSent=0;int lastPercent=-1;
                while((line=reader.readLine())!=null) {int p=InstallProgress.parsePercent(line);if(p>=0&&(p!=lastPercent||System.currentTimeMillis()-lastSent>=250)) {
                    long done=total>0?completedBytes+(Math.max(1,packageBytes)*p/100):-1;
                    int phasePercent=total>0?(int)Math.min(100,done*100/total):(packageIndex+1)*p/Math.max(1,packageCount);
                    report("download",5+(phasePercent*70/100),done,total,line);lastPercent=p;lastSent=System.currentTimeMillis();}}
                int status=child.waitFor();checkCancelled();if(status!=0)throw new java.io.IOException("Flatpak operation failed: "+readErrorLog(error));
                completedBytes+=Math.max(1,packageBytes);
            } finally {process=null;child.destroyForcibly();error.delete();}
        }
    }
    /** Count allocated blocks once per inode, including hardlinked checkouts. */
    public static long allocatedBytes(File root)throws Exception {
        java.util.Set<String> seen=new java.util.HashSet<>();
        long[] bytes={0};
        try(java.util.stream.Stream<java.nio.file.Path> paths=Files.walk(root.toPath())) {
            java.util.Iterator<java.nio.file.Path> it=paths.iterator();
            while(it.hasNext()) {
                android.system.StructStat stat=android.system.Os.lstat(it.next().toString());
                if(seen.add(stat.st_dev+":"+stat.st_ino))bytes[0]=Math.addExact(bytes[0],Math.multiplyExact(stat.st_blocks,512));
            }
        }
        return bytes[0];
    }
    public static long treeBytes(File root)throws Exception {
        long[] bytes={0};
        try(java.util.stream.Stream<java.nio.file.Path> paths=Files.walk(root.toPath())) {
            java.util.Iterator<java.nio.file.Path> it=paths.iterator();
            while(it.hasNext()) {java.nio.file.Path path=it.next();if(Files.isRegularFile(path,java.nio.file.LinkOption.NOFOLLOW_LINKS))bytes[0]=Math.addExact(bytes[0],Files.size(path));}
        }
        return bytes[0];
    }
    private static byte[] blob(java.io.DataInputStream in,int limit)throws Exception {
        int count=in.readInt();if(count<0||count>limit)throw new java.io.IOException("Invalid Flatpak field length");
        byte[] bytes=new byte[count];in.readFully(bytes);return bytes;
    }
    private static String field(java.io.DataInputStream in,int limit)throws Exception{return new String(blob(in,limit),StandardCharsets.UTF_8);}

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
        if(bytes.length>4*1024*1024)throw new java.io.IOException("Flatpak error output too large");
        return new String(bytes,StandardCharsets.UTF_8);
    }

    private synchronized void report(String phase, int percent, long bytesDone, long bytesTotal, String line) {
        try {
            long now=System.currentTimeMillis();
            if(percent>=0) {percent=Math.max(lastReportedPercent,Math.min(100,percent));if(percent==lastReportedPercent&&now-lastReportedAt<250)return;lastReportedPercent=percent;lastReportedAt=now;}
            String text = line == null ? "" : line;
            callback.onProgress(operationId, phase, percent,bytesDone,bytesTotal,
                    text.length() > MAX_LINE ? text.substring(0, MAX_LINE) : text);
        } catch (Exception error) { Log.w(TAG, "Cannot report install progress", error); }
    }

    private void fail(String message) {
        try { callback.onFailed(operationId, message); }
        catch (Exception error) { Log.w(TAG, "Cannot report install failure", error); }
    }
}
