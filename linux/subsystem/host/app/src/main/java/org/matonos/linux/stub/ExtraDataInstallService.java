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
    private static final android.os.IBinder EROFS_TOKEN = new android.os.Binder();

    /** Binder call 1: result pipe fd, ref, remote, url, subpaths. FDs cannot travel in service Intents. */
    @Override public IBinder onBind(Intent intent) {
        return new android.os.Binder() {
            @Override protected boolean onTransact(int code, android.os.Parcel data, android.os.Parcel reply, int flags)
                    throws android.os.RemoteException {
                if (code == 2) {
                    File root=new File(getDataDir(), "install");
                    if (new File(root, ".erofs-handback").isFile()) deleteTree(root);
                    return true;
                }
                if (code != 1) return super.onTransact(code, data, reply, flags);
                ParcelFileDescriptor result = data.readFileDescriptor();
                Intent request = new Intent(ACTION).putExtra("ref", data.readString()).putExtra("remote", data.readString())
                        .putExtra("url", data.readString()).putExtra("subpaths", data.readString())
                        .putExtra("runtime", data.readString());
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
        // This exact app-private root is covered by compat-proxy's generic
        // OSTree hardlink emulation. Keep it until the returned image validates.
        File root = new File(getDataDir(), "install");
        deleteTree(root);
        new File(getDataDir(), "extra-data-image.erofs").delete();
        File home = new File(root, "home"), tmp = new File(root, "tmp"), run = new File(root, "run"), cache = new File(root, "cache");
        for (File dir : new File[]{root, home, tmp, run, cache}) if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("Cannot create Flatpak directory");
        File userData = new File(root, "xdg-data");
        File userConfig = new File(root, "xdg-config");
        userData.mkdirs(); userConfig.mkdirs();
        File flatpakUser = new File(userData, "flatpak");
        File flatpakSystem = null;
        boolean erofsAvailable = ErofsLaunch.available();
        if (erofsAvailable) flatpakSystem = prepareRuntimeSystem(root, request.getStringExtra("runtime"));
        String dns=LoaderCommand.resolverAddress();
        File resolv=LoaderCommand.writeResolv(new File(run,"install-resolv.conf"),dns);
        List<String> prefix=new ArrayList<>();
        prefix.add(LoaderCommand.loader(this).getAbsolutePath());
        if (new File(getCacheDir(), "v3-trace").exists()) prefix.add("--trace");
        prefix.add("--parent-pid");
        prefix.add(Integer.toString(android.os.Process.myPid()));LoaderCommand.addDns(prefix,dns);LoaderCommand.addResolvMaps(prefix,resolv);
        runFlatpak(prefix,home, tmp, run, cache, flatpakUser, flatpakSystem, "remote-add", "--user", "--if-not-exists", remote, url);
        String tags=android.os.LocaleList.getDefault().toLanguageTags();
        List<String> localeTags=new ArrayList<>();if(tags!=null&&!tags.isEmpty())java.util.Collections.addAll(localeTags,tags.split(","));
        runFlatpak(prefix,home,tmp,run,cache,flatpakUser,flatpakSystem,"config","--user","--set","languages",
                org.matonos.linux.stub.FlatpakLanguages.configuration(localeTags));
        List<String> install = new ArrayList<>();
        install.add("install"); install.add("--user"); install.add("--assumeyes"); install.add("--no-related");
        String subpaths = request.getStringExtra("subpaths");
        if (subpaths != null) for (String subpath : subpaths.split("\n")) if (!subpath.isEmpty()) install.add("--subpath=" + subpath);
        install.add(remote); install.add(ref);
        runFlatpak(prefix,home, tmp, run, cache, flatpakUser, flatpakSystem, install.toArray(new String[0]));
        // Stock Flatpak layout: <installation>/<kind>/<id>/<arch>/<branch>/active/files.
        File current = new File(flatpakUser, ref + "/active/files");
        if (!current.isDirectory()) throw new IOException("Flatpak did not create a deployed files tree");
        String imagePrefix = ref.startsWith("runtime/")
                ? "/" + ref.substring(ref.lastIndexOf('/') + 1) + "/" : "/app/";
        List<ObjectPacks.Entry> entries = ObjectPacks.fromTreeWithModes(current, imagePrefix);
        File output = new File(root, "deployment.erofs");
        File handback = output;
        File mkfs = LoaderCommand.tool(this, "libmatonos-mkfs-erofs.so");
        ErofsImage.build(entries, output.getParentFile(), mkfs, System.currentTimeMillis() / 1000L);
        if (erofsAvailable) {
            List<ExtraImageValidator.Entry> trusted = new ArrayList<>();
            for (ObjectPacks.Entry entry : entries) trusted.add(new ExtraImageValidator.Entry(
                    entry.path, entry.kind, entry.mode, entry.target, entry.checksum, entry.source));
            File checked = new File(root, "image-validated");
            try {
                ExtraImageValidator.checkImage(LoaderCommand.tool(this, "libmatonos-fsck-erofs.so"),
                        output, checked, trusted);
            } finally { deleteTree(checked); }
            if (!new File(root, ".erofs-handback").createNewFile())
                throw new IOException("Cannot mark validated EROFS handback");
            // The host transaction validates it against the pulled commit
            // before asking this service to remove install/.
        } else {
            // TODO: launch directly from this full app+runtime installation
            // when matonos-erofsd is absent; retain install/ as the fallback.
        }
        try (java.io.InputStream in = org.matonos.linuxhost.stubgen.PlaceholderFiles.open(handback)) {
            out.writeInt(0); out.writeLong(handback.length());
            byte[] buffer = new byte[65536]; int count;
            while ((count = in.read(buffer)) != -1) out.write(buffer, 0, count);
        }
    }

    /** The minimal stub has no deployment split yet; the subsystem names the
     *  app's runtime from the commit metadata it already verified. */
    private File prepareRuntimeSystem(File root, String runtimeRef) throws Exception {
        if (runtimeRef == null || !runtimeRef.matches("runtime/[A-Za-z0-9._+-]+/[A-Za-z0-9._+-]+/[A-Za-z0-9._+-]+"))
            throw new IOException("EROFS install requires the app's runtime ref");
        android.content.pm.PackageInfo self = getPackageManager().getPackageInfo(getPackageName(),
                android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES);
        String packageName = org.matonos.linux.stub.DeployManifest.packageForPublic(runtimeRef);
        android.content.pm.PackageInfo pkg = getPackageManager().getPackageInfo(packageName,
                android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES);
        android.content.pm.Signature[] selfSigners=self.signingInfo==null?null:self.signingInfo.getApkContentsSigners();
        android.content.pm.Signature[] runtimeSigners=pkg.signingInfo==null?null:pkg.signingInfo.getApkContentsSigners();
        if (self.signingInfo == null || pkg.signingInfo == null || self.signingInfo.hasMultipleSigners()
                || pkg.signingInfo.hasMultipleSigners() || selfSigners==null || selfSigners.length!=1
                || runtimeSigners==null || runtimeSigners.length!=1 || !selfSigners[0].equals(runtimeSigners[0]))
            throw new SecurityException("Untrusted installed Flatpak runtime");
        org.matonos.linuxhost.stubgen.DeploymentRecord runtime = org.matonos.linuxhost.stubgen.DeploymentRecord.installed(
                pkg.splitNames, pkg.applicationInfo.splitSourceDirs);
        if (!runtime.ref.equals(runtimeRef)) throw new IOException("Installed runtime ref mismatch");
        String mounted = ErofsLaunch.mountInstalledImage(pkg.splitNames, pkg.applicationInfo.splitSourceDirs, EROFS_TOKEN);
        File system = new File(root, "system");
        runtime.materialize(system, "/usr");
        File files = new File(system, runtimeRef + "/" + runtime.commit + "/files");
        if (!files.delete()) throw new IOException("Cannot replace runtime deployment payload");
        java.nio.file.Files.createSymbolicLink(files.toPath(), java.nio.file.Path.of(mounted,
                runtimeRef.substring(runtimeRef.lastIndexOf('/') + 1)));
        if (!files.exists())
            throw new IOException("Cannot expose installed EROFS runtime to Flatpak");
        makeReadOnly(system);
        return system;
    }

    private static void deleteTree(File file) {
        ExtraDataFiles.deleteTree(file);
    }

    private static void makeReadOnly(File file) throws IOException {
        if (java.nio.file.Files.isSymbolicLink(file.toPath())) return;
        if (file.isDirectory()) {
            File[] children=file.listFiles();
            if (children==null) throw new IOException("Cannot read Flatpak system installation");
            for (File child:children) makeReadOnly(child);
        }
        if (!file.setWritable(false,false)) throw new IOException("Cannot make Flatpak system installation read-only");
    }

    private void runFlatpak(List<String> prefix,File home, File tmp, File run, File cache, File userDir, File systemDir, String... args) throws Exception {
        List<String> command = new ArrayList<>();
        command.addAll(prefix);
        command.add(LoaderCommand.tool(this, LoaderCommand.FLATPAK_CLI).getAbsolutePath());
        if (new File(getCacheDir(), "v3-trace").exists()) command.add("--verbose");
        java.util.Collections.addAll(command, args);
        ProcessBuilder builder = new ProcessBuilder(command).directory(home).redirectErrorStream(true);
        java.util.Map<String,String> env = builder.environment(); env.clear();
        String nativeDir = LoaderCommand.nativeLibraryDir(this).getAbsolutePath();
        env.put("PATH", nativeDir + ":/system/bin"); env.put("LD_LIBRARY_PATH", nativeDir + ":/system/lib64");
        env.put("LC_ALL","C");env.put("HOME", home.getAbsolutePath()); env.put("TMPDIR", tmp.getAbsolutePath()); env.put("XDG_RUNTIME_DIR", run.getAbsolutePath());
        env.put("XDG_DATA_HOME", new File(home, ".local/share").getAbsolutePath()); env.put("XDG_CONFIG_HOME", new File(home, ".config").getAbsolutePath()); env.put("XDG_CACHE_HOME", cache.getAbsolutePath());
        env.put("FLATPAK_USER_DIR", userDir.getAbsolutePath()); env.put("FLATPAK_DOWNLOAD_TMPDIR", tmp.getAbsolutePath());
        if (systemDir != null) env.put("FLATPAK_SYSTEM_DIR", systemDir.getAbsolutePath());
        env.put("FLATPAK_SYSTEM_CACHE_DIR",cache.getAbsolutePath());
        for(String key:new String[]{"SSL_CERT_DIR","CURL_CA_BUNDLE","G_TLS_CA_PATH"})env.put(key,"/apex/com.android.conscrypt/cacerts");
        Process process = builder.start(); StringBuilder log = new StringBuilder();
        // Stream evidence while the operation is running, including successful
        // apply_extra output; full proxy traces must not wait for process exit.
        try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(
                process.getInputStream(), java.nio.charset.StandardCharsets.UTF_8))) {
            String line;
            while ((line = in.readLine()) != null) {
                android.util.Log.i(TAG, line);
                log.append(line).append('\n');
            }
        }
        if (process.waitFor() != 0) throw new IOException("Stock Flatpak failed: " + log);
    }

}
