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
    private static final java.util.concurrent.ExecutorService SPAWNER =
            java.util.concurrent.Executors.newSingleThreadExecutor(task -> {
                Thread thread = new Thread(task, "v3-spawner");
                thread.setDaemon(true);
                return thread;
            });
    private static Process process;
    private static Integer exit;
    private static String error;
    private static java.nio.channels.FileChannel deploymentChannel;
    private static java.nio.channels.FileLock deploymentLease;

    private static File pinDeployment(File current,int appid) throws Exception {
        for (;;) {
            File version=current.getCanonicalFile();
            if(!version.getParent().equals("/data/linux-app/install") ||
               !version.getName().matches(appid+"-[A-Za-z0-9]+"))
                throw new java.io.IOException("Invalid Linux deployment pointer");
            java.nio.channels.FileChannel channel;
            try {channel=java.nio.channels.FileChannel.open(
                    new File(version,".lease").toPath(),java.nio.file.StandardOpenOption.READ);}
            catch(java.nio.file.NoSuchFileException removed) {
                if(!current.getCanonicalFile().equals(version))continue;
                throw removed;
            }
            java.nio.channels.FileLock lease=channel.tryLock(0,Long.MAX_VALUE,true);
            if(lease==null){channel.close();Thread.yield();continue;}
            if(!current.getCanonicalFile().equals(version)) {lease.release();channel.close();continue;}
            deploymentChannel=channel;deploymentLease=lease;return version;
        }
    }

    static synchronized JSONObject launch(Context context, String ref) throws Exception {
        if (process != null) return getLaunchStatus();
        if (ref == null || !ref.matches("app/[A-Za-z0-9_.-]+/x86_64/[A-Za-z0-9_.-]+"))
            throw new IllegalArgumentException("Invalid application reference");
        int uid = android.os.Process.myUid();
        String root="/data/linux-app";
        File current=new File(root+"/install/"+(uid%100000));
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
        env.put("FLATPAK_SYSTEM_DIR", root + "/runtime");
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
        File deployment=pinDeployment(current,uid%100000);
        // PR_SET_PDEATHSIG (the loader's --parent-pid) fires when the *thread*
        // that forked it exits, not the process. Activity launch threads end
        // whenever a window closes, so fork from a thread that lives as long as
        // this stub process.
        try {env.put("FLATPAK_USER_DIR",deployment.getAbsolutePath());process = SPAWNER.submit(builder::start).get();}
        catch(Exception failure){deploymentLease.release();deploymentChannel.close();deploymentLease=null;deploymentChannel=null;throw failure;}
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

    static synchronized JSONObject getLaunchStatus() throws Exception {
        JSONObject status = new JSONObject().put("ok", process != null && exit == null && error == null);
        if (exit != null) status.put("exitCode", exit).put("error", "Application exited (" + exit + ")");
        if (error != null) status.put("error", error);
        return status;
    }
}
