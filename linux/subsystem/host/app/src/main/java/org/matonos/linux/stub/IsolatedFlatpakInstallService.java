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
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Map;
import java.util.concurrent.TimeUnit;

/** Stock Flatpak no-pull deployment in an Android isolated UID. */
public final class IsolatedFlatpakInstallService extends Service {
    private static final String TAG="MatonIsolatedFlatpak";
    private static final long TIMEOUT_MS=14*60*1000L;
    private final IIsolatedFlatpakInstall.Stub binder=new IIsolatedFlatpakInstall.Stub() {
        @Override public void deploy(ParcelFileDescriptor input,ParcelFileDescriptor result,
                String remote,String ref,String commit,String[] subpaths) {
            authorize();validate(remote,ref,commit,subpaths);
            Thread worker=new Thread(()->serve(input,result,remote,ref,commit,subpaths),"isolated-flatpak-deploy");
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

    private void serve(ParcelFileDescriptor input,ParcelFileDescriptor result,
            String remote,String ref,String commit,String[] subpaths) {
        boolean responseStarted=false;
        try(ParcelFileDescriptor keepInput=input;
                DataOutputStream out=new DataOutputStream(new ParcelFileDescriptor.AutoCloseOutputStream(result))) {
          java.lang.Process child=null;
          try {
            int uid=Process.myUid(),appUid=getApplicationInfo().uid;
            if(uid==appUid)throw new SecurityException("Deployment service is running as the subsystem UID");
            Log.i(TAG,"isolated uid="+uid+" appUid="+appUid+" deploying "+ref+" at "+commit);
            String version=commit;for(String path:subpaths)version+="-"+path.replace("/","");
            String extra="/maton-flatpak/"+ref+"/"+version+"/files";
            String deployScript="extra=\"$1\"; shift; if /tools/libmatonos-v3-loader.so /tools/libmatonos-app-flatpak.so "
                    +"install --system --assumeyes --noninteractive --no-pull --no-deps --no-related \"$@\" >/dev/stderr 2>&1; then "
                    +"printf '\\000'; /bin/toybox mkdir -p \"$extra/extra\"; /bin/toybox tar -cf - -C \"$extra\" extra; "
                    +"else printf '\\002Flatpak offline deploy failed\\n'; exit 1; fi";
            String script="/bin/toybox tar -xf - -C /work || { printf '\\002archive extraction failed\\n'; exit 1; }; "
                    +"extra=$1; shift; exec /tools/libmatonos-v3-loader.so /tools/libmatonos-app-bwrap.so --unshare-net "
                    +"--bind /work/install /maton-flatpak --dir /maton-flatpak/repo --bind /work/repo /maton-flatpak/repo "
                    +"--proc /proc --dev /dev /bin/toybox sh -c "+shellQuote(deployScript)+" matonos \"$extra\" \"$@\"";
            ArrayList<String> command=new ArrayList<>();
            command.add(LoaderCommand.loader(this).getAbsolutePath());
            command.add("--parent-pid");command.add(Integer.toString(Process.myPid()));
            command.add(LoaderCommand.tool(this,LoaderCommand.FLATPAK_BWRAP).getAbsolutePath());
            command.add("--unshare-net");command.add("--tmpfs");command.add("/work");
            command.add("--ro-bind");command.add(LoaderCommand.nativeLibraryDir(this).getAbsolutePath());command.add("/tools");
            command.add("--ro-bind");command.add("/system/bin/toybox");command.add("/bin/toybox");
            command.add("--proc");command.add("/proc");command.add("--dev");command.add("/dev");
            command.add("/bin/toybox");command.add("sh");command.add("-c");command.add(script);command.add("matonos");command.add(extra);
            for(String path:subpaths)command.add("--subpath="+path);
            command.add(remote);command.add(ref);
            ProcessBuilder builder=new ProcessBuilder(command).directory(new File("/")).redirectErrorStream(false);
            Map<String,String> env=builder.environment();env.clear();
            String nativeDir=LoaderCommand.nativeLibraryDir(this).getAbsolutePath();
            env.put("PATH",nativeDir+":/system/bin");env.put("LD_LIBRARY_PATH",nativeDir+":/system/lib64");env.put("LC_ALL","C");
            env.put("HOME","/maton-flatpak/home");env.put("TMPDIR","/maton-flatpak/tmp");env.put("XDG_RUNTIME_DIR","/maton-flatpak/run");
            env.put("XDG_DATA_HOME","/maton-flatpak/home/.local/share");env.put("XDG_CONFIG_HOME","/maton-flatpak/home/.config");
            env.put("XDG_CACHE_HOME","/maton-flatpak/cache");env.put("FLATPAK_SYSTEM_DIR","/maton-flatpak");env.put("FLATPAK_USER_DIR","/maton-flatpak");
            env.put("FLATPAK_SYSTEM_CACHE_DIR","/maton-flatpak/cache");env.put("FLATPAK_DOWNLOAD_TMPDIR","/maton-flatpak/tmp");
            env.put("FLATPAK_BWRAP",LoaderCommand.tool(this,LoaderCommand.FLATPAK_BWRAP).getAbsolutePath());
            Log.i(TAG,"starting stock flatpak install --no-pull from a streamed archive in bwrap tmpfs");
            child=builder.start();StringBuilder tail=new StringBuilder();java.lang.Process running=child;
            Thread errors=new Thread(()->readErrors(running,tail),"isolated-flatpak-log");errors.setDaemon(true);errors.start();
            java.util.concurrent.ScheduledExecutorService watchdog=java.util.concurrent.Executors.newSingleThreadScheduledExecutor(r->{Thread t=new Thread(r,"isolated-flatpak-timeout");t.setDaemon(true);return t;});
            java.util.concurrent.ScheduledFuture<?> kill=watchdog.schedule(()->running.destroyForcibly(),TIMEOUT_MS,TimeUnit.MILLISECONDS);
            try {
                try(InputStream archive=new ParcelFileDescriptor.AutoCloseInputStream(keepInput);OutputStream stdin=child.getOutputStream()) {
                    byte[] buffer=new byte[65536];int count;while((count=archive.read(buffer))!=-1)stdin.write(buffer,0,count);
                }
                int status=child.getInputStream().read();
                if(status==0) {out.writeByte(0);out.flush();responseStarted=true;copy(child.getInputStream(),out);}
                else if(status==2) {
                    BufferedReader reader=new BufferedReader(new InputStreamReader(child.getInputStream(),StandardCharsets.UTF_8));
                    String message=reader.readLine();writeError(out,new IOException(message==null?"Flatpak offline deploy failed":message));
                } else throw new IOException("Flatpak sandbox exited without a result: "+tail.toString().trim());
                if(!child.waitFor(1,TimeUnit.SECONDS)) {child.destroyForcibly();throw new IOException("Flatpak offline deploy timed out");}
                errors.join(1000);
                if(child.exitValue()!=0)Log.e(TAG,"Flatpak archive stream exited "+child.exitValue()+": "+tail.toString().trim());
            } finally {
                kill.cancel(false);watchdog.shutdownNow();
            }
          } catch(Throwable failure) {
            Log.e(TAG,"Isolated stock Flatpak deployment failed for "+ref,failure);
            if(!responseStarted)writeError(out,failure);
            if(child!=null)child.destroyForcibly();
          }
        } catch(Throwable failure) {
            Log.e(TAG,"Cannot close isolated deploy descriptors for "+ref,failure);
        }
    }

    private static void copy(InputStream input,OutputStream output)throws IOException {
        byte[] buffer=new byte[65536];int count;while((count=input.read(buffer))!=-1)output.write(buffer,0,count);output.flush();
    }
    private static void readErrors(java.lang.Process child,StringBuilder tail) {
        try(BufferedReader reader=new BufferedReader(new InputStreamReader(child.getErrorStream(),StandardCharsets.UTF_8))) {
            String line;while((line=reader.readLine())!=null){Log.i(TAG,"flatpak: "+line);synchronized(tail){if(tail.length()<32768)tail.append(line).append('\n');}}
        }catch(IOException ignored){}
    }
    private static void writeError(DataOutputStream out,Throwable failure) {
        try {byte[] message=(failure.getMessage()==null?failure.toString():failure.getMessage()).getBytes(StandardCharsets.UTF_8);
            out.writeByte(2);out.writeInt(Math.min(message.length,1024*1024));out.write(message,0,Math.min(message.length,1024*1024));out.flush();
        }catch(IOException ignored){}
    }
    private static String shellQuote(String value) { return "'"+value.replace("'","'\\''")+"'"; }
}
