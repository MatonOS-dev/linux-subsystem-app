package org.matonos.linux.stub;

import android.content.Context;
import android.util.Log;
import org.json.JSONObject;
import org.matonos.systembridge.ICompositorInstallCallback;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Map;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * One compositor-side Flatpak install into the linuxd-created staging
 * installation for this authorized operation.
 *
 * <p>The worker runs the shipped {@code matonos-app-flatpak} through the same
 * v3 loader the stub uses, so pull and deployment happen under this app's own
 * PackageManager UID. An application's {@code apply_extra} is deliberately not
 * run here: it is arbitrary vendor code, so it runs later under the stub's own
 * UID through the generated package's {@code ExtraService} (see
 * {@link ExtraService}) and its output is published as a content split. It
 * never chooses a path,
 * loader flag or final identity: the operation ID and ref/remote come from the
 * bridge, and the staging directory is derived from this process's own UID.
 * When the worker exits it closes everything it opened into staging, reports
 * the deployment manifest, and lets linuxd verify and publish.
 */
public final class InstallTransaction {
    private static final String TAG = "MatonInstall";
    /** Linuxd-owned staging root; the per-UID parent is not ours to write. */
    private static final String STAGING_ROOT = "/data/linux-app/staging";
    private static final java.util.regex.Pattern OPERATION_ID =
            java.util.regex.Pattern.compile("[A-Za-z0-9_-]{1,64}");
    private static final int MAX_LINE = 2048;

    /** Notified exactly once, on the worker thread, after the worker is done. */
    public interface Completion { void workerFinished(); }

    private final Context context;
    private final ICompositorInstallCallback callback;
    private final String operationId;
    private final String appRef;
    private final String remote;
    private final AtomicBoolean cancelled = new AtomicBoolean();
    private volatile Process process;

    private InstallTransaction(Context context, ICompositorInstallCallback callback,
            String operationId, String appRef, String remote) {
        this.context = context;
        this.callback = callback;
        this.operationId = operationId;
        this.appRef = appRef;
        this.remote = remote;
    }

    /** Validates everything the bridge may pass before any staging is touched. */
    public static InstallTransaction create(Context context, ICompositorInstallCallback callback,
            String operationId, String appRef, String remote) {
        if (operationId == null || !OPERATION_ID.matcher(operationId).matches())
            throw new IllegalArgumentException("Invalid operation ID");
        if (appRef == null || !appRef.startsWith("app/") || !DeployManifest.validRef(appRef))
            throw new IllegalArgumentException("Invalid application reference");
        if (remote == null || !remote.matches("[A-Za-z0-9._-]{1,128}"))
            throw new IllegalArgumentException("Invalid remote");
        if (callback == null) throw new IllegalArgumentException("A completion callback is required");
        return new InstallTransaction(context, callback, operationId, appRef, remote);
    }

    String appRef() { return appRef; }

    String operation() { return operationId; }

    public void start(Completion completion) {
        Thread worker = new Thread(() -> {
            try { install(); }
            finally { completion.workerFinished(); }
        }, "maton-install");
        worker.setDaemon(true);
        worker.start();
    }

    /** Stops this operation's worker. Descendants are reaped with the compositor. */
    public void cancel(String operation) {
        if (operationId.equals(operation)) cancel();
    }

    /** Stops this operation's worker regardless of how it is addressed. */
    public void cancel() {
        cancelled.set(true);
        Process child = process;
        if (child != null) child.destroyForcibly();
    }

