package org.matonos.linux.stub;

import android.app.Service;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.util.Log;
import org.matonos.linuxhost.stubgen.PackLaunch;
import org.matonos.linuxhost.stubgen.StubGenerator;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/**
 * Applies this package's Flatpak extra data, under the package's own UID.
 *
 * <p>Declared by every generated app stub and runtime package. An
 * {@code apply_extra} is arbitrary vendor code, so the installer never runs
 * it: it is deferred here, where the process already belongs to the package's
 * UID and SELinux domain, and the result is handed back to the subsystem app
 * as an archive so it can be published as an ordinary content split.
 *
 * <p>The mount point comes from the service's own {@code EXTRA_MOUNT}
 * metadata ({@code /app} for apps, the runtime's mount point otherwise). The
 * package's packs are mounted read-only there through the v3 loader and its
 * raw downloads are copied into a writable directory mapped over
 * {@code <mount>/extra}, exactly where Flatpak's deploy-time step would have
 * put them. The script then runs with that directory as its working
 * directory; whatever it leaves there is the applied extra tree.
 */
public final class ExtraService extends Service {
    private static final String TAG = "MatonExtraService";
    /** The only caller allowed: the subsystem app that owns the installer. */
    private static final String HOST_PACKAGE = "org.matonos.linux";
    /** A stuck vendor script must not pin the package forever; the subsystem
     * app waits slightly longer and reports the timeout. */
    private static final long TIMEOUT_MS = 14 * 60 * 1000;
    private static final int MAX_DIAGNOSTIC = 4096;

    private final IExtraExec.Stub binder = new IExtraExec.Stub() {
        @Override public void applyExtra(ParcelFileDescriptor output) {
            // Authorize on the binder thread: Binder.getCallingUid() is
            // thread-local and would read the worker's own UID otherwise.
            authorize();
            Thread worker = new Thread(() -> serve(output), "extra-apply");
            worker.setDaemon(true);
            // The service returns at once so the subsystem app can read the
            // pipe while this thread writes; a blocking call would deadlock on
            // the pipe buffer.
            worker.start();
        }
    };

    /** Runs one apply_extra and streams its result; always closes the pipe. */
    private void serve(ParcelFileDescriptor output) {
        try (OutputStream stream = new ParcelFileDescriptor.AutoCloseOutputStream(output)) {
            try {
                File work = prepare();
                ApplicationInfo info = getApplicationInfo();
                byte[] index = readIndex(info);
                Map<String, String> carriers = carriers(info);
                if (!ExtraPayload.materialize(index, carriers, work)) {
                    stream.write(ExtraProtocol.STATUS_NONE);
                    return;
                }
                if (ExtraPayload.hasFile(index, ExtraPayload.APPLY_EXTRA)) runApplyExtra(info, work);
                stream.write(ExtraProtocol.STATUS_OK);
                Tar.write(work, stream);
            } catch (Throwable failure) {
                Log.e(TAG, "apply_extra failed for " + getPackageName(), failure);
                try {
                    stream.write(ExtraProtocol.STATUS_ERROR);
                    byte[] message = message(failure);
                    stream.write((message.length >>> 24) & 255);
                    stream.write((message.length >>> 16) & 255);
                    stream.write((message.length >>> 8) & 255);
                    stream.write(message.length & 255);
                    stream.write(message);
                } catch (IOException ignored) { }
            }
        } catch (IOException ignored) {
            // The subsystem app stopped reading (cancelled or timed out).
        }
    }

    @Override public IBinder onBind(Intent intent) {
        // The caller is checked per call, not here: onBind's binder identity is
        // the system's, while applyExtra is a direct call from the subsystem app.
        return binder;
    }

    /** Only the subsystem app may drive a package's own extra step. */
    private void authorize() {
        int caller = Binder.getCallingUid();
        try {
            ApplicationInfo host = getPackageManager().getApplicationInfo(HOST_PACKAGE, 0);
            if (caller == host.uid) return;
        } catch (PackageManager.NameNotFoundException missing) {
            throw new SecurityException("The Linux subsystem app is not installed");
        }
        throw new SecurityException("ExtraService is reserved for the Linux subsystem app");
    }

    /** Always starts from an empty, package-private working directory. */
    private File prepare() throws IOException {
        File work = new File(getDataDir(), "extra").getCanonicalFile();
        if (!work.getParentFile().equals(getDataDir().getCanonicalFile()))
            throw new IOException("Invalid extra working directory");
        remove(work);
        if (!work.mkdirs()) throw new IOException("Cannot create extra working directory");
        return work;
    }

    private static void remove(File tree) {
        if (!java.nio.file.Files.isSymbolicLink(tree.toPath()) && tree.isDirectory()) {
            File[] entries = tree.listFiles();
            if (entries != null) for (File entry : entries) remove(entry);
        }
        //noinspection ResultOfMethodCallIgnored
        tree.delete();
    }

