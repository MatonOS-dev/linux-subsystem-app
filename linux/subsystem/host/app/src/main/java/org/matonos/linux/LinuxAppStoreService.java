package org.matonos.linux;

import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.PackageInfo;
import android.content.ComponentName;
import android.content.pm.PackageInstaller;
import android.content.IntentSender;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import org.json.JSONObject;
import org.matonos.linux.stub.InstallTransaction;
import org.matonos.linuxhost.stubgen.StubGenerator;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

/** Permission-checked Binder front end for the Linux app store. */
public final class LinuxAppStoreService extends Service {
    private static final String TAG = "LinuxAppStore";
    private static final String INSTALL = "org.matonos.permission.INSTALL_LINUX_APPS";
    private static final String QUERY = "org.matonos.permission.QUERY_LINUX_APPS";
    private static final int MAX_ICON_BYTES = 1024 * 1024;
    private final java.util.Set<InstallTransaction> activeInstalls = java.util.concurrent.ConcurrentHashMap.newKeySet();
    private android.os.PowerManager.WakeLock installWakeLock;

    private void beginInstallForeground() {
        android.app.NotificationManager manager=getSystemService(android.app.NotificationManager.class);
        manager.createNotificationChannel(new android.app.NotificationChannel("linux-install",
                "Linux application downloads",android.app.NotificationManager.IMPORTANCE_LOW));
        startForeground(3,new android.app.Notification.Builder(this,"linux-install")
                .setContentTitle("Linux application download").setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true).build(),android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        installWakeLock=getSystemService(android.os.PowerManager.class).newWakeLock(
                android.os.PowerManager.PARTIAL_WAKE_LOCK,"MatonOS:Install");
        installWakeLock.setReferenceCounted(true);
        installWakeLock.acquire();
    }

    private synchronized void finishInstall(InstallTransaction transaction) {
        if(!activeInstalls.remove(transaction))return;
        if(installWakeLock!=null && installWakeLock.isHeld())installWakeLock.release();
        if(activeInstalls.isEmpty()) { installWakeLock=null; stopForeground(STOP_FOREGROUND_REMOVE); }
    }

    // Bound concurrent store jobs by the device's available CPU parallelism.
    private final ExecutorService worker = Executors.newFixedThreadPool(Math.max(1,Runtime.getRuntime().availableProcessors()));
    private final Map<String, Operation> operations = new ConcurrentHashMap<>();
    private final Map<String, Operation> pending = new ConcurrentHashMap<>();
    private final Map<String,Object> packageInstallLocks = new ConcurrentHashMap<>();
    private static volatile LinuxAppStoreService instance;
    private interface FinalizePackage {void run()throws Exception;}
    private static final class Operation {
        final java.util.List<FinalizePackage> finalizers=new java.util.ArrayList<>();
        final String stagingId="stage-"+java.util.UUID.randomUUID().toString().replace("-","");
        final int uid;
        final String id, kind, ref, remote, name, url;
        final ILinuxAppStoreCallback callback;
        volatile boolean cancelled;
        volatile InstallTransaction installService;
        volatile String stagedMetadata;
        volatile File packArtifacts;
        // cancel() can reach result() on a Binder thread while the worker adds
        // leases, so never iterate this list without holding its monitor.
        final java.util.List<AutoCloseable> workLeases=java.util.Collections.synchronizedList(new java.util.ArrayList<>());
        volatile File stubWork;
        volatile PackageInfo installed;
        volatile JSONObject stagedManifest;
        volatile org.matonos.linux.stub.PackInstaller packInstaller;
        final AtomicBoolean completed=new AtomicBoolean();
        Operation(int uid, String id, String kind, String ref, String remote, String name, String url,
                ILinuxAppStoreCallback callback) {
            this.uid=uid; this.id=id; this.kind=kind; this.ref=ref; this.remote=remote;
            this.name=name; this.url=url; this.callback=callback;
        }
    }

    private final ILinuxAppStore.Stub binder = new ILinuxAppStore.Stub() {
        @Override public void install(String ref, String remote, String id, ILinuxAppStoreCallback cb) {
            submit("install", ref, remote, null, null, id, cb);
        }
        @Override public void update(String ref, String id, ILinuxAppStoreCallback cb) {
            submit("update", ref, null, null, null, id, cb);
        }
        @Override public void repair(String ref,String id,ILinuxAppStoreCallback cb){
            int uid=Binder.getCallingUid();
            if(id==null||!id.matches("[A-Za-z0-9_-]{1,64}")||cb==null)throw new IllegalArgumentException("Invalid repair request");
            PackageInfo info;
            try {
                String pkg=packageFor(ref);String[] packages=getPackageManager().getPackagesForUid(uid);
                if(packages==null||packages.length!=1||!packages[0].equals(pkg))throw new SecurityException("Repair caller is not the stub for this app ref");
                if(!ref.equals(authenticateStubRef(uid)))throw new SecurityException("Repair ref does not match the installed stub");
                info=getPackageManager().getPackageInfo(pkg,PackageManager.GET_SIGNING_CERTIFICATES);
                if(!packageSignerMatches(info))throw new SecurityException("Untrusted repair package");
            }catch(SecurityException e){throw e;}catch(Exception e){throw new SecurityException("Cannot authenticate stub repair",e);}
            Operation op=new Operation(uid,id,"repair",ref,"flathub",null,null,cb);String key=operationKey(uid,id);
            if(operations.putIfAbsent(key,op)!=null)throw new IllegalArgumentException("Operation ID already active for caller");
            worker.execute(()->execute(id,op));
        }
        @Override public void uninstall(String ref, String id, ILinuxAppStoreCallback cb) {
            submit("uninstall", ref, null, null, null, id, cb);
        }
        @Override public String listInstalled() { enforceQuery(); return listInstalledPackages(); }
        @Override public String listRemotes() { enforceQuery(); return remoteList(); }
        @Override public void addRemote(String name, String url, String id, ILinuxAppStoreCallback cb) {
            submit("addRemote", null, null, name, url, id, cb);
        }
        @Override public void removeRemote(String name, String id, ILinuxAppStoreCallback cb) {
            submit("removeRemote", null, null, name, null, id, cb);
        }
        @Override public void cancel(String id) {
            int uid=Binder.getCallingUid(); Operation op=operations.get(operationKey(uid,id));
            if(op==null || !StorePermissionPolicy.ownsOperation(op.uid,uid))
                throw new SecurityException("Operation is not owned by caller");
            if(op.packInstaller!=null && !op.packInstaller.cancelBeforeCommit())return;
            op.cancelled=true;
            if(op.installService!=null)try{op.installService.cancel(op.stagingId);}catch(Exception ignored){}
            if(op.installService==null)result(op,false,"Operation cancelled");
        }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }

