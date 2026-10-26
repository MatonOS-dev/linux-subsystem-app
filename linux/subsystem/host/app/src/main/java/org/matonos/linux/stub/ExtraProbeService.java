package org.matonos.linux.stub;

import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.util.Log;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Map;

/** Short lived on-device proof that isolated services can run the proxy and bwrap from directory fds. */
public final class ExtraProbeService extends Service {
    private static final String TAG="MatonExtraProbe";
    private final IExtraProbe.Stub binder=new IExtraProbe.Stub() {
        @Override public void run(ParcelFileDescriptor usr, ParcelFileDescriptor app) {
            int caller=Binder.getCallingUid();
            try {
                if(caller!=getPackageManager().getApplicationInfo("org.matonos.linux",0).uid)
                    throw new SecurityException("Probe accepts calls only from the subsystem app");
            } catch(PackageManager.NameNotFoundException missing) { throw new SecurityException(missing); }
            Thread worker=new Thread(()->execute(usr,app),"isolated-extra-probe");worker.setDaemon(true);worker.start();
        }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }

    private void execute(ParcelFileDescriptor usr,ParcelFileDescriptor app) {
        try( ParcelFileDescriptor keepUsr=usr; ParcelFileDescriptor keepApp=app ) {
            int uid=Process.myUid();
            int appUid=getApplicationInfo().uid;
            Log.i(TAG,"isolated uid="+uid+" appUid="+appUid+" ownPid="+Process.myPid()+" fds="+usr.getFd()+","+app.getFd());
            if(uid==appUid) throw new SecurityException("Service is running as the subsystem UID, not an isolated UID");
            String usrPath="/proc/"+Process.myPid()+"/fd/"+usr.getFd();
            String appPath="/proc/"+Process.myPid()+"/fd/"+app.getFd();
            ArrayList<String> command=new ArrayList<>();
            command.add(LoaderCommand.loader(this).getAbsolutePath());
            command.add("--parent-pid");command.add(Integer.toString(Process.myPid()));
            command.add("--map-ro");command.add(usrPath);command.add("/usr");
            command.add("--map");command.add(appPath);command.add("/app");
            command.add(LoaderCommand.tool(this,LoaderCommand.FLATPAK_BWRAP).getAbsolutePath());
            command.add("--unshare-net");command.add("--ro-bind");command.add("/usr");command.add("/usr");
            command.add("--ro-bind");command.add("/app");command.add("/app");
            command.add("--ro-bind");command.add("/system/bin/toybox");command.add("/bin/toybox");
            command.add("--proc");command.add("/proc");command.add("--dev");command.add("/dev");
            command.add("/bin/toybox");command.add("true");
            Log.i(TAG,"starting v3 proxy + bwrap with fd-backed /usr and /app; network namespace requested");
            ProcessBuilder builder=new ProcessBuilder(command).redirectErrorStream(true);
            Map<String,String> env=builder.environment();env.clear();
            String nativeDir=LoaderCommand.nativeLibraryDir(this).getAbsolutePath();
            env.put("PATH",nativeDir+":/system/bin");env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");
            env.put("LC_ALL","C");env.put("HOME","/");env.put("TMPDIR","/tmp");
            java.lang.Process child=builder.start();
            try(BufferedReader output=new BufferedReader(new InputStreamReader(child.getInputStream()))) {
                String line;while((line=output.readLine())!=null)Log.i(TAG,"child: "+line);
            }
            int status=child.waitFor();
            Log.i(TAG,"proxy+bwrap exit="+status);
        } catch(Throwable error) { Log.e(TAG,"isolated proxy+bwrap probe failed",error); }
    }
}