    /** The package's index split, embedded in the {@code index_} APK. */
    private static byte[] readIndex(ApplicationInfo info) throws IOException {
        String[] names = info.splitNames;
        String[] paths = info.splitSourceDirs;
        if (names == null || paths == null || names.length != paths.length)
            throw new IOException("Missing installed content splits");
        String index = null;
        for (int i = 0; i < names.length; i++) {
            if (!names[i].matches("index_[0-9a-f]{64}")) continue;
            if (index != null) throw new IOException("Multiple index splits");
            index = paths[i];
        }
        if (index == null) throw new IOException("Missing index split");
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(index)) {
            java.util.zip.ZipEntry entry = zip.getEntry("assets/matonos/objects.index");
            if (entry == null) throw new IOException("Index split has no index");
            try (java.io.InputStream in = zip.getInputStream(entry)) { return in.readAllBytes(); }
        }
    }

    /** Indexed pack name to the installed {@code pack_} split that carries it. */
    private static Map<String, String> carriers(ApplicationInfo info) throws IOException {
        Map<String, String> carriers = new HashMap<>();
        String[] names = info.splitNames;
        String[] paths = info.splitSourceDirs;
        if (names == null || paths == null) return carriers;
        for (int i = 0; i < names.length && i < paths.length; i++)
            if (names[i].matches("pack_[0-9a-f]{64}"))
                carriers.put("pack-" + names[i].substring(5) + ".zip", paths[i]);
        return carriers;
    }

    private String mountPoint() throws IOException {
        android.content.pm.ServiceInfo info;
        try {
            info = getPackageManager().getServiceInfo(
                    new android.content.ComponentName(this, ExtraService.class),
                    PackageManager.GET_META_DATA);
        } catch (PackageManager.NameNotFoundException missing) {
            throw new IOException("The package declares no extra service", missing);
        }
        String mount = info.metaData == null ? null : info.metaData.getString(StubGenerator.EXTRA_MOUNT_META);
        if (mount == null) mount = StubGenerator.APP_MOUNT;
        if (!mount.matches("/(?:[A-Za-z0-9_.+-]+/?)*")) throw new IOException("Invalid extra mount point");
        return mount;
    }

    /**
     * Runs {@code <mount>/bin/apply_extra} through the v3 loader this package
     * already uses, with its packs mounted at {@code <mount>} and the writable
     * working directory mapped over {@code <mount>/extra}.
     *
     * <p>Deliberate deviations from Flatpak's deploy-time step, documented in
     * {@code docs/FLATPAK-EXTRA-DATA.md}: no bubblewrap sandbox, and the
     * loader runs directly rather than through {@code matonos-app-flatpak run}.
     */
    private void runApplyExtra(ApplicationInfo info, File work) throws Exception {
        String mount = mountPoint();
        File cache = getCacheDir().getCanonicalFile();
        ArrayList<String> command = new ArrayList<>();
        command.add(LoaderCommand.loader(this).getAbsolutePath());
        command.add("--parent-pid"); command.add(Integer.toString(Process.myPid()));
        command.addAll(PackLaunch.arguments(new File(info.sourceDir), info.splitNames,
                info.splitSourceDirs, cache, mount));
        command.add("--map"); command.add(work.getAbsolutePath()); command.add(mount + "/extra");
        command.add(mount + "/bin/apply_extra");
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        Map<String, String> env = builder.environment();
        env.clear();
        env.put("PATH", "/system/bin");
        env.put("LD_LIBRARY_PATH", "/system/lib64");
        env.put("LC_ALL", "C");
        env.put("HOME", getDataDir().getAbsolutePath());
        env.put("TMPDIR", work.getAbsolutePath());
        builder.directory(work);
        java.lang.Process child = builder.start();
        ScheduledExecutorService watchdog = java.util.concurrent.Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "extra-timeout");
            thread.setDaemon(true);
            return thread;
        });
        ScheduledFuture<?> kill = watchdog.schedule(() -> {
            Log.w(TAG, "apply_extra timed out for " + getPackageName());
            child.destroyForcibly();
        }, TIMEOUT_MS, TimeUnit.MILLISECONDS);
        StringBuilder output = new StringBuilder();
        try (BufferedReader lines = new BufferedReader(
                new InputStreamReader(child.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = lines.readLine()) != null) {
                if (output.length() < MAX_DIAGNOSTIC) output.append(line).append('\n');
                Log.i(TAG, line);
            }
        }
        int status = child.waitFor();
        kill.cancel(false);
        watchdog.shutdownNow();
        if (status != 0)
            throw new IOException("apply_extra failed (" + status + "): " + output.toString().trim());
    }

    private static byte[] message(Throwable failure) {
        String text = failure.getMessage() == null ? failure.toString() : failure.getMessage();
        byte[] bytes = text.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > ExtraProtocol.MAX_MESSAGE_BYTES)
            bytes = java.util.Arrays.copyOf(bytes, ExtraProtocol.MAX_MESSAGE_BYTES);
        return bytes;
    }
}
