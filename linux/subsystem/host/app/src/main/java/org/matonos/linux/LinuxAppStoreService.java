package org.matonos.linux;

import android.app.Service;
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
        volatile InstallTransaction transaction;
        volatile String stagedMetadata, stagedDesktop;
        volatile File packArtifacts;
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
        @Override public void uninstall(String ref, String id, ILinuxAppStoreCallback cb) {
            submit("uninstall", ref, null, null, null, id, cb);
        }
        @Override public String listInstalled() { enforceQuery(); return call("list_installed", args("userId", android.os.UserHandle.myUserId())); }
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
            op.cancelled=true;
            if(op.transaction!=null) op.transaction.cancel();
            result(op,false,"Operation cancelled");
        }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }

    @Override public void onCreate() { super.onCreate(); instance=this; }

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
        boolean needsConfirm=kind.equals("addRemote") || kind.equals("removeRemote")
                ? StorePermissionPolicy.remoteMutation(privileged)==StorePermissionPolicy.Decision.CONFIRM
                : StorePermissionPolicy.packageOperation(privileged)==StorePermissionPolicy.Decision.CONFIRM;
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
                case "install": case "update": runStagedInstall(id,op); return;
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
        String remote=op.remote;
        if("update".equals(op.kind)) remote=remoteFor(op.ref);
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
                    File installation=new File(new File("/data/linux-app/staging", Integer.toString(android.os.Process.myUid())),
                            operationId + "/installation");
                    String[] details = org.matonos.linux.stub.DeployManifest.stagedDetails(installation, staged);
                    op.stagedMetadata = details[0];
                    op.stagedDesktop = details[1];
                    File scratch=new File(getCacheDir(),"flatpak-pack-artifacts");
                    if(!scratch.isDirectory()&&!scratch.mkdirs())throw new java.io.IOException("Cannot create pack scratch");
                    op.packArtifacts=java.nio.file.Files.createTempDirectory(scratch.toPath(),"stage-").toFile();
                    org.matonos.linux.stub.StagedPacks.build(LinuxAppStoreService.this,installation,staged,
                            new File(op.packArtifacts,"objects"),()->op.cancelled);
                    if(op.cancelled)throw new java.io.IOException("Install cancelled");
                    JSONObject args=new JSONObject(); put(args,"operationId",operationId);
                    args.put("manifest",staged);
                    JSONObject reply=new JSONObject(linuxd().call("verify_staged_install",args.toString()));
                    if(!reply.optBoolean("ok",false)) throw new IllegalStateException(reply.optString("error","Staged install verification failed"));
                    installFinalStub(op);
                } catch(Exception e) { result(op,false,e.getMessage()); }
            }
            @Override public void onFailed(String operationId,String error) {
                try { linuxd().call("abort_staged_install",args("operationId",operationId).toString()); }
                catch(Exception ignored) { }
                result(op,false,error);
            }
        };
        op.transaction=InstallTransaction.create(this,stagedCallback,id,op.ref,remote);
        if(op.cancelled) { op.transaction.cancel(); return; }
        op.transaction.start(()-> { });
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

    private void installFinalStub(Operation op) throws Exception {
        String pkg=packageFor(op.ref), remote="install".equals(op.kind)?op.remote:remoteFor(op.ref);
        if(op.stagedMetadata==null || op.stagedDesktop==null)
            throw new IllegalStateException("Staged Flatpak metadata unavailable");
        byte[] icon=fallbackIcon();
        File work=new File(getCacheDir(),"flatpak-stubs"); if(!work.isDirectory()&&!work.mkdirs())throw new java.io.IOException("Cannot create stub scratch directory");
        File desktop=new File(work,pkg+".desktop"), apk=new File(work,pkg+".apk");
        try {
            try(FileOutputStream out=new FileOutputStream(desktop)){out.write(op.stagedDesktop.getBytes(StandardCharsets.UTF_8));}
            // Task 4b produces a complete reviewable set. Task 4c will switch
            // the PackageInstaller session below to these base + split files.
            if(op.packArtifacts==null)throw new IllegalStateException("Staged object packs unavailable");
            File objects=new File(op.packArtifacts,"objects");
            File packBase=new File(op.packArtifacts,"base.apk");
            StubGenerator.generate(work,op.ref,remote,desktop,icon,StubGenerator.permissionsForMetadata(op.stagedMetadata),pkg,packBase,null,null,objects);
            Log.i(TAG,"Pack artifact base: "+packBase);
            File[] packs=objects.listFiles((dir,name)->name.matches("pack-[0-9a-f]{64}\\.zip"));
            if(packs==null)throw new java.io.IOException("Missing object pack directory");
            java.util.Arrays.sort(packs,java.util.Comparator.comparing(File::getName));
            for(File pack:packs) {
                if(op.cancelled)throw new java.io.IOException("Install cancelled");
                File split=new File(op.packArtifacts,"pack_"+pack.getName().substring(5,69)+".apk");
                StubGenerator.generatePack(work,pkg,pack,split);
                Log.i(TAG,"Pack artifact split: "+split);
            }
            StubGenerator.generate(work,op.ref,remote,desktop,icon,StubGenerator.permissionsForMetadata(op.stagedMetadata),pkg,apk,null,null);
            PackageInstaller installer=getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(pkg); params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
            int sessionId=installer.createSession(params);
            try(PackageInstaller.Session session=installer.openSession(sessionId)) {
                try(FileInputStream input=new FileInputStream(apk); java.io.OutputStream output=session.openWrite("base.apk",0,apk.length())) {
                    byte[] buffer=new byte[32768]; int count; while((count=input.read(buffer))!=-1)output.write(buffer,0,count); session.fsync(output);
                }
                session.commit(new IntentSender((android.content.IIntentSender)new android.content.IIntentSender.Stub(){
                    @Override public void send(int code,Intent intent,String type,IBinder token,android.content.IIntentReceiver receiver,String permission,android.os.Bundle options) { worker.execute(()->finishStubInstall(op,remote,pkg,intent)); }
                }));
            } catch(Exception e){installer.abandonSession(sessionId); throw e;}
        } finally { desktop.delete(); apk.delete(); }
    }

    private void finishStubInstall(Operation op,String remote,String pkg,Intent intent) {
        try {
            if(intent==null||intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)!=PackageInstaller.STATUS_SUCCESS)
                throw new IllegalStateException(intent==null?"Stub install failed":intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
            JSONObject bind=new JSONObject(); put(bind,"ref",op.ref); put(bind,"remote",remote); put(bind,"operationId",op.id); put(bind,"uid",stubUid(op.ref)); put(bind,"packageName",pkg);
            JSONObject bound=new JSONObject(linuxd().call("bind_pending",bind.toString()));
            if(!bound.optBoolean("ok"))throw new IllegalStateException(bound.optString("error","Stub binding failed"));
            getPackageManager().setComponentEnabledSetting(new android.content.ComponentName(pkg,StubGenerator.HOST_ACTIVITY),PackageManager.COMPONENT_ENABLED_STATE_ENABLED,PackageManager.DONT_KILL_APP);
            linuxd().call("install_complete",new JSONObject().put("ref",op.ref).put("remote",remote).put("operationId",op.id).toString());
            result(op,true,null);
        } catch(Exception e) {
            try { linuxd().call("install_failed",new JSONObject().put("ref",op.ref).put("remote",remote).put("operationId",op.id).put("error",e.toString()).toString()); } catch(Exception ignored) { }
            result(op,false,e.getMessage());
        }
    }

    private void uninstallStub(Operation op) throws Exception {
        String pkg=packageFor(op.ref); int uid=stubUid(op.ref);
        JSONObject args=args("ref",op.ref); put(args,"uid",uid); put(args,"deleteData",false); put(args,"operationId",op.id);
        JSONObject reply=new JSONObject(linuxd().call("uninstall",args.toString()));
        if(!reply.optBoolean("ok")) throw new IllegalStateException(reply.optString("error","Uninstall failed"));
        getPackageManager().getPackageInstaller().uninstall(pkg,new IntentSender((android.content.IIntentSender)new android.content.IIntentSender.Stub(){
            @Override public void send(int code,Intent intent,String type,IBinder token,android.content.IIntentReceiver receiver,String permission,android.os.Bundle options) {
                int status=intent==null?PackageInstaller.STATUS_FAILURE:intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE);
                result(op,status==PackageInstaller.STATUS_SUCCESS,status==PackageInstaller.STATUS_SUCCESS?null:intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
            }
        }));
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
        if(!op.completed.compareAndSet(false,true)) return;
        try { op.callback.onResult(op.id,ok,error); }
        catch(Exception e) { Log.w(TAG,"Cannot deliver store result",e); }
        operations.remove(operationKey(op.uid,op.id),op); pending.remove(operationKey(op.uid,op.id),op);
    }

    @Override public void onDestroy() { instance=null; worker.shutdownNow(); super.onDestroy(); }
}
