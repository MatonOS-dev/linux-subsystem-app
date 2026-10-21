package org.matonos.linux.stub;

import android.content.Context;
import org.matonos.linux.WorkDirs;
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
    interface DependencyRepair { void repair(String ref) throws Exception; }
    private static volatile DependencyRepair dependencyRepair;
    static void setDependencyRepairForTests(DependencyRepair repair){dependencyRepair=repair;}
    private static final java.util.concurrent.ExecutorService SPAWNER =
            java.util.concurrent.Executors.newSingleThreadExecutor(task -> {
                Thread thread = new Thread(task, "v3-spawner");
                thread.setDaemon(true);
                return thread;
            });
    private static Process process;
    private static Integer exit;
    private static String error;
    private static final java.util.Set<Runnable> statusListeners =
            new java.util.concurrent.CopyOnWriteArraySet<>();

    static void addLaunchStatusListener(Runnable listener) {
        boolean terminal;
        synchronized (V3Launcher.class) {
            terminal = exit != null || error != null;
            if (!terminal) statusListeners.add(listener);
        }
        if (terminal) listener.run();
    }
    static void removeLaunchStatusListener(Runnable listener) { statusListeners.remove(listener); }
    private static void notifyLaunchStatusChanged() {
        for (Runnable listener : statusListeners) listener.run();
    }
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
        WorkDirs.sweep(WorkDirs.root(context));
        File launchWork = WorkDirs.directory(context,"launch-data");
        WorkDirs.keep(launchWork); // Loader pack mappings live until process exit.
        File resolv = new File(launchWork, "v3-resolv.conf");
        String dns = LoaderCommand.resolverAddress();
        LoaderCommand.writeResolv(resolv, dns);
        ArrayList<String> command = new ArrayList<>();
        command.add(loader.getAbsolutePath());
        command.add("--gpu-bridge");
        // adb (root): touch <cache>/v3-trace to log every trapped call.
        if (new File(cache, "v3-trace").exists()) command.add("--trace");
        if (context.checkSelfPermission(android.Manifest.permission.INTERNET) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED) {
            LoaderCommand.addDns(command, dns, WorkDirs.root(context));
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
        java.util.List<DeclaredPackages.Entry> packages=DeclaredPackages.resolve(context,dependencyRef->{
            DependencyRepair repair=dependencyRepair;
            if(repair!=null)repair.repair(dependencyRef);else repairDependency(context,ref,dependencyRef);
        });
        String runtimeMetadata=null;
        for(DeclaredPackages.Entry entry:packages)
            if(entry.record.ref.startsWith("runtime/org.freedesktop.Platform/x86_64/")) {
                runtimeMetadata=new String(entry.record.metadata,StandardCharsets.UTF_8);break;
            }
        Context host=context.createPackageContext("org.matonos.linux",0);
        boolean matonGlEnabled=runtimeMetadata!=null;
        File deployment=PackageDeployment.prepare(context,ref);
        if(matonGlEnabled) {
            String runtimeRef=null;
            for(DeclaredPackages.Entry entry:packages)
                if(entry.record.ref.startsWith("runtime/org.freedesktop.Platform/x86_64/")) {runtimeRef=entry.record.ref;break;}
            String runtimeBranch=runtimeRef==null?"":runtimeRef.substring(runtimeRef.lastIndexOf('/')+1);
            String glBranch=PackageMounts.glExtensionBranch(runtimeMetadata,runtimeBranch);
            File extensionFiles=PackageDeployment.prepareGlExtension(new File(deployment,"system"),
                    new File(host.getApplicationInfo().sourceDir),glBranch);
            command.addAll(org.matonos.linuxhost.stubgen.PackLaunch.argumentsFromArchive(
                    new File(host.getApplicationInfo().sourceDir), "assets/gl-maton", launchWork,
                    extensionFiles.getAbsolutePath()));
            command.addAll(org.matonos.linuxhost.stubgen.PackLaunch.backingArguments(
                    extensionFiles.getAbsolutePath(),extensionFiles));
        }
        command.addAll(DeclaredPackages.arguments(packages,launchWork,true,deployment));
        java.util.Collections.addAll(command,
                LoaderCommand.tool(context, LoaderCommand.FLATPAK_CLI).getAbsolutePath(),
                // The session bus is the stub's private broker, which enforces the app's
                // bus policy itself: bind it directly instead of starting xdg-dbus-proxy.
                "--user", "run", "--socket=wayland", "--socket=x11", "--socket=session-bus",
                "--no-documents-portal",
                "--no-a11y-bus", "--nofilesystem=host", "--nofilesystem=/tmp",
                "--nodevice=all", "--device=dri",
                "--env=QT_WAYLAND_DISABLE_WINDOWDECORATION=1", "--env=GTK_CSD=0",
                "--env=NO_AT_BRIDGE=1",
                // Prefer Wayland; X11-only apps still reach Xwayland via DISPLAY.
                "--env=GDK_BACKEND=wayland,x11", "--env=QT_QPA_PLATFORM=wayland;xcb",
                "--env=ELECTRON_OZONE_PLATFORM_HINT=auto", "--env=MOZ_ENABLE_WAYLAND=1");
        if(matonGlEnabled){command.add("--env=FLATPAK_GL_DRIVERS=maton:default");command.add("--env=__GLX_VENDOR_LIBRARY_NAME=maton");}
        // adb (root): touch <cache>/v3-vkdebug to log Vulkan loader driver discovery.
        if (new File(cache, "v3-vkdebug").exists()) command.add("--env=VK_LOADER_DEBUG=all");
        // adb (root): write a command name or absolute sandbox path to <cache>/v3-command
        // to run it instead of the app's default (e.g. vkcube, or a test binary in the app dir).
        File override=new File(cache, "v3-command");
        if (override.exists()) {
            String name=new String(java.nio.file.Files.readAllBytes(override.toPath()),
                    java.nio.charset.StandardCharsets.UTF_8).trim();
            if (!name.matches("[A-Za-z0-9._+-]+|(/[A-Za-z0-9._+-]+)+") || name.contains("/../") || name.endsWith("/..")) throw new java.io.IOException("Invalid v3-command");
            command.add("--command="+name);
        }
        command.add(ref.substring(4));
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        Map<String, String> env = builder.environment();
        env.clear();
        env.put("FLATPAK_SYSTEM_DIR", new File(deployment,"system").getAbsolutePath());
        // Flatpak reads this in the launcher process while selecting runtime
        // extensions; the matching --env below exposes the choice to the app.
        if(matonGlEnabled){env.put("FLATPAK_GL_DRIVERS","maton:default");env.put("__GLX_VENDOR_LIBRARY_NAME","maton");}
        env.put("MATON_FLATPAK_DNS", dns);
        String nativeDir = LoaderCommand.nativeLibraryDir(context).getAbsolutePath();
        env.put("PATH", nativeDir + ":/system/bin");
        env.put("LD_LIBRARY_PATH", nativeDir + ":/system/lib64");
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
                notifyLaunchStatusChanged();
                Log.i("MatonV3", "Loader exited: " + status);
                endWithLinuxApp(context);
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                synchronized (V3Launcher.class) { error = "Interrupted while waiting for loader"; }
                notifyLaunchStatusChanged();
            }
        }, "v3-exit").start();
        return getLaunchStatus();
    }

    /** The Android process lives exactly as long as its Linux app: when the
     *  loader exits (clean exit or crash), close this app's tasks and end. */
    private static void endWithLinuxApp(Context context) {
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
            try {
                android.app.ActivityManager am = context.getSystemService(android.app.ActivityManager.class);
                for (android.app.ActivityManager.AppTask task : am.getAppTasks()) task.finishAndRemoveTask();
            } catch (Exception failure) { Log.w("MatonV3", "Cannot close tasks after loader exit", failure); }
            android.os.Process.killProcess(android.os.Process.myPid());
        });
    }

    static void repairDependency(Context context,String appRef,String dependencyRef) throws Exception {
        java.util.concurrent.CountDownLatch complete=new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.atomic.AtomicReference<String> error=new java.util.concurrent.atomic.AtomicReference<>();
        android.content.ServiceConnection connection=new android.content.ServiceConnection(){
            @Override public void onServiceConnected(android.content.ComponentName name,android.os.IBinder binder){
                try{org.matonos.linux.ILinuxAppStore.Stub.asInterface(binder).repair(appRef,"repair-"+java.util.UUID.randomUUID().toString().replace("-",""),new org.matonos.linux.ILinuxAppStoreCallback.Stub(){
                    @Override public void onProgress(String id,String phase,int percent,long done,long total){}
                    @Override public void onResult(String id,boolean ok,String message){if(!ok)error.set(message==null?"Subsystem app repair failed":message);complete.countDown();}
                });}catch(Exception e){error.set(e.getMessage());complete.countDown();}
            }
            @Override public void onServiceDisconnected(android.content.ComponentName name){error.set("Linux Subsystem disconnected during dependency repair");complete.countDown();}
        };
        android.content.Intent intent=new android.content.Intent().setClassName("org.matonos.linux","org.matonos.linux.LinuxAppStoreService");
        if(!context.bindService(intent,connection,Context.BIND_AUTO_CREATE))throw new java.io.IOException("Cannot connect to Linux Subsystem to repair declared package "+dependencyRef);
        boolean finished;
        try{finished=complete.await(10,java.util.concurrent.TimeUnit.MINUTES);}
        finally{try{context.unbindService(connection);}catch(Exception ignored){}}
        if(!finished)throw new java.io.IOException("Timed out repairing declared package "+dependencyRef);
        if(error.get()!=null)throw new java.io.IOException("Cannot repair declared package "+dependencyRef+": "+error.get());
    }

    static synchronized JSONObject getLaunchStatus() throws Exception {
        JSONObject status = new JSONObject().put("ok", process != null && exit == null && error == null);
        if (exit != null) status.put("exitCode", exit).put("error", "Application exited (" + exit + ")");
        if (error != null) status.put("error", error);
        return status;
    }
}
