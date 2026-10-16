package org.matonos.linux.stub;

import android.content.Context;
import android.util.Log;
import org.json.JSONObject;
import org.matonos.linux.PerAppRuntime;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Map;

/** One Linux process tree per stub UID, under the inherited app filter/domain. */
final class V3Launcher {
    interface RuntimeRepair { void repair(String ref) throws Exception; }
    private static volatile RuntimeRepair runtimeRepair;
    static void setRuntimeRepairForTests(RuntimeRepair repair){runtimeRepair=repair;}
    private static final java.util.concurrent.ExecutorService SPAWNER =
            java.util.concurrent.Executors.newSingleThreadExecutor(task -> {
                Thread thread = new Thread(task, "v3-spawner");
                thread.setDaemon(true);
                return thread;
            });
    private static Process process;
    private static Integer exit;
    private static String error;
    /** Installed runtime package, trusted only when signed by this subsystem's key. */
    private static android.content.pm.PackageInfo runtimePackage(Context context,String name)
            throws android.content.pm.PackageManager.NameNotFoundException,java.io.IOException {
        android.content.pm.PackageManager pm=context.getPackageManager();
        android.content.pm.PackageInfo pkg=pm.getPackageInfo(name,android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES);
        android.content.pm.PackageInfo host=pm.getPackageInfo(context.getPackageName(),android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES);
        if(!sameSigner(pkg,host))throw new java.io.IOException("Runtime package has an untrusted signer: "+name);
        return pkg;
    }
    private static boolean sameSigner(android.content.pm.PackageInfo a,android.content.pm.PackageInfo b){return a.signingInfo!=null&&b.signingInfo!=null&&!a.signingInfo.hasMultipleSigners()&&!b.signingInfo.hasMultipleSigners()&&a.signingInfo.getApkContentsSigners().length==1&&b.signingInfo.getApkContentsSigners().length==1&&a.signingInfo.getApkContentsSigners()[0].equals(b.signingInfo.getApkContentsSigners()[0]);}