    @Override public void onCreate() { super.onCreate(); instance=this;
        // Stale work can be gigabytes; never walk it on the main thread.
        worker.execute(this::sweepWork); }

    @Override public int onStartCommand(Intent intent,int flags,int startId){
        if(intent!=null&&"org.matonos.linux.CLEAN_UNUSED_PACKAGES".equals(intent.getAction())){
            worker.execute(()->{sweepWork();cleanupUnusedPackages();stopSelf(startId);});return START_NOT_STICKY;
        }
        return START_NOT_STICKY;
    }

    static void confirmPending(int uid, String id, boolean accepted) {
        LinuxAppStoreService service=instance;
        if(service!=null) service.confirm(uid,id,accepted);
    }

    private void submit(String kind, String ref, String remote, String name, String url,
            String id, ILinuxAppStoreCallback callback) {
        int uid=Binder.getCallingUid();
        if (id==null || !id.matches("[A-Za-z0-9_-]{1,64}") || callback==null)
            throw new IllegalArgumentException("Invalid operation request");
        boolean privileged=checkCallingPermission(INSTALL)==PackageManager.PERMISSION_GRANTED;
        if (!privileged && (kind.equals("install") || kind.equals("update"))) {
            ref=authenticateStubRef(uid);
            try {remote=remoteFor(ref);}catch(Exception e){throw new SecurityException("Cannot resolve installed origin",e);}
        }
        if(kind.equals("install"))try {
            remote=privileged?normalizeRemote(remote):org.matonos.linuxhost.stubgen.RemoteOrigin.parse(remote).encode();
            try {String installedOrigin=remoteFor(ref);if(!installedOrigin.equals(remote))throw new SecurityException("Ref already installed from another remote; uninstall first");}
            catch(PackageManager.NameNotFoundException absent) { }
        }catch(RuntimeException e){throw e;}catch(Exception e){throw new IllegalArgumentException("Invalid install origin",e);}
        Operation op=new Operation(uid,id,kind,ref,remote,name,url,callback);
        String key=operationKey(uid,id);
        if(operations.putIfAbsent(key,op)!=null) throw new IllegalArgumentException("Operation ID already active for caller");
        boolean needsConfirm=(kind.equals("install") && !"flathub".equals(remote)) || (kind.equals("addRemote") || kind.equals("removeRemote")
                ? StorePermissionPolicy.remoteMutation(privileged)==StorePermissionPolicy.Decision.CONFIRM
                : StorePermissionPolicy.packageOperation(privileged)==StorePermissionPolicy.Decision.CONFIRM);
        if(needsConfirm) {
            pending.put(key,op);
            Intent intent=new Intent(this,ConfirmInstallActivity.class)
                    .putExtra("operationId",id).putExtra("kind",kind).putExtra("ref",ref)
                    .putExtra("remote",remoteDisplay(remote)).putExtra("name",name).putExtra("url",url)
                    .putExtra("caller",callerLabel(uid)).putExtra("callerUid",uid).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
        } else worker.execute(()->execute(id,op));
    }

    /** Called only by the non-exported confirmation activity after explicit consent. */
    void confirm(int uid, String id, boolean accepted) {
        Operation op=pending.remove(operationKey(uid,id));
        if(op==null) return;
        if(!accepted) { result(op,false,"Request declined"); return; }
        worker.execute(()->execute(id,op));
    }

    private void execute(String id, Operation op) {
        try {
            if(op.cancelled) return;
            switch(op.kind) {
                case "install": case "update": case "repair": runStagedInstall(id,op); return;
                case "uninstall": uninstallStub(op); return;
                case "addRemote":
                    if(!"flathub".equals(op.name) || (op.url!=null && !op.url.isEmpty() && !"https://dl.flathub.org/repo/flathub.flatpakrepo".equals(op.url)))
                        throw new IllegalArgumentException("Only the configured Flathub remote is supported");
                    getSharedPreferences("flatpak-remotes",MODE_PRIVATE).edit().putBoolean("flathub",true).apply();
                    result(op,true,null); return;
                case "removeRemote":
                    getSharedPreferences("flatpak-remotes",MODE_PRIVATE).edit().remove(op.name).apply();
                    result(op,true,null); return;
                default: throw new IllegalArgumentException("Unknown operation");
            }
        } catch(Exception error) {
            Log.w(TAG,"Store operation failed",error);
            result(op,false,error.getMessage()==null?error.toString():error.getMessage());
        }
    }

    private String remoteList() {
        JSONObject result=new JSONObject();
        try {
            boolean enabled=getSharedPreferences("flatpak-remotes",MODE_PRIVATE).getBoolean("flathub",true);
            result.put("ok",true).put("output",enabled?"flathub":"");
            return result.toString();
        } catch(Exception error) { return "{\"ok\":false,\"error\":\"Cannot read remote preferences\"}"; }
    }

