package org.matonos.linux.stub;

import android.app.PendingIntent;
import android.content.*;
import android.content.pm.PackageInstaller;
import java.io.*;
import java.util.UUID;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import org.matonos.linuxhost.stubgen.PackSessionPlan;

/** One atomic session. Only a terminal PackageInstaller result releases artifacts. */
public final class PackInstaller implements PackSessionPlan.Installer {
    private final Context context;
    private final PackageInstaller installer;
    private final String pkg;
    private final File artifacts;
    private final BooleanSupplier cancelled;
    private final Consumer<Intent> callback;
    private PackageInstaller.Session session;
    private int id=-1;
    private PendingIntent status;
    private BroadcastReceiver receiver;
    private boolean terminal,committed;
    private volatile boolean cancelRequested;
    public PackInstaller(Context context,String pkg,File artifacts,BooleanSupplier cancelled,Consumer<Intent> callback) {
        this.context=context;this.pkg=pkg;this.artifacts=artifacts;this.cancelled=cancelled;this.callback=callback;
        installer=context.getPackageManager().getPackageInstaller();
    }
    @Override public synchronized void begin(PackSessionPlan.Mode mode) throws Exception {
        if(terminal || cancelRequested || cancelled.getAsBoolean())throw new IOException("Install cancelled");
        PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(mode==PackSessionPlan.Mode.FULL?
                PackageInstaller.SessionParams.MODE_FULL_INSTALL:PackageInstaller.SessionParams.MODE_INHERIT_EXISTING);
        params.setAppPackageName(pkg);
        params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
        id=installer.createSession(params);
        try {session=installer.openSession(id);}catch(Exception e){installer.abandonSession(id);id=-1;throw e;}
    }
    @Override public synchronized void write(String name) throws Exception {
        File apk=new File(artifacts,name);
        if(!name.matches("base\\.apk|(?:pack|index|extra|launcher)_[0-9a-f]{64}\\.apk") || !apk.isFile())throw new IOException("Invalid session artifact");
        try(InputStream input=new FileInputStream(apk);OutputStream output=session.openWrite(name,0,apk.length())) {
            byte[] buffer=new byte[32768];int n;
            while((n=input.read(buffer))!=-1) {
                if(terminal || cancelRequested || cancelled.getAsBoolean())throw new IOException("Install cancelled");
                output.write(buffer,0,n);
            }
            session.fsync(output);
        }
        // PackageInstaller has its durable copy now. Releasing this staged APK
        // before writing the next split keeps the session from duplicating the
        // entire staged split set on disk.
        if(!apk.delete() && apk.exists())throw new IOException("Cannot release staged session artifact: "+name);
    }
    @Override public synchronized void remove(String split) throws Exception {session.removeSplit(split);}
    @Override public synchronized void commit() throws Exception {
        if(terminal || cancelRequested || cancelled.getAsBoolean())throw new IOException("Install cancelled");
        String action=context.getPackageName()+".PACK_INSTALL."+UUID.randomUUID();
        receiver=new BroadcastReceiver(){@Override public void onReceive(Context c,Intent intent){
            int code=intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE);
            if(code==PackageInstaller.STATUS_PENDING_USER_ACTION) {
                code=PackageInstaller.STATUS_FAILURE;
                intent.putExtra(PackageInstaller.EXTRA_STATUS,code);
                intent.putExtra(PackageInstaller.EXTRA_STATUS_MESSAGE,"PackageInstaller requires user action");
            }
            synchronized(PackInstaller.this) {
                if(terminal)return;
                terminal=true;
                if(code!=PackageInstaller.STATUS_SUCCESS && id>=0)try{installer.abandonSession(id);}catch(Exception ignored){}
                release();
            }
            callback.accept(intent);
        }};
        context.registerReceiver(receiver,new IntentFilter(action),Context.RECEIVER_NOT_EXPORTED);
        status=PendingIntent.getBroadcast(context,id,new Intent(action).setPackage(context.getPackageName()),PendingIntent.FLAG_MUTABLE|PendingIntent.FLAG_UPDATE_CURRENT);
        session.commit(status.getIntentSender());committed=true;session.close();session=null;
    }
    /** Commit is the point of no return; do not report a committed install as cancelled. */
    public boolean cancelBeforeCommit() {
        cancelRequested=true;
        synchronized(this) {
            if(committed || terminal)return false;
            abandon();return true;
        }
    }
    @Override public synchronized void abandon() {
        if(terminal)return;
        if(id>=0)try{installer.abandonSession(id);}catch(Exception ignored){}
        terminal=true;release();
    }
    private void release() {
        if(session!=null){session.close();session=null;}
        if(receiver!=null){context.unregisterReceiver(receiver);receiver=null;}
        if(status!=null){status.cancel();status=null;}
    }
}
