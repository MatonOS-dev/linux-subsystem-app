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

/** Downloads and verifies an authorized Flatpak transaction in subsystem-private data.
 * Flatpak checks signatures while pulling; OSTree fsck checks content before packs
 * are produced. Extra-data execution remains in the generated package's own UID. */
public final class InstallTransaction {
    private static final String TAG = "MatonInstall";
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
        if (!"flathub".equals(remote))
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
            ArrayList<String> remoteCommand=new ArrayList<>(command.subList(0,command.indexOf("install")));
            java.util.Collections.addAll(remoteCommand,"remote-add","--system","--if-not-exists","flathub",
                    "/apex/com.matonos.flatpak/etc/flatpak/remotes.d/flathub.flatpakrepo");
            builder.command(remoteCommand);
            Process setup=builder.start();process=setup;
            try(java.io.InputStream output=setup.getInputStream()){output.transferTo(java.io.OutputStream.nullOutputStream());}
            if(setup.waitFor()!=0)throw new java.io.IOException("Cannot configure authenticated Flathub remote");
            if(cancelled.get())throw new java.io.IOException("Install cancelled");
            builder.command(command);
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
            // No worker writes staging after the completion callback.
            DnsForwarder.release();
            StagedPacks.verify(context,installation,() -> cancelled.get());
            JSONObject manifest = DeployManifest.build(installation, appRef, remote, operationId);
            callback.onComplete(operationId, manifest.toString());
        } catch (Throwable error) {
            Log.e(TAG, "Install worker failed for " + appRef, error);
            fail(error.getMessage() == null ? error.toString() : error.getMessage());
        } finally {
            // The forwarder socket lives in this operation's run/ directory, so
            // it must not outlive the worker on any path, cancellation
            // included. Success already
            // released it before reporting completion.
            DnsForwarder.release();
        }
    }

    public static File installation(Context context,String id) throws Exception {
        return new File(operation(context,id),"installation");
    }
    private static File operation(Context context,String id) throws Exception {
        if(id==null||!OPERATION_ID.matcher(id).matches())throw new IllegalArgumentException("Invalid operation ID");
        File parent=new File(context.getCacheDir().getCanonicalFile(),"flatpak-staging");
        return new File(parent,id);
    }
    public static void prepare(Context context,String id) throws Exception {
        File operation=operation(context,id);
        // A reused caller ID must never adopt a prior interrupted transaction.
        cleanup(context,id);
        for(String name:new String[]{"installation","home","cache","tmp","run"})
            if(!new File(operation,name).mkdirs())throw new java.io.IOException("Cannot create private staging "+name);
    }
    public static void cleanup(Context context,String id) {
        try {
            java.nio.file.Files.walkFileTree(operation(context,id).toPath(),new java.nio.file.SimpleFileVisitor<java.nio.file.Path>() {
                @Override public java.nio.file.FileVisitResult visitFile(java.nio.file.Path p,java.nio.file.attribute.BasicFileAttributes a) throws java.io.IOException {Files.delete(p);return java.nio.file.FileVisitResult.CONTINUE;}
                @Override public java.nio.file.FileVisitResult postVisitDirectory(java.nio.file.Path p,java.io.IOException e) throws java.io.IOException {if(e!=null)throw e;Files.delete(p);return java.nio.file.FileVisitResult.CONTINUE;}
            });
        }catch(java.nio.file.NoSuchFileException absent) { }
        catch(Exception error){Log.w(TAG,"Cannot clean private staging",error);}
    }
    private File operationDirectory() throws Exception {return operation(context,operationId);}

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