    private void runStagedInstall(String id, Operation op) throws Exception {
        final String remote="update".equals(op.kind)||"repair".equals(op.kind)?remoteFor(op.ref):op.remote;
        if("install".equals(op.kind))try {
            if(!remoteFor(op.ref).equals(remote))throw new SecurityException("Ref already installed from another remote; uninstall first");
        }catch(PackageManager.NameNotFoundException absent) { }
        op.workLeases.add(WorkDirs.keep(new File(new File(WorkDirs.root(this),"flatpak-staging"),op.stagingId)));
        op.stubWork=new File(WorkDirs.directory(this,"flatpak-stubs"),op.stagingId);
        op.workLeases.add(WorkDirs.keep(op.stubWork));
        java.nio.file.Files.createDirectories(op.stubWork.toPath());
        InstallTransaction.prepare(this,op.stagingId,"repair".equals(op.kind));
        InstallTransaction.Callback stagedCallback=new InstallTransaction.Callback() {
            @Override public void onProgress(String operationId,String phase,int percent,long bytesDone,long bytesTotal,String line) {
                try { op.callback.onProgress(op.id,phase,percent,bytesDone,bytesTotal); }
                catch(Exception e) { Log.w(TAG,"Cannot deliver install progress",e); }
            }
            @Override public void onComplete(String operationId,String manifest) {
                try {
                    JSONObject staged = new JSONObject(manifest);
                    op.stagedManifest=staged;
                    File installation=InstallTransaction.installation(LinuxAppStoreService.this,op.stagingId);
                    String details = org.matonos.linux.stub.DeployManifest.stagedMetadata(installation, staged);
                    op.stagedMetadata = details;
                    File scratch=new File(WorkDirs.root(LinuxAppStoreService.this),"flatpak-pack-artifacts");
                    if(!scratch.isDirectory()&&!scratch.mkdirs())throw new java.io.IOException("Cannot create pack scratch");
                    op.packArtifacts=new File(scratch,op.stagingId);
                    op.workLeases.add(WorkDirs.keep(op.packArtifacts));
                    java.nio.file.Files.createDirectory(op.packArtifacts.toPath());
                    try {op.installed=getPackageManager().getPackageInfo(packageFor(op.ref),0);}
                    catch(PackageManager.NameNotFoundException absent){op.installed=null;}
                    long packBytes=InstallTransaction.treeBytes(new File(
                            org.matonos.linux.stub.DeployManifest.stagedDeployment(installation,InstallTransaction.packageManifest(staged,op.ref)),"files"));
                    op.callback.onProgress(op.id,"packs",85,0,packBytes);
                    buildAllPacks(op,installation,staged);
                    if(op.cancelled)throw new java.io.IOException("Install cancelled");
                    op.callback.onProgress(op.id,"packs",90,packBytes,packBytes);
                    installDeclaredPackages(op);
                    op.callback.onProgress(op.id,"Android package install",95,-1,-1);
                    installFinalStub(op);
                } catch(Throwable e) {
                    Log.e(TAG,"Staged install failed for "+op.ref,e);
                    result(op,false,e.getMessage()==null?e.toString():e.getMessage());
                } finally {if(op.cancelled)cleanArtifacts(op);}
            }
            @Override public void onFailed(String operationId,String error) {
                result(op,false,error);
            }
        };
        synchronized (this) {
            InstallTransaction transaction=InstallTransaction.create(this,stagedCallback,op.stagingId,op.ref,remote);
            activeInstalls.add(transaction);
            op.installService=transaction;
            try {
                if(activeInstalls.size()==1)beginInstallForeground();else installWakeLock.acquire();
                if(op.cancelled)transaction.cancel();
                transaction.start(()->finishInstall(transaction));
            } catch(RuntimeException error) {finishInstall(transaction);throw error;}
        }
    }

    private void sweepWork() {
        sweepWork(this);
    }

    private static void sweepWork(Context context) {
        try { WorkDirs.sweep(WorkDirs.root(context)); }
        catch(Exception error) { Log.w(TAG,"Cannot sweep stale work",error); }
    }

    private void cleanupUnusedPackages() {
        cleanupUnusedPackages(this);
    }

    private static void cleanupUnusedPackages(Context context) {
        try {
            PackageManager manager=context.getPackageManager();
            java.util.Set<String> needed=new java.util.HashSet<>();
            for(PackageInfo installed:manager.getInstalledPackages(PackageManager.GET_META_DATA|PackageManager.GET_SIGNING_CERTIFICATES)) {
                if(installed.applicationInfo==null||installed.signingInfo==null||installed.signingInfo.hasMultipleSigners())continue;
                if(!packageSignerMatches(context,installed))continue;
                android.content.pm.ApplicationInfo entry=manager.getApplicationInfo(installed.packageName,PackageManager.GET_META_DATA);
                if(entry.metaData==null)continue;
                String map=entry.metaData.getString(StubGenerator.DEPENDENCIES_META);
                if(map!=null)for(String row:map.split(";")){String[] fields=row.split(">",3);if(fields.length==3)needed.add(fields[1]);}
            }
            for(PackageInfo installed:manager.getInstalledPackages(PackageManager.GET_SIGNING_CERTIFICATES)) {
                if(installed.packageName==null||!installed.packageName.startsWith("flatpak."))continue;
                if(installed.splitNames!=null&&java.util.Arrays.stream(installed.splitNames).anyMatch(name->name.startsWith(StubGenerator.LAUNCHER_SPLIT_PREFIX)))continue;
                if(needed.contains(installed.packageName)||!packageSignerMatches(context,installed))continue;
                String action=context.getPackageName()+".PACKAGE_CLEAN."+java.util.UUID.randomUUID();
                PendingIntent status=PendingIntent.getBroadcast(context,0,new Intent(action).setPackage(context.getPackageName()),PendingIntent.FLAG_MUTABLE);
                manager.getPackageInstaller().uninstall(installed.packageName,status.getIntentSender());
            }
        } catch(Exception error){Log.w(TAG,"Runtime cleanup scan failed",error);}
    }

    /** System-scheduled cleanup avoids starting a background service from BOOT_COMPLETED. */
    public static final class CleanupJobService extends android.app.job.JobService {
        private Thread cleanupThread;
        @Override public boolean onStartJob(android.app.job.JobParameters params) {
            cleanupThread=new Thread(()->{
                try { sweepWork(this); cleanupUnusedPackages(this); }
                finally { jobFinished(params,false); cleanupThread=null; }
            },"matonos-package-cleanup");
            cleanupThread.start();
            return true;
        }
        @Override public boolean onStopJob(android.app.job.JobParameters params) {
            Thread thread=cleanupThread;if(thread!=null)thread.interrupt();
            return true;
        }
    }