    private void install() {
        try {
            File operation = operationDirectory();
            File installation = requireDirectory(operation, "installation");
            File home = requireDirectory(operation, "home");
            File cache = requireDirectory(operation, "cache");
            File tmp = requireDirectory(operation, "tmp");
            File run = requireDirectory(operation, "run");
            String dns = LoaderCommand.resolverAddress();
            File resolv = new File(run, "install-resolv.conf");
            LoaderCommand.writeResolv(resolv, dns);
            ArrayList<String> command = new ArrayList<>();
            command.add(LoaderCommand.loader(context).getAbsolutePath());
            // The loader dies with this app process, not with the worker thread.
            command.add("--parent-pid");
            command.add(Integer.toString(android.os.Process.myPid()));
            LoaderCommand.addDns(command, dns, run);
            LoaderCommand.addResolvMaps(command, resolv);
            command.add(LoaderCommand.FLATPAK_BIN + "/matonos-app-flatpak");
            command.add("install");
            command.add("--system");
            // --assumeyes keeps the unattended CLI that emits download progress.
            command.add("--assumeyes");
            command.add(remote);
            command.add(appRef);
            ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
            Map<String, String> env = builder.environment();
            env.clear();
            env.put("PATH", LoaderCommand.FLATPAK_BIN + ":/system/bin");
            env.put("LD_LIBRARY_PATH", "/apex/com.matonos.flatpak/lib64:/system/lib64");
            env.put("LC_ALL", "C");
            env.put("HOME", home.getAbsolutePath());
            env.put("TMPDIR", tmp.getAbsolutePath());
            env.put("XDG_RUNTIME_DIR", run.getAbsolutePath());
            env.put("XDG_DATA_HOME", new File(home, ".local/share").getAbsolutePath());
            env.put("XDG_CONFIG_HOME", new File(home, ".config").getAbsolutePath());
            env.put("XDG_CACHE_HOME", cache.getAbsolutePath());
            // The staging installation is this operation's private "system" and
            // "user" installation, never the shared runtime store.
            env.put("FLATPAK_SYSTEM_DIR", installation.getAbsolutePath());
            env.put("FLATPAK_USER_DIR", installation.getAbsolutePath());
            env.put("FLATPAK_SYSTEM_CACHE_DIR", cache.getAbsolutePath());
            env.put("FLATPAK_DOWNLOAD_TMPDIR", tmp.getAbsolutePath());
            env.put("SSL_CERT_DIR", "/apex/com.android.conscrypt/cacerts");
            env.put("CURL_CA_BUNDLE", "/apex/com.android.conscrypt/cacerts");
            env.put("G_TLS_CA_PATH", "/apex/com.android.conscrypt/cacerts");
            builder.directory(home);
            Process child = builder.start();
            process = child;
            try (BufferedReader output = new BufferedReader(
                    new InputStreamReader(child.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while (!cancelled.get() && (line = output.readLine()) != null)
                    report("staging", percent(line), line);
            }
            int status = child.waitFor();
            process = null;
            if (cancelled.get()) { fail("Install cancelled"); return; }
            if (status != 0) { fail("Flatpak install failed (" + status + ")"); return; }
            // Release every handle this worker opened into staging before linuxd
            // takes the directory namespace and seals the regular files.
            DnsForwarder.release();
            JSONObject manifest = DeployManifest.build(installation, appRef, remote, operationId);
            callback.onComplete(operationId, manifest.toString());
        } catch (Throwable error) {
            Log.e(TAG, "Install worker failed for " + appRef, error);
            fail(error.getMessage() == null ? error.toString() : error.getMessage());
        } finally {
            // The forwarder socket lives in this operation's run/ directory, so
            // it must not outlive the worker on any path, cancellation
            // included. Release before linuxd seals the tree; success already
            // released it before reporting completion.
            DnsForwarder.release();
        }
    }

    /**
     * The linuxd-created operation directory: {@code <staging>/<uid>/<txn>}.
     * Java cannot open each component with O_NOFOLLOW, so every component is
     * compared canonically and each directory must also be owned by this UID.
     */
    private File operationDirectory() throws Exception {
        File parent = new File(STAGING_ROOT, Integer.toString(android.os.Process.myUid()));
        File operation = new File(parent, operationId).getCanonicalFile();
        if (!parent.getCanonicalPath().equals(operation.getParent()))
            throw new java.io.IOException("Invalid staging directory");
        if (!operation.isDirectory() || Files.isSymbolicLink(operation.toPath()))
            throw new java.io.IOException("The staging transaction is not ready");
        return operation;
    }

    private static File requireDirectory(File operation, String name) throws Exception {
        File directory = new File(operation, name).getCanonicalFile();
        if (!operation.getPath().equals(directory.getParent())
                || !directory.isDirectory() || Files.isSymbolicLink(directory.toPath()))
            throw new java.io.IOException("Missing staging directory " + name);
        android.system.StructStat stat = android.system.Os.lstat(directory.getPath());
        if (stat.st_uid != android.os.Process.myUid())
            throw new java.io.IOException("Unowned staging directory " + name);
        return directory;
    }

    /** Digits immediately before a {@code %} on this line, as the CLI reports it. */
    private static int percent(String line) {
        int mark = line.indexOf('%');
        if (mark <= 0) return -1;
        int begin = mark;
        while (begin > 0 && Character.isDigit(line.charAt(begin - 1))) begin--;
        if (begin == mark) return -1;
        try { return Math.min(100, Integer.parseInt(line.substring(begin, mark))); }
        catch (NumberFormatException ignored) { return -1; }
    }

    private void report(String phase, int percent, String line) {
        try {
            String text = line == null ? "" : line;
            callback.onProgress(operationId, phase, percent,
                    text.length() > MAX_LINE ? text.substring(0, MAX_LINE) : text);
        } catch (Exception error) { Log.w(TAG, "Cannot report install progress", error); }
    }

    private void fail(String message) {
        try { callback.onFailed(operationId, message); }
        catch (Exception error) { Log.w(TAG, "Cannot report install failure", error); }
    }
}
