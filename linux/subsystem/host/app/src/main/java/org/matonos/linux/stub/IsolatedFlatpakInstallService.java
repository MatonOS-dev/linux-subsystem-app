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
import java.io.DataOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/** Stock Flatpak no-pull deployment in an Android isolated UID. */
public final class IsolatedFlatpakInstallService extends Service {
    private static final String TAG="MatonIsolatedFlatpak";
    private static final long TIMEOUT_MS=14*60*1000L;
    private final IIsolatedFlatpakInstall.Stub binder=new IIsolatedFlatpakInstall.Stub() {
        @Override public void deploy(ParcelFileDescriptor installation,ParcelFileDescriptor repo,
                ParcelFileDescriptor result,String remote,String ref,String commit,String[] subpaths) {
            authorize();validate(remote,ref,commit,subpaths);
            Thread worker=new Thread(()->serve(installation,repo,result,remote,ref,commit,subpaths),"isolated-flatpak-deploy");
            worker.setDaemon(true);worker.start();
        }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }

    private void authorize() {
        int caller=Binder.getCallingUid();
        try {if(caller==getPackageManager().getApplicationInfo("org.matonos.linux",0).uid)return;}
        catch(PackageManager.NameNotFoundException missing){throw new SecurityException("Subsystem app is missing",missing);}
        throw new SecurityException("Isolated deploy service accepts only the subsystem app");
    }
    private static void validate(String remote,String ref,String commit,String[] subpaths) {
        if(remote==null||!remote.matches("[A-Za-z0-9_.-]{1,64}")||!DeployManifest.validRef(ref)
                ||commit==null||!commit.matches("[0-9a-f]{64}")||subpaths==null||subpaths.length>256)
            throw new IllegalArgumentException("Invalid isolated Flatpak deployment request");
        for(String path:subpaths)if(!validSubpath(path))throw new IllegalArgumentException("Invalid Flatpak subpath");
    }
    private static boolean validSubpath(String path) {
        if(path==null||!path.startsWith("/")||path.endsWith("/")||path.contains("//"))return false;
        if(path.equals("/"))return true;
        for(String part:path.substring(1).split("/"))if(part.isEmpty()||part.equals(".")||part.equals(".."))return false;
        return true;
    }

    private void serve(ParcelFileDescriptor installation,ParcelFileDescriptor repo,ParcelFileDescriptor result,
            String remote,String ref,String commit,String[] subpaths) {
        boolean[] responseStarted={false};
        try(ParcelFileDescriptor keepInstall=installation;ParcelFileDescriptor keepRepo=repo;
                DataOutputStream out=new DataOutputStream(new ParcelFileDescriptor.AutoCloseOutputStream(result))) {
          try {
            int uid=Process.myUid(),appUid=getApplicationInfo().uid;
            if(uid==appUid)throw new SecurityException("Deployment service is running as the subsystem UID");
            Log.i(TAG,"isolated uid="+uid+" appUid="+appUid+" deploying "+ref+" at "+commit);
            String installPath="/maton-flatpak";
            ArrayList<String> command=new ArrayList<>();
            command.add(LoaderCommand.loader(this).getAbsolutePath());
            command.add("--parent-pid");command.add(Integer.toString(Process.myPid()));
            command.add("--map");command.add("/proc/"+Process.myPid()+"/fd/"+keepInstall.getFd());command.add(installPath);
            command.add("--map");command.add("/proc/"+Process.myPid()+"/fd/"+keepRepo.getFd());command.add(installPath+"/repo");
            command.add(LoaderCommand.tool(this,LoaderCommand.FLATPAK_CLI).getAbsolutePath());
            command.add("install");command.add("--system");command.add("--assumeyes");command.add("--noninteractive");
            command.add("--no-pull");command.add("--no-deps");command.add("--no-related");
            for(String path:subpaths)command.add("--subpath="+path);
            command.add(remote);command.add(ref);
            ProcessBuilder builder=new ProcessBuilder(command).directory(new File("/")).redirectErrorStream(true);
            Map<String,String> env=builder.environment();env.clear();
            String nativeDir=LoaderCommand.nativeLibraryDir(this).getAbsolutePath();
            env.put("PATH",nativeDir+":/system/bin");env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");env.put("LC_ALL","C");
            env.put("HOME",installPath+"/home");env.put("TMPDIR",installPath+"/tmp");env.put("XDG_RUNTIME_DIR",installPath+"/run");
            env.put("XDG_DATA_HOME",installPath+"/home/.local/share");env.put("XDG_CONFIG_HOME",installPath+"/home/.config");
            env.put("XDG_CACHE_HOME",installPath+"/cache");env.put("FLATPAK_SYSTEM_DIR",installPath);env.put("FLATPAK_USER_DIR",installPath);
            env.put("FLATPAK_SYSTEM_CACHE_DIR",installPath+"/cache");env.put("FLATPAK_DOWNLOAD_TMPDIR",installPath+"/tmp");
            env.put("FLATPAK_BWRAP",LoaderCommand.tool(this,LoaderCommand.FLATPAK_BWRAP).getAbsolutePath());
            Log.i(TAG,"starting stock flatpak install --no-pull in proxy with fd-backed installation/repo");
            java.lang.Process child=builder.start();StringBuilder tail=new StringBuilder();
            java.util.concurrent.ScheduledExecutorService watchdog=java.util.concurrent.Executors.newSingleThreadScheduledExecutor(r->{Thread t=new Thread(r,"isolated-flatpak-timeout");t.setDaemon(true);return t;});
            java.util.concurrent.ScheduledFuture<?> kill=watchdog.schedule(()->child.destroyForcibly(),TIMEOUT_MS,TimeUnit.MILLISECONDS);
            try(BufferedReader reader=new BufferedReader(new InputStreamReader(child.getInputStream(),StandardCharsets.UTF_8))) {
                String line;while((line=reader.readLine())!=null){Log.i(TAG,"flatpak: "+line);if(tail.length()<32768)tail.append(line).append('\n');}
            } finally {kill.cancel(false);watchdog.shutdownNow();}
            if(!child.waitFor(1,TimeUnit.SECONDS)) {child.destroyForcibly();throw new IOException("Flatpak offline deploy timed out");}
            if(child.exitValue()!=0)throw new IOException("Flatpak offline deploy failed ("+child.exitValue()+"): "+tail.toString().trim());
            String version=commit;for(String path:subpaths)version+="-"+path.replace("/","");
            File extra=new File("/proc/"+Process.myPid()+"/fd/"+keepInstall.getFd()+"/"+ref+"/"+version+"/files/extra");
            out.writeByte(0);out.flush();responseStarted[0]=true;
            TarStream.writeExtra(extra.toPath(),out);
          } catch(Throwable failure) {
            Log.e(TAG,"Isolated stock Flatpak deployment failed for "+ref,failure);
            if(!responseStarted[0])try {
                byte[] message=(failure.getMessage()==null?failure.toString():failure.getMessage()).getBytes(StandardCharsets.UTF_8);
                out.writeByte(2);out.writeInt(Math.min(message.length,1024*1024));out.write(message,0,Math.min(message.length,1024*1024));
            }catch(IOException ignored){}
          }
        } catch(Throwable failure) {
            Log.e(TAG,"Cannot close isolated deploy descriptors for "+ref,failure);
        }
    }
}
