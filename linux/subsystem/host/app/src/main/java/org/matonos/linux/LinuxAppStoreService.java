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
import org.matonos.systembridge.ICompositorInstallCallback;
import org.matonos.systembridge.ILinuxd;
import org.matonos.linux.stub.InstallTransaction;
import org.matonos.linuxhost.stubgen.StubGenerator;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
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
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Map<String, Operation> operations = new ConcurrentHashMap<>();
    private final Map<String, Operation> pending = new ConcurrentHashMap<>();
    private static volatile LinuxAppStoreService instance;
    private static final class Operation {
        final int uid;
        final String id, kind, ref, remote, name, url;
        final ILinuxAppStoreCallback callback;
        volatile boolean cancelled;
        volatile org.matonos.systembridge.ICompositorInstall installService;
        volatile String stagedMetadata, stagedDesktop;
        volatile File packArtifacts;
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
                android.content.pm.ActivityInfo stubInfo=getPackageManager().getActivityInfo(new ComponentName(pkg,StubGenerator.HOST_ACTIVITY),PackageManager.GET_META_DATA);
                String recorded=stubInfo.metaData==null?null:stubInfo.metaData.getString(StubGenerator.APP_REF_META);
                if(!ref.equals(recorded))throw new SecurityException("Requested repair ref differs from signed stub metadata");
                info=getPackageManager().getPackageInfo(pkg,PackageManager.GET_SIGNING_CERTIFICATES);
                if(info.signingInfo==null||info.signingInfo.hasMultipleSigners())throw new SecurityException("Invalid stub signer");
                android.content.pm.Signature[] signers=info.signingInfo.getApkContentsSigners();byte[] expected=StubGenerator.getExistingSigningCertificate();
                byte[][] encoded=signers==null?null:new byte[][]{signers[0].toByteArray()};
                if(!StubSelfInstallIdentity.repairCallerMatches(ref,recorded,uid,packages,pkg,info.applicationInfo.uid,encoded,expected))throw new SecurityException("Repair caller is not a trusted stub for this ref");
            }catch(SecurityException e){throw e;}catch(Exception e){throw new SecurityException("Cannot authenticate stub repair",e);}
            Operation op=new Operation(uid,id,"repair",ref,"flathub",null,null,cb);String key=operationKey(uid,id);
            if(operations.putIfAbsent(key,op)!=null)throw new IllegalArgumentException("Operation ID already active for caller");
            worker.execute(()->execute(id,op));
        }
        @Override public void uninstall(String ref, String id, ILinuxAppStoreCallback cb) {
            submit("uninstall", ref, null, null, null, id, cb);
        }
        @Override public String listInstalled() { enforceQuery(); return call("list_installed", args("userId", android.os.Process.myUid()/100000)); }
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
            if(op.installService!=null)try{op.installService.cancelInstall(op.id);}catch(Exception ignored){}
            result(op,false,"Operation cancelled");
        }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }

    @Override public void onCreate() { super.onCreate(); instance=this; }

    @Override public int onStartCommand(Intent intent,int flags,int startId){
        if(intent!=null&&"org.matonos.linux.CLEAN_UNUSED_RUNTIMES".equals(intent.getAction())){
            worker.execute(()->{cleanupUnusedRuntimePackages();stopSelf(startId);});return START_NOT_STICKY;
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
            if (kind.equals("install")) remote="flathub";
        }
        Operation op=new Operation(uid,id,kind,ref,remote,name,url,callback);
        String key=operationKey(uid,id);
        if(operations.putIfAbsent(key,op)!=null) throw new IllegalArgumentException("Operation ID already active for caller");
        boolean needsConfirm=(kind.equals("addRemote") || kind.equals("removeRemote")
                ? StorePermissionPolicy.remoteMutation(privileged)==StorePermissionPolicy.Decision.CONFIRM
                : StorePermissionPolicy.packageOperation(privileged)==StorePermissionPolicy.Decision.CONFIRM);
        if(needsConfirm) {
            pending.put(key,op);
            Intent intent=new Intent(this,ConfirmInstallActivity.class)
                    .putExtra("operationId",id).putExtra("kind",kind).putExtra("ref",ref)
                    .putExtra("remote",remote).putExtra("name",name).putExtra("url",url)
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
        cleanupUnusedRuntimePackages();
        final String remote="update".equals(op.kind)||"repair".equals(op.kind)?remoteFor(op.ref):op.remote;
        JSONObject begin=new JSONObject(); put(begin,"operationId",id); put(begin,"ref",op.ref);
        put(begin,"remote",remote); put(begin,"compositorUid",android.os.Process.myUid());
        JSONObject beginReply=new JSONObject(linuxd().call("begin_staged_install",begin.toString()));
        if(!beginReply.optBoolean("ok",false)) throw new IllegalStateException(beginReply.optString("error","Cannot begin staged install"));
        ICompositorInstallCallback stagedCallback=new ICompositorInstallCallback.Stub() {
            @Override public void onProgress(String operationId,String phase,int percent,String line) {
                try { op.callback.onProgress(operationId,phase,percent,-1,-1); }
                catch(Exception e) { Log.w(TAG,"Cannot deliver install progress",e); }
            }
            @Override public void onComplete(String operationId,String manifest) {
                try {
                    JSONObject staged = new JSONObject(manifest);
                    op.stagedManifest=staged;
                    File installation=new File(new File("/data/linux-app/staging", Integer.toString(android.os.Process.myUid())),
                            operationId + "/installation");
                    String[] details = org.matonos.linux.stub.DeployManifest.stagedDetails(installation, staged);
                    op.stagedMetadata = details[0];
                    op.stagedDesktop = details[1];
                    File scratch=new File(getCacheDir(),"flatpak-pack-artifacts");
                    if(!scratch.isDirectory()&&!scratch.mkdirs())throw new java.io.IOException("Cannot create pack scratch");
                    op.packArtifacts=java.nio.file.Files.createTempDirectory(scratch.toPath(),"stage-").toFile();
                    try {op.installed=getPackageManager().getPackageInfo(packageFor(op.ref),0);}
                    catch(PackageManager.NameNotFoundException absent){op.installed=null;}
                    org.matonos.linux.stub.StagedPacks.build(LinuxAppStoreService.this,installation,staged,
                            new File(op.packArtifacts,"objects"),()->op.cancelled,op.installed);
                    if(op.cancelled)throw new java.io.IOException("Install cancelled");
                    JSONObject args=new JSONObject(); put(args,"operationId",operationId);
                    args.put("manifest",staged);
                    JSONObject reply=new JSONObject(linuxd().call("verify_staged_install",args.toString()));
                    if(!reply.optBoolean("ok",false)) throw new IllegalStateException(reply.optString("error","Staged install verification failed"));
                    installRuntimePackages(op,installation);
                    installFinalStub(op);
                } catch(Exception e) {
                    try {linuxd().call("abort_staged_install",args("operationId",operationId).toString());}catch(Exception ignored){}
                    result(op,false,e.getMessage());
                } finally {if(op.cancelled)cleanArtifacts(op);}
            }
            @Override public void onFailed(String operationId,String error) {
                try { linuxd().call("abort_staged_install",args("operationId",operationId).toString()); }
                catch(Exception ignored) { }
                result(op,false,error);
            }
        };
        android.content.ServiceConnection connection=new android.content.ServiceConnection(){
            @Override public void onServiceConnected(ComponentName name,IBinder service){try{op.installService=org.matonos.systembridge.ICompositorInstall.Stub.asInterface(service);op.installService.startInstall(stagedCallback,id,op.ref,remote);}catch(Exception error){result(op,false,error.getMessage());}finally{unbindService(this);}}
            @Override public void onServiceDisconnected(ComponentName name){result(op,false,"Flatpak installer disconnected");}
        };
        if(!bindService(new Intent(this,org.matonos.linux.stub.CompositorInstallService.class),connection,Context.BIND_AUTO_CREATE))throw new IllegalStateException("Cannot connect to Flatpak installer");
    }

    private void cleanupUnusedRuntimePackages() {
        try {
            java.util.Set<String> needed=new java.util.HashSet<>();
            for(PackageInfo installed:getPackageManager().getInstalledPackages(PackageManager.GET_META_DATA|PackageManager.GET_SIGNING_CERTIFICATES)) {
                if(installed.applicationInfo==null||installed.signingInfo==null||installed.signingInfo.hasMultipleSigners())continue;
                android.content.pm.Signature[] signers=installed.signingInfo.getApkContentsSigners();if(signers==null||signers.length!=1||!signers[0].equals(StubGenerator.getExistingSigningCertificate()))continue;
                android.content.pm.ActivityInfo entry;
                try{entry=getPackageManager().getActivityInfo(new ComponentName(installed.packageName,StubGenerator.HOST_ACTIVITY),PackageManager.GET_META_DATA|PackageManager.MATCH_DISABLED_COMPONENTS);}catch(Exception ignored){continue;}
                if(entry.metaData==null)continue;String map=entry.metaData.getString(StubGenerator.RUNTIME_PACKAGES_META);if(map!=null)for(String row:map.split(";")){String[] p=row.split(">",3);if(p.length==3)needed.add(p[1]);}
            }
            for(PackageInfo installed:getPackageManager().getInstalledPackages(PackageManager.GET_SIGNING_CERTIFICATES)) {
                if(installed.packageName==null||!installed.packageName.matches("flatpak\\.(?:runtime|app)\\..+\\.x[0-9a-f]{12}"))continue;
                if(needed.contains(installed.packageName)||!runtimeSignerMatches(installed))continue;
                String action=getPackageName()+".RUNTIME_CLEAN."+java.util.UUID.randomUUID();
                PendingIntent status=PendingIntent.getBroadcast(this,0,new Intent(action).setPackage(getPackageName()),PendingIntent.FLAG_MUTABLE);
                getPackageManager().getPackageInstaller().uninstall(installed.packageName,status.getIntentSender());
            }
        } catch(Exception error){Log.w(TAG,"Runtime cleanup scan failed",error);}
    }

    public static final class BootReceiver extends android.content.BroadcastReceiver {
        @Override public void onReceive(Context context,Intent intent){if(!Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction()))return;context.startService(new Intent(context,LinuxAppStoreService.class).setAction("org.matonos.linux.CLEAN_UNUSED_RUNTIMES"));}
    }


    private String remoteFor(String ref) throws Exception {
        JSONObject request=args("ref",ref); put(request,"uid",stubUid(ref));
        JSONObject info=new JSONObject(linuxd().call("installed_commit",request.toString()));
        if(!info.optBoolean("ok",false)) throw new IllegalStateException(info.optString("error","Installed app not found"));
        String[] fields=info.optString("output").split("\\t",-1);
        if(fields.length<2 || fields[0].isEmpty()) throw new IllegalStateException("Installed remote is unavailable");
        return fields[0];
    }

    private int stubUid(String ref) throws Exception {
        return getPackageManager().getApplicationInfo(packageFor(ref),0).uid;
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
            android.content.pm.ActivityInfo activity=manager.getActivityInfo(
                    new ComponentName(packages[0],org.matonos.linuxhost.stubgen.StubGenerator.HOST_ACTIVITY),
                    PackageManager.GET_META_DATA | PackageManager.MATCH_DISABLED_COMPONENTS);
            String ref=activity.metaData==null?null:activity.metaData.getString(
                    org.matonos.linuxhost.stubgen.StubGenerator.REF_META);
            if(!StubSelfInstallIdentity.validRef(ref)) throw new SecurityException("Stub has no valid Flatpak ref");
            return ref;
        } catch(SecurityException error) { throw error; }
        catch(Exception error) { throw new SecurityException("Cannot authenticate stub install request",error); }
    }

    private static String operationKey(int uid,String id) { return uid+":"+id; }

    private boolean runtimeSignerMatches(PackageInfo info){try{PackageInfo host=getPackageManager().getPackageInfo(getPackageName(),PackageManager.GET_SIGNING_CERTIFICATES);if(info.signingInfo==null||host.signingInfo==null||info.signingInfo.hasMultipleSigners()||host.signingInfo.hasMultipleSigners())return false;android.content.pm.Signature[] a=info.signingInfo.getApkContentsSigners(),b=host.signingInfo.getApkContentsSigners();return a!=null&&b!=null&&a.length==1&&b.length==1&&a[0].equals(b[0]);}catch(Exception e){return false;}}

    private void installFinalStub(Operation op) throws Exception {
        String pkg=packageFor(op.ref), remote="install".equals(op.kind)?op.remote:remoteFor(op.ref);
        if(op.stagedMetadata==null || op.stagedDesktop==null)
            throw new IllegalStateException("Staged Flatpak metadata unavailable");
        byte[] icon=fallbackIcon();
        File work=new File(getCacheDir(),"flatpak-stubs"); if(!work.isDirectory()&&!work.mkdirs())throw new java.io.IOException("Cannot create stub scratch directory");
        File desktop=new File(work,pkg+".desktop");
        try {
            try(FileOutputStream out=new FileOutputStream(desktop)){out.write(op.stagedDesktop.getBytes(StandardCharsets.UTF_8));}
            if(op.packArtifacts==null)throw new IllegalStateException("Staged object packs unavailable");
            File objects=new File(op.packArtifacts,"objects"),base=new File(op.packArtifacts,"base.apk");
            java.util.List<String> requiredPackages=new java.util.ArrayList<>();
            java.util.Map<String,String> runtimePackages=new java.util.TreeMap<>();
            org.json.JSONArray required=op.stagedManifest.optJSONArray("requiredPackages");
            java.util.Map<String,org.matonos.linux.stub.RuntimeMounts.Entry> resolved=new java.util.HashMap<>();
            org.json.JSONArray deployments=op.stagedManifest.getJSONArray("deployments");
            java.util.List<String> refs=new java.util.ArrayList<>(),commits=new java.util.ArrayList<>();
            for(int i=0;i<deployments.length();i++){JSONObject row=deployments.getJSONObject(i);refs.add(row.getString("ref"));commits.add(row.getString("commit"));}
            File installation=new File(new File("/data/linux-app/staging",Integer.toString(android.os.Process.myUid())),op.id+"/installation");
            for(org.matonos.linux.stub.RuntimeMounts.Entry entry:org.matonos.linux.stub.RuntimeMounts.resolve(refs,commits,op.ref,(ref,commit)->{
                File deploy=new File(installation,ref+"/"+commit),metadata=new File(deploy,"metadata");
                if(!deploy.getCanonicalPath().startsWith(installation.getCanonicalPath()+File.separator)||java.nio.file.Files.isSymbolicLink(metadata.toPath())||!metadata.isFile())throw new java.io.IOException("Cannot read verified Flatpak metadata for "+ref);
                byte[] bytes=java.nio.file.Files.readAllBytes(metadata.toPath());if(bytes.length==0||bytes.length>128*1024)throw new java.io.IOException("Invalid Flatpak metadata size");return new String(bytes,StandardCharsets.UTF_8);
            }))resolved.put(entry.ref,entry);
            if(required!=null)for(int i=0;i<required.length();i++){
                JSONObject row=required.getJSONObject(i);String ref=row.getString("ref");org.matonos.linux.stub.RuntimeMounts.Entry mount=resolved.get(ref);
                if(mount==null)throw new IllegalStateException("Runtime package mount was not resolved: "+ref);
                String name=mount.packageName;requiredPackages.add(name);runtimePackages.put(ref,name+">"+mount.mountPoint);
            }
            org.matonos.linuxhost.stubgen.StubGenerator.generateWithRuntimePackages(work,op.ref,remote,desktop,icon,
                    StubGenerator.permissionsForMetadata(op.stagedMetadata),pkg,base,null,objects,requiredPackages,runtimePackages);
            boolean full=op.installed==null || op.installed.getLongVersionCode()!=StubGenerator.STUB_VERSION_CODE ||
                    !org.matonos.linuxhost.stubgen.PackSessionPlan.sameBase(base,new File(op.installed.applicationInfo.sourceDir));
            java.util.List<String> prior=op.installed==null || op.installed.splitNames==null?java.util.Collections.emptyList():java.util.Arrays.asList(op.installed.splitNames);
            int revision=org.matonos.linuxhost.stubgen.PackSessionPlan.nextRevision(
                    op.installed==null?0:op.installed.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,
                    op.installed==null?0:op.installed.baseRevisionCode,op.installed==null?null:op.installed.splitRevisionCodes);
            java.util.List<String> desired=new java.util.ArrayList<>(),available=new java.util.ArrayList<>();
            String index=StubGenerator.indexSplitName(objects);desired.add(index);
            if(full || !prior.contains(index)) {
                StubGenerator.generateIndex(work,pkg,objects,new File(op.packArtifacts,index+".apk"),revision);available.add(index);
            }
            for(org.matonos.linuxhost.stubgen.ObjectPacks.Entry row:org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(new File(objects,"objects.index"))) {
                if(!row.kind.equals("file"))continue;
                String split="pack_"+row.pack.substring(5,69);
                if(desired.contains(split))continue;
                desired.add(split);
                File destination=new File(op.packArtifacts,split+".apk");
                if(prior.contains(split) && op.installed.getLongVersionCode()==StubGenerator.STUB_VERSION_CODE) {
                    if(full) {
                        int position=prior.indexOf(split);
                        java.nio.file.Files.copy(java.nio.file.Path.of(op.installed.applicationInfo.splitSourceDirs[position]),destination.toPath());
                        available.add(split);
                    }
                } else {
                    if(op.cancelled)throw new java.io.IOException("Install cancelled");
                    StubGenerator.generatePack(work,pkg,new File(objects,row.pack),destination,revision);available.add(split);
                }
            }
            org.matonos.linuxhost.stubgen.PackSessionPlan plan=org.matonos.linuxhost.stubgen.PackSessionPlan.create(
                    op.installed!=null,op.installed==null?0:op.installed.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,
                    prior,desired,available,full);
            if(plan.write.isEmpty() && plan.remove.isEmpty()) {
                finishStubInstall(op,remote,pkg,new Intent().putExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_SUCCESS));
                return;
            }
            op.packInstaller=new org.matonos.linux.stub.PackInstaller(this,pkg,op.packArtifacts,()->op.cancelled,
                    intent->worker.execute(()->finishStubInstall(op,remote,pkg,intent)));
            plan.submit(op.packInstaller,()->op.cancelled);
        } finally { desktop.delete(); }
    }

    private void installRuntimePackages(Operation op,File installation) throws Exception {
        org.json.JSONArray deployments=op.stagedManifest.optJSONArray("deployments");
        if(deployments==null)throw new IllegalStateException("Missing staged Flatpak deployments");
        java.util.List<String> refs=new java.util.ArrayList<>(),commits=new java.util.ArrayList<>();
        for(int i=0;i<deployments.length();i++){JSONObject row=deployments.getJSONObject(i);refs.add(row.getString("ref"));commits.add(row.getString("commit"));}
        java.util.Map<String,org.matonos.linux.stub.RuntimeMounts.Entry> mounts=new java.util.HashMap<>();
        for(org.matonos.linux.stub.RuntimeMounts.Entry entry:org.matonos.linux.stub.RuntimeMounts.resolve(refs,commits,op.ref,(ref,commit)->{
            File deployment=new File(installation,ref+"/"+commit);File file=new File(deployment,"metadata");
            if(!deployment.getCanonicalPath().startsWith(installation.getCanonicalPath()+File.separator)||java.nio.file.Files.isSymbolicLink(file.toPath())||!file.isFile())throw new java.io.IOException("Cannot read verified Flatpak metadata for "+ref);
            byte[] bytes=java.nio.file.Files.readAllBytes(file.toPath());if(bytes.length==0||bytes.length>128*1024)throw new java.io.IOException("Flatpak metadata size invalid");return new String(bytes,StandardCharsets.UTF_8);
        }))mounts.put(entry.ref,entry);
        for(int i=0;i<deployments.length();i++) {
            JSONObject deployment=deployments.getJSONObject(i);
            String ref=deployment.getString("ref");
            if(ref.equals(op.ref))continue;
            org.matonos.linux.stub.RuntimeMounts.Entry mount=mounts.get(ref);if(mount==null)continue;
            String pkg=mount.packageName;
            PackageInfo prior=null;
            try{prior=getPackageManager().getPackageInfo(pkg,0);}catch(PackageManager.NameNotFoundException ignored){}
            File runtimeDir=new File(op.packArtifacts,"runtime-"+i);
            if(!runtimeDir.isDirectory()&&!runtimeDir.mkdirs())throw new java.io.IOException("Cannot create runtime package artifacts");
            File objects=new File(runtimeDir,"objects");
            org.matonos.linux.stub.StagedPacks.buildRuntime(this,installation,ref,deployment.getString("commit"),objects,()->op.cancelled);
            File base=new File(runtimeDir,"base.apk");
            String mountPoint=mount.mountPoint;
            org.matonos.linuxhost.stubgen.StubGenerator.generateRuntime(runtimeDir,ref,pkg,base,objects,mountPoint);
            if(prior!=null&&!runtimeSignerMatches(prior))throw new SecurityException("Installed runtime package signer mismatch: "+pkg);
            boolean full=prior==null||prior.getLongVersionCode()!=StubGenerator.STUB_VERSION_CODE||
                    !org.matonos.linuxhost.stubgen.PackSessionPlan.sameBase(base,new File(prior.applicationInfo.sourceDir));
            java.util.List<String> priorSplits=prior==null||prior.splitNames==null?java.util.Collections.emptyList():java.util.Arrays.asList(prior.splitNames);
            int revision=org.matonos.linuxhost.stubgen.PackSessionPlan.nextRevision(prior==null?0:prior.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,
                    prior==null?0:prior.baseRevisionCode,prior==null?null:prior.splitRevisionCodes);
            java.util.List<String> desired=new java.util.ArrayList<>(),available=new java.util.ArrayList<>();
            String index=StubGenerator.indexSplitName(objects);desired.add(index);
            if(full||!priorSplits.contains(index)){StubGenerator.generateIndex(runtimeDir,pkg,objects,new File(runtimeDir,index+".apk"),revision);available.add(index);}
            for(org.matonos.linuxhost.stubgen.ObjectPacks.Entry row:org.matonos.linuxhost.stubgen.ObjectPacks.readIndex(new File(objects,"objects.index"))) {
                if(!row.kind.equals("file"))continue;
                String split="pack_"+row.pack.substring(5,69);
                if(desired.contains(split))continue;
                desired.add(split);
                if(!full&&priorSplits.contains(split))continue;
                File apk=new File(runtimeDir,split+".apk");
                StubGenerator.generatePack(runtimeDir,pkg,new File(objects,row.pack),apk,revision);available.add(split);
            }
            org.matonos.linuxhost.stubgen.PackSessionPlan plan=org.matonos.linuxhost.stubgen.PackSessionPlan.create(
                    prior!=null,prior==null?0:prior.getLongVersionCode(),StubGenerator.STUB_VERSION_CODE,
                    priorSplits,desired,available,full);
            java.util.concurrent.CountDownLatch done=new java.util.concurrent.CountDownLatch(1);
            java.util.concurrent.atomic.AtomicReference<Intent> result=new java.util.concurrent.atomic.AtomicReference<>();
            org.matonos.linux.stub.PackInstaller installer=new org.matonos.linux.stub.PackInstaller(this,pkg,runtimeDir,
                    ()->op.cancelled,intent->{result.set(intent);done.countDown();});
            plan.submit(installer,()->op.cancelled);
            if(plan.write.isEmpty()&&plan.remove.isEmpty())continue;
            while(!done.await(1,java.util.concurrent.TimeUnit.SECONDS))if(op.cancelled)throw new java.io.IOException("Install cancelled");
            Intent completed=result.get();
            if(completed==null||completed.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)!=PackageInstaller.STATUS_SUCCESS)
                throw new IllegalStateException("Could not install Flatpak runtime "+ref+": "+(completed==null?"no session result":completed.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)));
        }
    }

    private void finishStubInstall(Operation op,String remote,String pkg,Intent intent) {
        try {
            if(op.completed.get())return;
            if(intent==null||intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)!=PackageInstaller.STATUS_SUCCESS)
                throw new IllegalStateException(intent==null?"Stub install failed":intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
            JSONObject bind=new JSONObject(); put(bind,"ref",op.ref); put(bind,"remote",remote); put(bind,"operationId",op.id); put(bind,"uid",stubUid(op.ref)); put(bind,"packageName",pkg);
            JSONObject bound=new JSONObject(linuxd().call("bind_pending",bind.toString()));
            if(!bound.optBoolean("ok"))throw new IllegalStateException(bound.optString("error","Stub binding failed"));
            linuxd().call("install_complete",new JSONObject().put("ref",op.ref).put("remote",remote).put("operationId",op.id).toString());
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
        String pkg=packageFor(op.ref); int uid=stubUid(op.ref);
        JSONObject args=args("ref",op.ref); put(args,"uid",uid); put(args,"deleteData",false); put(args,"operationId",op.id);
        JSONObject reply=new JSONObject(linuxd().call("uninstall",args.toString()));
        if(!reply.optBoolean("ok")) throw new IllegalStateException(reply.optString("error","Uninstall failed"));
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
        if(ref==null || !ref.matches("app/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))
            throw new IllegalArgumentException("Invalid application reference");
        String[] parts=ref.split("/"); String id=parts[1];
        boolean plain=parts[2].equals("x86_64")&&parts[3].equals("stable")&&id.length()<=200;
        StringBuilder safe=new StringBuilder();
        for(String segment:id.split("\\.",-1)) {
            String fixed=segment.replaceAll("[^A-Za-z0-9_]","_");
            if(fixed.isEmpty()||!Character.isLetter(fixed.charAt(0))) fixed="x"+fixed;
            if(!fixed.equals(segment)) plain=false;
            if(safe.length()>0) safe.append('.'); safe.append(fixed);
        }
        if(plain) return "flatpak."+id;
        String readable=safe.length()<=200?safe.toString():safe.substring(0,200).replaceAll("[._]+$","");
        byte[] digest=MessageDigest.getInstance("SHA-256").digest(ref.getBytes(StandardCharsets.UTF_8));
        StringBuilder suffix=new StringBuilder(".x");
        for(int i=0;i<4;i++) suffix.append(String.format(java.util.Locale.ROOT,"%02x",digest[i]&255));
        return "flatpak."+readable+suffix;
    }

    private void enforceQuery() {
        if(StorePermissionPolicy.query(checkCallingPermission(QUERY)==PackageManager.PERMISSION_GRANTED,
                checkCallingPermission(INSTALL)==PackageManager.PERMISSION_GRANTED)==StorePermissionPolicy.Decision.DENY)
            throw new SecurityException("QUERY_LINUX_APPS permission required");
    }

    private JSONObject args(String key, Object value) {
        JSONObject args=new JSONObject(); put(args,key,value); return args;
    }

    private void put(JSONObject args, String key, Object value) {
        try { args.put(key,value); }
        catch(org.json.JSONException e) { throw new IllegalArgumentException(e); }
    }

    private String call(String command, JSONObject args) {
        try { return linuxd().call(command,args.toString()); }
        catch(Exception e) { throw new IllegalStateException("linuxd unavailable",e); }
    }

    private ILinuxd linuxd() throws Exception {
        IBinder service=(IBinder)Class.forName("android.os.ServiceManager")
                .getMethod("checkService",String.class)
                .invoke(null,"org.matonos.systembridge.ILinuxd/default");
        if(service==null) throw new IllegalStateException("linuxd unavailable");
        return ILinuxd.Stub.asInterface(service);
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
        cleanArtifacts(op);
        if(!op.completed.compareAndSet(false,true)) return;
        if(!ok && op.packInstaller!=null)op.packInstaller.abandon();
        if(!ok && op.stagedMetadata!=null && ("install".equals(op.kind)||"update".equals(op.kind)||"repair".equals(op.kind))) {
            try {linuxd().call("install_failed",new JSONObject().put("ref",op.ref).put("remote","install".equals(op.kind)?op.remote:remoteFor(op.ref))
                    .put("operationId",op.id).put("error",error==null?"Install failed":error).toString());}
            catch(Exception ignored){}
        }
        try { op.callback.onResult(op.id,ok,error); }
        catch(Exception e) { Log.w(TAG,"Cannot deliver store result",e); }
        operations.remove(operationKey(op.uid,op.id),op); pending.remove(operationKey(op.uid,op.id),op);
    }

    @Override public void onDestroy() { instance=null; worker.shutdownNow(); super.onDestroy(); }
}