    static synchronized JSONObject launch(Context context, String ref)
            throws Exception {
        if (process != null) return getLaunchStatus();
        if (ref == null || !ref.matches("app/[A-Za-z0-9_.-]+/x86_64/[A-Za-z0-9_.-]+"))
            throw new IllegalArgumentException("Invalid application reference");
        // PackageManager follows signature-verified updates of the image host.
        File loader = LoaderCommand.loader(context);
        PerAppRuntime runtime = StubService.runtime();
        if (runtime == null) throw new IllegalStateException("Application runtime is unavailable");
        // Canonical paths: /data/user/0 is a symlink to /data/data, and Flatpak
        // recreates symlinked HOME ancestors in the sandbox, which collides
        // with directories bwrap already made for other binds.
        File home = new File(context.getDataDir().getCanonicalFile(), "home");
        File cache = context.getCacheDir().getCanonicalFile();
        for (File directory : new File[]{home, cache, new File(home, ".config"), new File(home, ".local/share")})
            if (!directory.isDirectory() && !directory.mkdirs())
                throw new java.io.IOException("Cannot create application directory " + directory);
        File resolv = new File(cache, "v3-resolv.conf");
        String dns = LoaderCommand.resolverAddress();
        LoaderCommand.writeResolv(resolv, dns);
        ArrayList<String> command = new ArrayList<>();
        command.add(loader.getAbsolutePath());
        // adb (root): touch <cache>/v3-trace to log every trapped call.
        if (new File(cache, "v3-trace").exists()) command.add("--trace");
        if (context.checkSelfPermission(android.Manifest.permission.INTERNET) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED) {
            LoaderCommand.addDns(command, dns, runtime.directory());
        }
        File commandSocket=new File(runtime.directory(),"host-command");
        if(commandSocket.exists()&&!commandSocket.delete())throw new java.io.IOException("Cannot remove stale command socket");
        command.add("--host-command");command.add(commandSocket.getAbsolutePath());
        command.add("--parent-pid"); command.add(Integer.toString(android.os.Process.myPid()));
        LoaderCommand.addResolvMaps(command, resolv);
        File x11 = new File(runtime.directory(), "x11");
        if (x11.isDirectory()) {
            command.add("--map"); command.add(x11.getAbsolutePath()); command.add("/tmp/.X11-unix");
        }
        android.content.pm.ApplicationInfo installed=context.getApplicationInfo();
        // The applied extra output is a self-contained split mounted at
        // <mount>/extra, exactly where a deploy-time apply_extra would have
        // left its output; the base packs mount at <mount>.
        command.addAll(org.matonos.linuxhost.stubgen.PackLaunch.argumentsWithExtra(
                new File(installed.sourceDir),installed.splitNames,installed.splitSourceDirs,cache,
                org.matonos.linuxhost.stubgen.StubGenerator.APP_MOUNT,
                org.matonos.linuxhost.stubgen.StubGenerator.APP_MOUNT+"/extra"));
        android.content.pm.ActivityInfo stubInfo=context.getPackageManager().getActivityInfo(
                new android.content.ComponentName(context,StubActivity.class),
                android.content.pm.PackageManager.GET_META_DATA);
        String required=stubInfo.metaData==null?null:stubInfo.metaData.getString(org.matonos.linuxhost.stubgen.StubGenerator.RUNTIME_PACKAGES_META);
        if(required!=null&&!required.isEmpty())for(String runtimeRecord:required.split(";")) {
            String[] fields=runtimeRecord.split(">",3);if(fields.length!=3)throw new java.io.IOException("Invalid runtime package metadata");
            String runtimeRef=fields[0],runtimePackage=fields[1],mountPoint=fields[2];
            android.content.pm.PackageInfo pkg;
            try {
                pkg=runtimePackage(context,runtimePackage);
            } catch(android.content.pm.PackageManager.NameNotFoundException missing) {
                RuntimeRepair repair=runtimeRepair;
                if(repair!=null)repair.repair(runtimeRef);
                else repairRuntime(context,ref,runtimeRef);
                try {
                    pkg=runtimePackage(context,runtimePackage);
                }catch(android.content.pm.PackageManager.NameNotFoundException stillMissing){throw new java.io.IOException("Runtime repair completed but required package is still missing: "+runtimeRef,stillMissing);}
            }
            command.addAll(org.matonos.linuxhost.stubgen.PackLaunch.argumentsWithExtra(
                    new File(pkg.applicationInfo.sourceDir),
                    pkg.splitNames,pkg.applicationInfo.splitSourceDirs,cache,mountPoint,mountPoint+"/extra"));
        }
        File deployment=PackageDeployment.prepare(context,ref);
        java.util.Collections.addAll(command,
                "/apex/com.matonos.flatpak/bin/matonos-app-flatpak",
                "--user", "run", "--socket=wayland", "--socket=x11", "--no-documents-portal",
                "--no-a11y-bus", "--nofilesystem=host", "--nofilesystem=/tmp",
                "--nodevice=all", "--device=dri",
                "--env=QT_WAYLAND_DISABLE_WINDOWDECORATION=1", "--env=GTK_CSD=0",
                "--env=NO_AT_BRIDGE=1", ref.substring(4));
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        Map<String, String> env = builder.environment();
        env.clear();
        env.put("FLATPAK_SYSTEM_DIR", new File(deployment,"system").getAbsolutePath());
        env.put("MATON_FLATPAK_DNS", dns);
        env.put("PATH", "/apex/com.matonos.flatpak/bin:/system/bin");
        env.put("LD_LIBRARY_PATH", "/apex/com.matonos.flatpak/lib64:/system/lib64");
        env.put("HOME", home.getAbsolutePath());
        env.put("TMPDIR", cache.getAbsolutePath());
        env.put("FLATPAK_DOWNLOAD_TMPDIR", cache.getAbsolutePath());
        env.put("FLATPAK_SYSTEM_CACHE_DIR", cache.getAbsolutePath());
        env.put("XDG_CACHE_HOME", cache.getAbsolutePath());
        env.put("XDG_CONFIG_HOME", new File(home, ".config").getAbsolutePath());
        env.put("XDG_DATA_HOME", new File(home, ".local/share").getAbsolutePath());
        env.put("XDG_RUNTIME_DIR", runtime.directory().getAbsolutePath());
        env.put("WAYLAND_DISPLAY", "wayland-0");
        env.put("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + new File(runtime.directory(), "bus"));
        env.put("PULSE_SERVER", "unix:" + new File(runtime.directory(), "pulse/native"));
        File alias = new File(runtime.directory(), "wayland-0-x11");
        if (Files.isSymbolicLink(alias.toPath())) {
            String target = Files.readSymbolicLink(alias.toPath()).toString();
            if (target.matches("x11/X[0-9]+")) env.put("DISPLAY", ":" + target.substring(5));
        }
        builder.directory(home);
        // PR_SET_PDEATHSIG (the loader's --parent-pid) fires when the *thread*
        // that forked it exits, not the process. Activity launch threads end
        // whenever a window closes, so fork from a thread that lives as long as
        // this stub process.
        env.put("FLATPAK_USER_DIR",deployment.getAbsolutePath());
        process = SPAWNER.submit(builder::start).get();
        final Process child = process;
        Log.i("MatonV3", "Started v3 loader for " + context.getPackageName());
        new Thread(() -> {
            try (BufferedReader output = new BufferedReader(new InputStreamReader(child.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = output.readLine()) != null) Log.i("MatonV3", line);
            } catch (Exception failure) { Log.w("MatonV3", "Cannot read loader output", failure); }
        }, "v3-output").start();
        new Thread(() -> {
            try {
                int status = child.waitFor();
                synchronized (V3Launcher.class) { exit = status; }
                Log.i("MatonV3", "Loader exited: " + status);
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                synchronized (V3Launcher.class) { error = "Interrupted while waiting for loader"; }
            }
        }, "v3-exit").start();
        return getLaunchStatus();
    }

    static void repairRuntime(Context context,String appRef,String runtimeRef) throws Exception {
        java.util.concurrent.CountDownLatch complete=new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.atomic.AtomicReference<String> error=new java.util.concurrent.atomic.AtomicReference<>();
        android.content.ServiceConnection connection=new android.content.ServiceConnection(){
            @Override public void onServiceConnected(android.content.ComponentName name,android.os.IBinder binder){
                try{org.matonos.linux.ILinuxAppStore.Stub.asInterface(binder).repair(appRef,"repair-"+java.util.UUID.randomUUID().toString().replace("-",""),new org.matonos.linux.ILinuxAppStoreCallback.Stub(){
                    @Override public void onProgress(String id,String phase,int percent,long done,long total){}
                    @Override public void onResult(String id,boolean ok,String message){if(!ok)error.set(message==null?"Subsystem app repair failed":message);complete.countDown();}
                });}catch(Exception e){error.set(e.getMessage());complete.countDown();}
            }
            @Override public void onServiceDisconnected(android.content.ComponentName name){error.set("Linux Subsystem disconnected during runtime repair");complete.countDown();}
        };
        android.content.Intent intent=new android.content.Intent().setClassName("org.matonos.linux","org.matonos.linux.LinuxAppStoreService");
        if(!context.bindService(intent,connection,Context.BIND_AUTO_CREATE))throw new java.io.IOException("Cannot connect to Linux Subsystem to repair required runtime "+runtimeRef);
        boolean finished;
        try{finished=complete.await(10,java.util.concurrent.TimeUnit.MINUTES);}
        finally{try{context.unbindService(connection);}catch(Exception ignored){}}
        if(!finished)throw new java.io.IOException("Timed out repairing required runtime "+runtimeRef);
        if(error.get()!=null)throw new java.io.IOException("Cannot repair required runtime "+runtimeRef+": "+error.get());
    }

    static synchronized JSONObject getLaunchStatus() throws Exception {
        JSONObject status = new JSONObject().put("ok", process != null && exit == null && error == null);
        if (exit != null) status.put("exitCode", exit).put("error", "Application exited (" + exit + ")");
        if (error != null) status.put("error", error);
        return status;
    }
}
