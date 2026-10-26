package org.matonos.linux.stub;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import org.matonos.linuxhost.stubgen.ErofsImage;
import org.matonos.linuxhost.stubgen.ObjectPacks;

/** Runs stock Flatpak deploy under the generated package's own UID. */
public final class ExtraDataInstallService extends Service {
    public static final String ACTION = "org.matonos.linuxhost.EXTRA_DATA_INSTALL";
    private static final String TAG = "MatonExtraData";

    /** Binder call 1: result pipe fd, ref, remote, url, subpaths. FDs cannot travel in service Intents. */
    @Override public IBinder onBind(Intent intent) {
        return new android.os.Binder() {
            @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
                    throws android.os.RemoteException {
                if (code != 1) return super.onTransact(code, data, reply, flags);
                ParcelFileDescriptor result = data.readFileDescriptor();
                Intent request = new Intent(ACTION).putExtra("ref", data.readString()).putExtra("remote", data.readString())
                        .putExtra("url", data.readString()).putExtra("subpaths", data.readString());
                if (result == null) return false;
                new Thread(() -> {
                    try (ParcelFileDescriptor fd = result;
                            java.io.DataOutputStream out = new java.io.DataOutputStream(new FileOutputStream(fd.getFileDescriptor()))) {
                        try {
                            build(request, out);
                            out.flush();
                        } catch (Throwable error) {
                            android.util.Log.e(TAG, "Extra-data deployment failed", error);
                            out.writeInt(-1);
                            out.writeUTF(String.valueOf(error.getMessage()));
                        }
                    } catch (Exception error) { android.util.Log.e(TAG, "Could not return extra-data result", error); }
                }, "maton-extra-data").start();
                return true;
            }
        };
    }

    private void build(Intent request, java.io.DataOutputStream out) throws Exception {
        String ref = request.getStringExtra("ref"), remote = request.getStringExtra("remote"), url = request.getStringExtra("url");
        if (ref == null || !ref.matches("(?:app|runtime)/[A-Za-z0-9._+-]+/[A-Za-z0-9._+-]+/[A-Za-z0-9._+-]+")
                || remote == null || !remote.matches("[A-Za-z0-9._-]+") || url == null || !url.startsWith("https://"))
            throw new IOException("Invalid Flatpak install request");
        File root = new File(getFilesDir(), "extra-flatpak");
        File home = new File(root, "home"), tmp = new File(root, "tmp"), run = new File(root, "run"), cache = new File(root, "cache");
        for (File dir : new File[]{root, home, tmp, run, cache}) if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create Flatpak directory");
        File userData = new File(root, "xdg-data");
        File userConfig = new File(root, "xdg-config");
        userData.mkdirs(); userConfig.mkdirs();
        File flatpakUser = new File(userData, "flatpak");
        String dns=LoaderCommand.resolverAddress();
        File resolv=LoaderCommand.writeResolv(new File(run,"install-resolv.conf"),dns);
        List<String> prefix=new ArrayList<>();
        prefix.add(LoaderCommand.loader(this).getAbsolutePath());prefix.add("--parent-pid");
        prefix.add(Integer.toString(android.os.Process.myPid()));LoaderCommand.addDns(prefix,dns);LoaderCommand.addResolvMaps(prefix,resolv);
        runFlatpak(prefix,home, tmp, run, cache, flatpakUser, "remote-add", "--user", "--if-not-exists", remote, url);
        String tags=android.os.LocaleList.getDefault().toLanguageTags();
        List<String> localeTags=new ArrayList<>();if(tags!=null&&!tags.isEmpty())java.util.Collections.addAll(localeTags,tags.split(","));
        runFlatpak(prefix,home,tmp,run,cache,flatpakUser,"config","--user","--set","languages",
                org.matonos.linux.stub.FlatpakLanguages.configuration(localeTags));
        List<String> install = new ArrayList<>();
        install.add("install"); install.add("--user"); install.add("--assumeyes"); install.add("--no-related");
        String subpaths = request.getStringExtra("subpaths");
        if (subpaths != null) for (String subpath : subpaths.split("\n")) if (!subpath.isEmpty()) install.add("--subpath=" + subpath);
        install.add(remote); install.add(ref);
        runFlatpak(prefix,home, tmp, run, cache, flatpakUser, install.toArray(new String[0]));
        File deploy = new File(flatpakUser, "repo/deploy/" + ref);
        File current = new File(deploy, "active/files");
        if (!current.isDirectory()) {
            String[] children = deploy.list();
            if (children != null) for (String child : children) if (child.endsWith(".files")) current = new File(deploy, child);
        }
        if (!current.isDirectory()) throw new IOException("Flatpak did not create a deployed files tree");
        String imagePrefix = ref.startsWith("runtime/")
                ? "/" + ref.substring(ref.lastIndexOf('/') + 1) + "/" : "/app/";
        List<ObjectPacks.Entry> entries = ObjectPacks.fromTreeWithModes(current, imagePrefix);
        File output = new File(root, "deployment.erofs");
        File mkfs = LoaderCommand.tool(this, "libmatonos-mkfs-erofs.so");
        ErofsImage.build(entries, output.getParentFile(), mkfs, System.currentTimeMillis() / 1000L);
        try (java.io.InputStream in = org.matonos.linuxhost.stubgen.PlaceholderFiles.open(output)) {
            out.writeInt(0); out.writeLong(output.length());
            byte[] buffer = new byte[65536]; int count;
            while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
        }
    }

    private void runFlatpak(List<String> prefix,File home, File tmp, File run, File cache, File userDir, String... args) throws Exception {
        List<String> command = new ArrayList<>();
        command.addAll(prefix);
        command.add(LoaderCommand.tool(this, LoaderCommand.FLATPAK_CLI).getAbsolutePath());
        java.util.Collections.addAll(command, args);
        ProcessBuilder builder = new ProcessBuilder(command).directory(home).redirectErrorStream(true);
        java.util.Map<String,String> env = builder.environment(); env.clear();
        String nativeDir = LoaderCommand.nativeLibraryDir(this).getAbsolutePath();
        env.put("PATH", nativeDir + ":/system/bin"); env.put("LD_LIBRARY_PATH", nativeDir + ":/system/lib64");
        env.put("LC_ALL","C");env.put("HOME", home.getAbsolutePath()); env.put("TMPDIR", tmp.getAbsolutePath()); env.put("XDG_RUNTIME_DIR", run.getAbsolutePath());
        env.put("XDG_DATA_HOME", new File(home, ".local/share").getAbsolutePath()); env.put("XDG_CONFIG_HOME", new File(home, ".config").getAbsolutePath()); env.put("XDG_CACHE_HOME", cache.getAbsolutePath());
        env.put("FLATPAK_USER_DIR", userDir.getAbsolutePath()); env.put("FLATPAK_DOWNLOAD_TMPDIR", tmp.getAbsolutePath());
        env.put("FLATPAK_SYSTEM_CACHE_DIR",cache.getAbsolutePath());
        for(String key:new String[]{"SSL_CERT_DIR","CURL_CA_BUNDLE","G_TLS_CA_PATH"})env.put(key,"/apex/com.android.conscrypt/cacerts");
        Process process = builder.start(); byte[] log;
        try (java.io.InputStream in = process.getInputStream()) { log = in.readAllBytes(); }
        if (process.waitFor() != 0) throw new IOException("Stock Flatpak failed: " + new String(log, java.nio.charset.StandardCharsets.UTF_8));
    }

}
