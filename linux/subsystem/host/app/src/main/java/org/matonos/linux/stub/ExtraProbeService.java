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

/** Short lived on-device proof for regular-file mappings in an isolated service. */
public final class ExtraProbeService extends Service {
    private static final String TAG="MatonExtraProbe";
    private final IExtraProbe.Stub binder=new IExtraProbe.Stub() {
        @Override public void run(ParcelFileDescriptor file) {
            int caller=Binder.getCallingUid();
            try {
                if(caller!=getPackageManager().getApplicationInfo("org.matonos.linux",0).uid)
                    throw new SecurityException("Probe accepts calls only from the subsystem app");
            } catch(PackageManager.NameNotFoundException missing) { throw new SecurityException(missing); }
            Thread worker=new Thread(()->execute(file),"isolated-extra-probe");worker.setDaemon(true);worker.start();
        }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }

    private void execute(ParcelFileDescriptor file) {
        try(ParcelFileDescriptor keepFile=file) {
            int uid=Process.myUid();
            int appUid=getApplicationInfo().uid;
            Log.i(TAG,"isolated uid="+uid+" appUid="+appUid+" ownPid="+Process.myPid()+" fileFd="+file.getFd());
            if(uid==appUid) throw new SecurityException("Service is running as the subsystem UID, not an isolated UID");
            String filePath="/proc/"+Process.myPid()+"/fd/"+file.getFd();
            if(!new File(filePath).isFile())throw new IllegalArgumentException("Probe transport descriptor is not a regular file");
            probeWrite("app-cache",new File(getCacheDir(),"extra-probe-isolated-write"));
            probeWrite("data-local-tmp",new File("/data/local/tmp/extra-probe-isolated-write-"+Process.myPid()));
            probeWrite("dev-shm",new File("/dev/shm/extra-probe-isolated-write-"+Process.myPid()));
            probeWrite("tmp",new File("/tmp/extra-probe-isolated-write-"+Process.myPid()));
            ArrayList<String> command=new ArrayList<>();
            command.add(LoaderCommand.loader(this).getAbsolutePath());
            command.add("--parent-pid");command.add(Integer.toString(Process.myPid()));
            command.add(LoaderCommand.tool(this,LoaderCommand.FLATPAK_BWRAP).getAbsolutePath());
            command.add("--unshare-net");command.add("--dir");command.add("/usr");
            command.add("--dir");command.add("/app");
            command.add("--tmpfs");command.add("/tmp");
            command.add("--file");command.add("0");command.add("/tmp/probe.txt");
            command.add("--ro-bind");command.add("/system/bin/toybox");command.add("/bin/toybox");
            command.add("--proc");command.add("/proc");command.add("--dev");command.add("/dev");
            command.add("/bin/toybox");command.add("sh");command.add("-c");
            command.add("cat /tmp/probe.txt && echo scratch-write-ok > /tmp/write-test && cat /tmp/write-test");
            Log.i(TAG,"starting v3 proxy + bwrap with stdin-to-file stream; network namespace requested");
            ProcessBuilder builder=new ProcessBuilder(command).redirectErrorStream(true);
            Map<String,String> env=builder.environment();env.clear();
            String nativeDir=LoaderCommand.nativeLibraryDir(this).getAbsolutePath();
            env.put("PATH",nativeDir+":/system/bin");env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");
            env.put("LC_ALL","C");env.put("HOME","/");env.put("TMPDIR","/tmp");
            java.lang.Process child=builder.start();
            try(java.io.InputStream input=new java.io.FileInputStream(keepFile.getFileDescriptor());
                    java.io.OutputStream stream=child.getOutputStream()) {
                byte[] buffer=new byte[4096];int count;while((count=input.read(buffer))!=-1)stream.write(buffer,0,count);
            }
            try(BufferedReader output=new BufferedReader(new InputStreamReader(child.getInputStream()))) {
                String line;while((line=output.readLine())!=null)Log.i(TAG,"child: "+line);
            }
            int status=child.waitFor();
            Log.i(TAG,"file-map proxy+bwrap exit="+status);
        } catch(Throwable error) { Log.e(TAG,"isolated proxy+bwrap probe failed",error); }
    }

    private void probeWrite(String label,File file) {
        try {
            try(java.io.FileOutputStream out=new java.io.FileOutputStream(file)) { out.write(1); }
            //noinspection ResultOfMethodCallIgnored
            file.delete();
            Log.i(TAG,"isolated write probe "+label+"=OK");
        } catch(Throwable error) {
            Log.i(TAG,"isolated write probe "+label+"=DENIED ("+error.getClass().getSimpleName()+")");
        }
    }
}
