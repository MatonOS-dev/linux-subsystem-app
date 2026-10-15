package org.matonos.linux;

import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.util.Log;
import org.json.JSONObject;
import org.matonos.systembridge.ICompositorInstallCallback;
import org.matonos.systembridge.ILinuxd;
import org.matonos.linux.stub.InstallTransaction;
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
        @Override public String listRemotes() { enforceQuery(); return call("list_remotes", new JSONObject()); }
        @Override public void addRemote(String name, String url, String id, ILinuxAppStoreCallback cb) {
            submit("addRemote", null, null, name, url, id, cb);
        }
        @Override public void removeRemote(String name, String id, ILinuxAppStoreCallback cb) {
            submit("removeRemote", null, null, name, null, id, cb);
        }
        @Override public void cancel(String id) {
            int uid=Binder.getCallingUid(); Operation op=operations.get(id);
            if(op==null || !StorePermissionPolicy.ownsOperation(op.uid,uid))
                throw new SecurityException("Operation is not owned by caller");
            op.cancelled=true;
            if(op.transaction!=null) op.transaction.cancel();
            result(op,false,"Operation cancelled");
        }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }

    @Override public void onCreate() { super.onCreate(); instance=this; }

    static void confirmPending(String id, boolean accepted) {
        LinuxAppStoreService service=instance;
        if(service!=null) service.confirm(id,accepted);
    }

    private void submit(String kind, String ref, String remote, String name, String url,
            String id, ILinuxAppStoreCallback callback) {
        int uid=Binder.getCallingUid();
        if (id==null || !id.matches("[A-Za-z0-9_-]{1,64}") || callback==null)
            throw new IllegalArgumentException("Invalid operation request");
        boolean privileged=checkCallingPermission(INSTALL)==PackageManager.PERMISSION_GRANTED;
        Operation op=new Operation(uid,id,kind,ref,remote,name,url,callback);
        if(operations.putIfAbsent(id,op)!=null) throw new IllegalArgumentException("Operation ID already active");
        boolean needsConfirm=kind.equals("addRemote") || kind.equals("removeRemote")
                ? StorePermissionPolicy.remoteMutation(privileged)==StorePermissionPolicy.Decision.CONFIRM
                : StorePermissionPolicy.packageOperation(privileged)==StorePermissionPolicy.Decision.CONFIRM;
        if(needsConfirm) {
            pending.put(id,op);
            Intent intent=new Intent(this,ConfirmInstallActivity.class)
                    .putExtra("operationId",id).putExtra("kind",kind).putExtra("ref",ref)
                    .putExtra("remote",remote).putExtra("name",name).putExtra("url",url)
                    .putExtra("caller",callerLabel(uid)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
        } else worker.execute(()->execute(id,op));
    }

    /** Called only by the non-exported confirmation activity after explicit consent. */
    void confirm(String id, boolean accepted) {
        Operation op=pending.remove(id);
        if(op==null) return;
        if(!accepted) { result(op,false,"Request declined"); return; }
        worker.execute(()->execute(id,op));
    }

    private void execute(String id, Operation op) {
        try {
            if(op.cancelled) return;
            JSONObject args=new JSONObject(); String command;
            switch(op.kind) {
                case "install": case "update": runStagedInstall(id,op); return;
                case "uninstall": command="uninstall"; put(args,"ref",op.ref); put(args,"uid",stubUid(op.ref)); put(args,"deleteData",false); put(args,"operationId",id); break;
                case "addRemote":
                    if(!"flathub".equals(op.name) || (op.url!=null && !op.url.isEmpty() && !"https://dl.flathub.org/repo/flathub.flatpakrepo".equals(op.url)))
                        throw new IllegalArgumentException("Only the configured Flathub remote is supported");
                    command="add_flathub"; break;
                case "removeRemote": command="remove_repo"; put(args,"remote",op.name); break;
                default: throw new IllegalArgumentException("Unknown operation");
            }
            JSONObject reply=new JSONObject(linuxd().call(command,args.toString()));
            boolean ok=reply.optBoolean("ok",false);
            result(op,ok,ok?null:reply.optString("error","Store operation failed"));
        } catch(Exception error) {
            Log.w(TAG,"Store operation failed",error);
            result(op,false,error.getMessage()==null?error.toString():error.getMessage());
        }
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
                    JSONObject args=new JSONObject(); put(args,"operationId",operationId);
                    args.put("manifest",new JSONObject(manifest));
                    JSONObject reply=new JSONObject(linuxd().call("verify_staged_install",args.toString()));
                    boolean ok=reply.optBoolean("ok",false);
                    result(op,ok,ok?null:reply.optString("error","Staged install verification failed"));
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
        operations.remove(op.id,op);
    }

    @Override public void onDestroy() { instance=null; worker.shutdownNow(); super.onDestroy(); }
}