    public static final class BootReceiver extends android.content.BroadcastReceiver {
        private static final int CLEANUP_JOB_ID=0x4d41544f;
        @Override public void onReceive(Context context,Intent intent) {
            if(!Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction()))return;
            PendingResult pending=goAsync();
            new Thread(()->{ErofsPin.ensure();pending.finish();}).start();
            android.app.job.JobInfo job=new android.app.job.JobInfo.Builder(CLEANUP_JOB_ID,
                    new ComponentName(context,CleanupJobService.class)).setPersisted(true).build();
            android.app.job.JobScheduler scheduler=context.getSystemService(android.app.job.JobScheduler.class);
            if(scheduler!=null)scheduler.schedule(job);
        }
    }


    private String remoteFor(String ref) throws Exception {
        PackageInfo pkg=getPackageManager().getPackageInfo(packageFor(ref),PackageManager.GET_SIGNING_CERTIFICATES);
        if(!packageSignerMatches(pkg))throw new SecurityException("Untrusted installed stub");
        org.matonos.linuxhost.stubgen.DeploymentRecord record=org.matonos.linuxhost.stubgen.DeploymentRecord.installed(pkg.splitNames,pkg.applicationInfo.splitSourceDirs);
        if(!record.ref.equals(ref))throw new SecurityException("Installed ref mismatch");
        return record.remote;
    }

    private static String remoteDisplay(String remote) {
        try{return org.matonos.linuxhost.stubgen.RemoteOrigin.parse(remote).display();}catch(Exception e){return remote;}
    }
    /** Existing Binder string accepts a JSON descriptor as well as the built-in name. */
    private static String normalizeRemote(String value) throws Exception {
        if("flathub".equals(value))return value;
        JSONObject descriptor=new JSONObject(value);
        if(descriptor.has("flatpakrepo"))return org.matonos.linuxhost.stubgen.RemoteOrigin.flatpakrepo(descriptor.getString("name"),descriptor.getString("flatpakrepo")).encode();
        return new org.matonos.linuxhost.stubgen.RemoteOrigin(descriptor.getString("name"),descriptor.getString("url"),descriptor.getString("gpgKey")).encode();
    }

    private int stubUid(String ref) throws Exception {
        return getPackageManager().getApplicationInfo(packageFor(ref),0).uid;
    }

    private String listInstalledPackages() {
        try {
            java.util.Set<String> refs=new java.util.TreeSet<>();
            for(PackageInfo pkg:getPackageManager().getInstalledPackages(0)) {
                if(pkg.applicationInfo==null||!pkg.packageName.startsWith("flatpak."))continue;
                try {String ref=authenticateStubRef(pkg.applicationInfo.uid);
                    if(pkg.packageName.equals(packageFor(ref)))refs.add(ref);
                } catch(Exception ignored) { }
            }
            return new JSONObject().put("ok",true).put("output",String.join("\n",refs)).toString();
        } catch(Exception error){throw new IllegalStateException("Cannot list installed stubs",error);}
    }

    private String authenticateStubRef(int uid) {
        try {
            PackageManager manager=getPackageManager();
            String[] packages=manager.getPackagesForUid(uid);
            if (packages==null || packages.length!=1) throw new SecurityException("Stub UID must own exactly one package");
            PackageInfo info=manager.getPackageInfo(packages[0],PackageManager.GET_SIGNING_CERTIFICATES);
            if (info.applicationInfo==null || info.signingInfo==null || info.signingInfo.hasMultipleSigners())
                throw new SecurityException("Stub must have exactly one signing certificate");
            android.content.pm.Signature[] signers=info.signingInfo.getApkContentsSigners();
            byte[] expected=org.matonos.linuxhost.stubgen.StubGenerator.getExistingSigningCertificate();
            byte[][] encoded=signers==null?null:new byte[signers.length][];
            if(encoded!=null) for(int i=0;i<signers.length;i++) encoded[i]=signers[i].toByteArray();
            if(!StubSelfInstallIdentity.matches(uid,info.applicationInfo.uid,packages,encoded,expected))
                throw new SecurityException("Caller is not a stub signed by this subsystem");
            android.content.pm.ApplicationInfo app=manager.getApplicationInfo(packages[0],PackageManager.GET_META_DATA);
            String ref=app.metaData==null?null:app.metaData.getString(StubGenerator.REF_META);
            if(!StubSelfInstallIdentity.validRef(ref)) throw new SecurityException("Stub has no valid Flatpak ref");
            return ref;
        } catch(SecurityException error) { throw error; }
        catch(Exception error) { throw new SecurityException("Cannot authenticate stub install request",error); }
    }

    private static String operationKey(int uid,String id) { return uid+":"+id; }

    private boolean packageSignerMatches(PackageInfo info) {
        return packageSignerMatches(this,info);
    }

    private static boolean packageSignerMatches(Context context,PackageInfo info) {
        try {
            if(info.signingInfo==null||info.signingInfo.hasMultipleSigners())return false;
            android.content.pm.Signature[] signers=info.signingInfo.getApkContentsSigners();
            byte[] expected=StubGenerator.getExistingSigningCertificate();
            return signers!=null&&signers.length==1&&expected!=null&&MessageDigest.isEqual(signers[0].toByteArray(),expected);
        }catch(Exception error){return false;}
    }

    private void installFinalStub(Operation op) throws Exception {
        boolean launchable=op.ref.startsWith("app/")&&org.matonos.linux.stub.PackageMounts.extensionParent(op.stagedMetadata)==null;
        if(op.installed!=null) {
            try {getPackageManager().getActivityInfo(new ComponentName(op.installed.packageName,StubGenerator.HOST_ACTIVITY),PackageManager.MATCH_DISABLED_COMPONENTS);launchable=true;}
            catch(PackageManager.NameNotFoundException noActivity){launchable=false;}
        }
        installStubPackage(op,op.ref,op.packArtifacts,op.installed,launchable,InstallTransaction.row(op.stagedManifest,op.ref).getString("mount"));
        // Every declared package's base/content must exist before any package
        // runs apply_extra: extension/runtime dependency graphs may contain cycles.
        for(FinalizePackage finish:op.finalizers)finish.run();
        finishStubInstall(op,success());
    }
    private static String commitFor(Operation op,String ref) throws Exception {
        org.json.JSONArray rows=op.stagedManifest.getJSONArray("deployments");
        for(int i=0;i<rows.length();i++)if(ref.equals(rows.getJSONObject(i).getString("ref")))return rows.getJSONObject(i).getString("commit");
        throw new java.io.IOException("Missing declared deployment: "+ref);
    }
    /** A ref's mount and launcher are parameters; every package uses this path. */
    private void installStubPackage(Operation op,String packageRef,File artifacts,PackageInfo installed,
            boolean launchable,String mountPoint) throws Exception {
        String packageName=packageFor(packageRef);
        Object lock=packageInstallLocks.computeIfAbsent(packageName,key->new Object());
        synchronized(lock) {
            if(installed==null)try {
                PackageInfo current=getPackageManager().getPackageInfo(packageName,PackageManager.GET_SIGNING_CERTIFICATES);
                JSONObject row=InstallTransaction.row(op.stagedManifest,packageRef);
                org.matonos.linuxhost.stubgen.DeploymentRecord record=org.matonos.linuxhost.stubgen.DeploymentRecord.installed(
                        current.splitNames,current.applicationInfo.splitSourceDirs);
                if(packageSignerMatches(current)&&record.ref.equals(packageRef)&&record.commit.equals(row.getString("commit"))
                        &&record.remote.equals(row.getString("origin")))return;
                throw new SecurityException("Package was installed by another operation with different content; retry");
            }catch(PackageManager.NameNotFoundException absent) { }
            else try {
                PackageInfo current=getPackageManager().getPackageInfo(packageName,PackageManager.GET_SIGNING_CERTIFICATES);
                if(!packageSignerMatches(current)||current.getLongVersionCode()!=installed.getLongVersionCode()
                        ||current.lastUpdateTime!=installed.lastUpdateTime)
                    throw new SecurityException("Package changed during planning; retry");
            }catch(PackageManager.NameNotFoundException absent) { throw new SecurityException("Package disappeared during planning; retry",absent); }
            installStubPackageLocked(op,packageRef,artifacts,installed,launchable,mountPoint);
        }
    }

    private void installStubPackageLocked(Operation op,String packageRef,File artifacts,PackageInfo installed,
            boolean launchable,String mountPoint) throws Exception {
        JSONObject packageRow=InstallTransaction.row(op.stagedManifest,packageRef);
        String pkg=packageFor(packageRef),remote=packageRow.getString("origin");
        if(op.stagedMetadata==null)
            throw new IllegalStateException("Staged Flatpak metadata unavailable");
        if(artifacts==null)throw new IllegalStateException("Staged object packs unavailable");
        WorkDirs.require(artifacts);
        File work=op.stubWork; if(!work.isDirectory()&&!work.mkdirs())throw new java.io.IOException("Cannot create stub scratch directory");
        File objects=new File(artifacts,"objects"),base=new File(artifacts,"base.apk");
        java.util.List<String> requiredPackages=new java.util.ArrayList<>();
        java.util.Map<String,String> dependencyPackages=new java.util.TreeMap<>();
        org.json.JSONArray declared=packageRow.getJSONArray("dependencies");
        File installation=InstallTransaction.installationFor(this,op.stagingId,packageRow);
        for(int i=0;i<declared.length();i++) {
            String ref=declared.getString(i);JSONObject dependency=InstallTransaction.row(op.stagedManifest,ref);
            org.matonos.linux.stub.PackageMounts.Entry mount=org.matonos.linux.stub.PackageMounts.entry(ref,dependency.getString("mount"));
            requiredPackages.add(mount.packageName);dependencyPackages.put(ref,mount.packageName+">"+mount.mountPoint);
        }
        // Launcher data is derived here, never supplied by a caller. The
        // deployment export wins; apply_extra may override it below; otherwise
        // the remote's appstream is read by this subsystem app, then app id.
        String appId=appId(packageRef);
        File deployment=org.matonos.linux.stub.DeployManifest.stagedDeployment(installation,new JSONObject().put("appRef",packageRef).put("appCommit",commitFor(op,packageRef)));
        org.matonos.linux.stub.LauncherData launcher=org.matonos.linux.stub.LauncherData.resolve(
                deployment,appId,null,launchable&&"flathub".equals(remote)?new org.matonos.linux.stub.LauncherData.Flathub():null);
        byte[] baseIcon=launcher.icon!=null?launcher.icon:fallbackIcon();
        org.matonos.linuxhost.stubgen.StubGenerator.generatePackage(work,packageRef,org.matonos.linuxhost.stubgen.RemoteOrigin.parse(remote).name,launcher.label,baseIcon,
                StubGenerator.permissionsForMetadata(org.matonos.linux.stub.DeployManifest.stagedMetadata(installation,new JSONObject().put("appRef",packageRef).put("appCommit",commitFor(op,packageRef)))),pkg,base,objects,requiredPackages,dependencyPackages,launchable,mountPoint);
        boolean full=installed==null || installed.getLongVersionCode()!=StubGenerator.STUB_VERSION_CODE ||
                !org.matonos.linuxhost.stubgen.PackSessionPlan.sameBase(base,new File(installed.applicationInfo.sourceDir));
        java.util.List<String> prior=installed==null || installed.splitNames==null?java.util.Collections.emptyList():java.util.Arrays.asList(installed.splitNames);
        int revision=org.matonos.linuxhost.stubgen.PackSessionPlan.nextRevision(
                installed==null?0:installed.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,
                installed==null?0:installed.baseRevisionCode,installed==null?null:installed.splitRevisionCodes);
        boolean hasExtra=false;
        for(org.matonos.linuxhost.stubgen.ObjectPacks.Entry row:org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(new File(objects,"objects.index")))
            if(row.path.equals("/extra")||row.path.startsWith("/extra/")){hasExtra=true;break;}
        java.util.List<String> desired=new java.util.ArrayList<>(),available=new java.util.ArrayList<>();
        String index=StubGenerator.indexSplitName(objects);desired.add(index);
        if(full || !prior.contains(index)) {
            StubGenerator.generateIndex(work,pkg,objects,new File(artifacts,index+".apk"),revision);available.add(index);
        }
        addErofsLayerSplits(op,pkg,work,objects,artifacts,installed,prior,full,revision,desired,available);
        if(launchable)for(String split:prior)if(split.startsWith(StubGenerator.LAUNCHER_SPLIT_PREFIX))desired.add(split);
        // Everything needed after this point is now in the pack artifacts,
        // generated APKs, or the captured launcher values. Drop this package's
        // downloaded Flatpak repo and any stale pack-repo from an earlier attempt
        // before PackageInstaller sessions; other rows have separate roots.
        File stagingRoot=new File(new File(WorkDirs.root(this),"flatpak-staging"),op.stagingId);
        long beforeRelease=InstallTransaction.allocatedBytes(stagingRoot);
        WorkDirs.remove(installation);
        WorkDirs.remove(new File(installation.getParentFile(),"pack-repo"));
        long afterRelease=InstallTransaction.allocatedBytes(stagingRoot);
        Log.i(TAG,"Released Flatpak and pack repositories for "+packageRef+": allocated operation bytes "+
                beforeRelease+" -> "+afterRelease+"; available="+stagingRoot.getUsableSpace());
        runPackSession(op,pkg,artifacts,plan(installed!=null,
                installed==null?0:installed.getLongVersionCode(),prior,desired,available,full));
        final boolean applyExtra=hasExtra;
        op.finalizers.add(()->{
            try {
                File applied=applyExtra?org.matonos.linux.stub.ExtraInstall.apply(this,pkg,new File(artifacts,"extra-output")):null;
                PackageInfo after=getPackageManager().getPackageInfo(pkg,0);
                java.util.List<String> installedNow=after.splitNames==null?java.util.Collections.emptyList():java.util.Arrays.asList(after.splitNames);
                java.util.List<String> desired2=new java.util.ArrayList<>(),available2=new java.util.ArrayList<>();
                desired2.add(index);
                addLayerSplitNames(objects,desired2);
                if(applied!=null){String split=applied.getName().replace(".apk","");desired2.add(split);available2.add(split);}
                if(launchable)addLauncherSplit(op,pkg,work,artifacts,revision,launcher.label,launcher.mimeTypes,baseIcon,installedNow,false,desired2,available2);
                runPackSession(op,pkg,artifacts,plan(true,after.getLongVersionCode(),installedNow,desired2,available2,false));
            }catch(Exception failure){if(installed==null)removePackage(pkg);throw failure;}
        });
    }

    /** Adds each signed EROFS pack layer, reusing unchanged installed carriers. */
    private void addErofsLayerSplits(Operation op,String pkg,File work,File objects,File artifacts,PackageInfo installed,java.util.List<String> prior,
            boolean full,int revision,java.util.List<String> desired,java.util.List<String> available) throws Exception {
        if(op.cancelled)throw new java.io.IOException("Install cancelled");
        org.matonos.linuxhost.stubgen.ErofsLayers.Index index=org.matonos.linuxhost.stubgen.ErofsLayers.read(
                java.nio.file.Files.readAllBytes(new File(objects,"layers.index").toPath()));
        for(org.matonos.linuxhost.stubgen.ErofsLayers.Layer layer:index.layers) {
            String split=layer.split;desired.add(split);File destination=new File(artifacts,split+".apk");
            if(installed!=null&&prior.contains(split)&&installed.getLongVersionCode()==StubGenerator.STUB_VERSION_CODE) {
                if(full) {
                    int position=prior.indexOf(split);
                    java.nio.file.Files.copy(java.nio.file.Path.of(installed.applicationInfo.splitSourceDirs[position]),destination.toPath());available.add(split);
                }
            } else {
                File image=new File(objects,"pack-"+layer.sha256+".erofs");
                StubGenerator.generateErofsLayerPack(work,pkg,image,destination,revision);available.add(split);
            }
        }
    }

    private static void addLayerSplitNames(File objects,java.util.List<String> desired) throws Exception {
        org.matonos.linuxhost.stubgen.ErofsLayers.Index index=org.matonos.linuxhost.stubgen.ErofsLayers.read(
                java.nio.file.Files.readAllBytes(new File(objects,"layers.index").toPath()));
        for(org.matonos.linuxhost.stubgen.ErofsLayers.Layer layer:index.layers)if(!desired.contains(layer.split))desired.add(layer.split);
    }

    /** Adds the code-free launcher split unless the same one is installed. */
    private void addLauncherSplit(Operation op,String pkg,File work,File artifacts,int revision,String label,
            java.util.List<String> mimeTypes,byte[] icon,java.util.List<String> prior,boolean full,
            java.util.List<String> desired,java.util.List<String> available) throws Exception {
        String split=StubGenerator.launcherSplitName(label,mimeTypes,icon);
        if(desired.contains(split))return;
        desired.add(split);
        if(full || !prior.contains(split)) {
            StubGenerator.generateLauncher(work,pkg,label,mimeTypes,icon,new File(artifacts,split+".apk"),revision);
            available.add(split);
        }
    }

    private static org.matonos.linuxhost.stubgen.PackSessionPlan plan(boolean installed,long installedVersion,
            java.util.List<String> prior,java.util.List<String> desired,java.util.List<String> available,
            boolean full) throws Exception {
        return org.matonos.linuxhost.stubgen.PackSessionPlan.create(installed,installedVersion,
                StubGenerator.STUB_VERSION_CODE,prior,desired,available,full);
    }

    /** Commits one atomic session and blocks until its terminal result. */
    private Intent runPackSession(Operation op,String pkg,File artifacts,
            org.matonos.linuxhost.stubgen.PackSessionPlan plan) throws Exception {
        if(plan.write.isEmpty() && plan.remove.isEmpty()) return success();
        java.util.concurrent.CountDownLatch done=new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.atomic.AtomicReference<Intent> result=new java.util.concurrent.atomic.AtomicReference<>();
        org.matonos.linux.stub.PackInstaller installer=new org.matonos.linux.stub.PackInstaller(this,pkg,artifacts,
                ()->op.cancelled,intent->{result.set(intent);done.countDown();});
        op.packInstaller=installer;
        plan.submit(installer,()->op.cancelled);
        while(!done.await(1,java.util.concurrent.TimeUnit.SECONDS))if(op.cancelled)throw new java.io.IOException("Install cancelled");
        Intent completed=result.get();
        if(completed==null)throw new IllegalStateException("Package installer returned no result");
        if(completed.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)!=PackageInstaller.STATUS_SUCCESS)
            throw new IllegalStateException(completed.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
        return completed;
    }

    private static Intent success() {
        return new Intent().putExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_SUCCESS);
    }

    /** Removes a first install that failed after its content was committed. */
    private void removePackage(String pkg) {
        try {
            String action=getPackageName()+".PACK_ROLLBACK."+java.util.UUID.randomUUID();
            PendingIntent status=PendingIntent.getBroadcast(this,0,
                    new Intent(action).setPackage(getPackageName()),PendingIntent.FLAG_MUTABLE);
            android.content.BroadcastReceiver receiver=new android.content.BroadcastReceiver(){
                @Override public void onReceive(android.content.Context context,Intent intent){
                    try{unregisterReceiver(this);}catch(Exception ignored){}
                    status.cancel();
                }
            };
            registerReceiver(receiver,new android.content.IntentFilter(action),RECEIVER_NOT_EXPORTED);
            getPackageManager().getPackageInstaller().uninstall(pkg,status.getIntentSender());
        } catch(Exception error) { Log.w(TAG,"Cannot remove partially installed package "+pkg,error); }
    }

    /** The Flatpak app id inside a validated application ref. */
    private static String appId(String ref) {
        String[] parts=ref==null?null:ref.split("/",-1);
        return parts!=null && parts.length==4 && !parts[1].isEmpty()?parts[1]:"Linux application";
    }

    private void installDeclaredPackages(Operation op) throws Exception {
        org.json.JSONArray deployments=op.stagedManifest.getJSONArray("deployments");
        for(int i=0;i<deployments.length();i++) {
            JSONObject deployment=deployments.getJSONObject(i);String ref=deployment.getString("ref");
            if(ref.equals(op.ref))continue;
            String pkg=packageFor(ref);PackageInfo prior=null;
            try{prior=getPackageManager().getPackageInfo(pkg,PackageManager.GET_SIGNING_CERTIFICATES);}catch(PackageManager.NameNotFoundException missing){}
            if(deployment.getBoolean("reused")) {
                if(prior==null||!packageSignerMatches(prior))throw new SecurityException("Declared package disappeared or changed signer; retry");
                org.matonos.linuxhost.stubgen.DeploymentRecord record=org.matonos.linuxhost.stubgen.DeploymentRecord.installed(prior.splitNames,prior.applicationInfo.splitSourceDirs);
                if(!record.ref.equals(ref)||!record.commit.equals(deployment.getString("commit"))||!record.remote.equals(deployment.getString("origin")))
                    throw new SecurityException("Declared package changed during planning; retry");
                continue;
            }
            if(prior!=null) {
                org.matonos.linuxhost.stubgen.DeploymentRecord record=org.matonos.linuxhost.stubgen.DeploymentRecord.installed(prior.splitNames,prior.applicationInfo.splitSourceDirs);
                if(packageSignerMatches(prior)&&record.ref.equals(ref)&&record.commit.equals(deployment.getString("commit"))
                        &&record.remote.equals(deployment.getString("origin")))continue;
                throw new SecurityException("Declared package was installed during planning with different content; retry");
            }
            File artifacts=new File(op.packArtifacts,"dependency-"+i);
            if(!artifacts.isDirectory())throw new java.io.IOException("Missing prepared dependency packs for "+ref);
            installStubPackage(op,ref,artifacts,null,false,deployment.getString("mount"));
        }
    }

    /** Pack independent refs in parallel; package commits remain dependency ordered below. */
    private void buildAllPacks(Operation op,File root,JSONObject manifest) throws Exception {
        org.json.JSONArray rows=manifest.getJSONArray("deployments");
        java.util.List<org.matonos.linuxhost.stubgen.DependencyPlan.Node> nodes=new java.util.ArrayList<>();
        java.util.Map<String,Integer> positions=new java.util.HashMap<>();
        for(int i=0;i<rows.length();i++) {
            JSONObject row=rows.getJSONObject(i);String ref=row.getString("ref");positions.put(ref,i);
            org.matonos.linuxhost.stubgen.DependencyPlan.Node node=new org.matonos.linuxhost.stubgen.DependencyPlan.Node(ref,
                    row.getString("origin"),null,row.getBoolean("reused"));
            org.json.JSONArray dependencies=row.getJSONArray("dependencies");
            for(int d=0;d<dependencies.length();d++)node.dependencies.add(dependencies.getString(d));
            nodes.add(node);
        }
        org.matonos.linuxhost.stubgen.PlanExecutor.Outcome<Void> outcome=org.matonos.linuxhost.stubgen.PlanExecutor.execute(nodes,
                Runtime.getRuntime().availableProcessors(),node->{
            if(op.cancelled)throw new java.io.IOException("Install cancelled");
            if(node.reused)return null;
            int index=positions.get(node.ref);JSONObject row=rows.getJSONObject(index);
            File artifacts=node.ref.equals(op.ref)?op.packArtifacts:new File(op.packArtifacts,"dependency-"+index);
            if(!node.ref.equals(op.ref)&&!artifacts.mkdirs())throw new java.io.IOException("Cannot create package artifacts");
            File installation=InstallTransaction.installationFor(this,op.stagingId,row);
            PackageInfo installed=node.ref.equals(op.ref)?op.installed:null;
            org.matonos.linux.stub.StagedPacks.build(this,installation,InstallTransaction.packageManifest(manifest,node.ref),
                    new File(artifacts,"objects"),()->op.cancelled,installed);
            return null;
        });
        if(!outcome.completed.containsKey(op.ref)) {
            Throwable error=outcome.failed.get(op.ref);
            if(error instanceof Exception)throw (Exception)error;
            throw new java.io.IOException("Requested package packs could not be built because a dependency failed",error);
        }
    }

    private void finishStubInstall(Operation op,Intent intent) {
        try {
            if(op.completed.get())return;
            if(intent==null||intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)!=PackageInstaller.STATUS_SUCCESS)
                throw new IllegalStateException(intent==null?"Stub install failed":intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
            result(op,true,null);
        } catch(Exception e) {
            result(op,false,e.getMessage());
        }
    }

    private static void cleanArtifacts(Operation op) {
        File directory=op.packArtifacts;if(directory==null)return;
        try {
            java.nio.file.Files.walkFileTree(directory.toPath(),new java.nio.file.SimpleFileVisitor<java.nio.file.Path>() {
                @Override public java.nio.file.FileVisitResult visitFile(java.nio.file.Path path,java.nio.file.attribute.BasicFileAttributes attrs) throws java.io.IOException {
                    java.nio.file.Files.deleteIfExists(path);return java.nio.file.FileVisitResult.CONTINUE;
                }
                @Override public java.nio.file.FileVisitResult postVisitDirectory(java.nio.file.Path path,java.io.IOException error) throws java.io.IOException {
                    java.nio.file.Files.deleteIfExists(path);return java.nio.file.FileVisitResult.CONTINUE;
                }
            });
        } catch(Exception e){Log.w(TAG,"Cannot remove pack artifacts",e);}
    }

    private void uninstallStub(Operation op) throws Exception {
        String pkg=packageFor(op.ref);
        if(!op.ref.equals(authenticateStubRef(stubUid(op.ref))))throw new SecurityException("Untrusted installed stub");
        String action=getPackageName()+".PACK_UNINSTALL."+java.util.UUID.randomUUID();
        android.app.PendingIntent status=android.app.PendingIntent.getBroadcast(this,0,
                new Intent(action).setPackage(getPackageName()),android.app.PendingIntent.FLAG_MUTABLE);
        android.content.BroadcastReceiver receiver=new android.content.BroadcastReceiver() {
            @Override public void onReceive(android.content.Context context,Intent intent) {
                int code=intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE);
                if(code==PackageInstaller.STATUS_PENDING_USER_ACTION)code=PackageInstaller.STATUS_FAILURE;
                unregisterReceiver(this);status.cancel();
                result(op,code==PackageInstaller.STATUS_SUCCESS,code==PackageInstaller.STATUS_SUCCESS?null:intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
            }
        };
        registerReceiver(receiver,new android.content.IntentFilter(action),RECEIVER_NOT_EXPORTED);
        try {getPackageManager().getPackageInstaller().uninstall(pkg,status.getIntentSender());}
        catch(Exception error){unregisterReceiver(receiver);status.cancel();throw error;}
    }

    private byte[] fallbackIcon() {
        Bitmap bitmap=Bitmap.createBitmap(96,96,Bitmap.Config.ARGB_8888); Canvas canvas=new Canvas(bitmap); canvas.drawColor(Color.rgb(55,95,45));
        Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG); paint.setColor(Color.WHITE); paint.setTextSize(52); paint.setTextAlign(Paint.Align.CENTER); canvas.drawText("L",48,67,paint);
        java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream(); bitmap.compress(Bitmap.CompressFormat.PNG,100,out); bitmap.recycle(); return out.toByteArray();
    }

    private static String packageFor(String ref) throws Exception {
        return org.matonos.linux.stub.DeployManifest.packageForPublic(ref);
    }

    private void enforceQuery() {
        if(StorePermissionPolicy.query(checkCallingPermission(QUERY)==PackageManager.PERMISSION_GRANTED,
                checkCallingPermission(INSTALL)==PackageManager.PERMISSION_GRANTED)==StorePermissionPolicy.Decision.DENY)
            throw new SecurityException("QUERY_LINUX_APPS permission required");
    }

    private String callerLabel(int uid) {
        try {
            String[] packages=getPackageManager().getPackagesForUid(uid);
            if(packages!=null && packages.length>0)
                return String.valueOf(getPackageManager().getApplicationLabel(getPackageManager().getApplicationInfo(packages[0],0)));
        } catch(Exception ignored) { }
        return "An app";
    }

    private void result(Operation op, boolean ok, String error) {
        if(!op.completed.compareAndSet(false,true)) return;
        if(!ok && (error==null || error.isEmpty()))error="Store operation failed for "+op.ref;
        if(!ok)Log.e(TAG,"Store operation failed for "+op.ref+": "+error);
        // Deliver the terminal result even if cleanup itself fails.
        try {
            cleanArtifacts(op);
            InstallTransaction.cleanup(this,op.stagingId);
            if(op.stubWork!=null)WorkDirs.remove(op.stubWork);
            if(!ok && op.packInstaller!=null)op.packInstaller.abandon();
        } catch(Throwable cleanup) { Log.e(TAG,"Operation cleanup failed",cleanup); }
        finally {
            java.util.List<AutoCloseable> leases;
            synchronized(op.workLeases){leases=new java.util.ArrayList<>(op.workLeases);op.workLeases.clear();}
            for(AutoCloseable lease:leases)try{lease.close();}catch(Throwable e){Log.w(TAG,"Cannot release work lease",e);}
            try { op.callback.onResult(op.id,ok,error); }
            catch(Exception e) { Log.w(TAG,"Cannot deliver store result",e); }
        }
        operations.remove(operationKey(op.uid,op.id),op); pending.remove(operationKey(op.uid,op.id),op);
    }

    @Override public synchronized void onDestroy() { for(InstallTransaction transaction:new java.util.ArrayList<>(activeInstalls)){transaction.cancel();finishInstall(transaction);} instance=null; worker.shutdownNow(); super.onDestroy(); }
}
