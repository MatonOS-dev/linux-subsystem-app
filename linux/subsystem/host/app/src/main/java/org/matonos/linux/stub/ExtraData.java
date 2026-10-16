package org.matonos.linux.stub;

import android.content.Context;
import android.util.Log;
import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * Deferred Flatpak extra data, applied by the application's own stub.
 *
 * <p>An app's extra data is arbitrary vendor code and download payload, so the
 * installer never runs {@code apply_extra}: the signed commit's checksum-verified
 * downloads stay in {@code <deployment>/files/extra}. This stub, which already
 * runs the application in its own package UID and SELinux domain, copies them
 * into its private data directory, runs {@code /app/bin/apply_extra} there once
 * per installed revision, and the launcher then maps the result over
 * {@code /app/extra}. Uninstalling the app removes the output with its data.
 *
 * <p>Flatpak's own {@code extra/export} promotion (icons and desktop files
 * produced by {@code apply_extra}) is not reproduced: the published deployment
 * is linuxd-owned and read-only.
 */
final class ExtraData {
    /** Install/launch status, always delivered off the UI thread. */
    interface Progress { void status(String text); }

    private static final String TAG = "MatonExtraData";
    private static final Pattern COMMIT = Pattern.compile("[0-9a-f]{64}");
    /** linuxd's deployment pointer and our state marker. */
    private static final String ACTIVE = "active";
    private static final String STATE = "extra.state";
    /** Private extra root under the stub's own data directory. */
    private static final String DIRECTORY = "extra";
    /** Enough of a failing script's output to explain what went wrong. */
    private static final int MAX_DIAGNOSTIC = 4096;
    /** The runtime installation root every launch uses. */
    private static final String RUNTIME_ROOT = "/data/linux-app/runtime";

    private ExtraData() { }

    /**
     * Resolves the app's deferred extra data and returns the directory to map
     * over {@code /app/extra}, or {@code null} when the app has none.
     *
     * <p>{@code version} is the pinned installation pointer the launch holds a
     * lease on; the current commit comes from linuxd's {@code active} link.
     */
    static File prepare(Context context, File version, String appRef, File home, File cache,
            File runtimeDirectory, Progress progress) throws Exception {
        File appDirectory = new File(version, appRef);
        String commit = activeCommit(appDirectory);
        if (commit == null) return null;
        File source = unpacked(new File(appDirectory, commit));
        File baseline = context.getDataDir().getCanonicalFile();
        File extra = new File(baseline, DIRECTORY);
        File state = new File(baseline, STATE);
        if (source == null) { forget(extra, state); return null; }
        if (extra.isDirectory() && commit.equals(recorded(state))) return extra;
        // An update, a changed deployment or a failed earlier attempt always
        // regenerates from scratch, so no stale output can survive.
        forget(extra, state);
        progress.status("Preparing application files\u2026");
        copy(source, extra);
        if (new File(appDirectory, commit + "/files/bin/apply_extra").isFile()) {
            progress.status("Extracting application files\u2026");
            apply(context, version, extra, home, cache, runtimeDirectory,
                    appRef.substring(4), commit, progress);
        }
        write(state, commit);
        return extra;
    }

    /**
     * The commit linuxd's {@code active} link names, or null when the app
     * directory has no usable pointer.
     */
    static String activeCommit(File appDirectory) {
        Path active = new File(appDirectory, ACTIVE).toPath();
        try {
            if (!Files.isSymbolicLink(active)) return null;
            String target = Files.readSymbolicLink(active).toString();
            return COMMIT.matcher(target).matches() ? target : null;
        } catch (IOException unreadable) {
            Log.w(TAG, "Cannot read the deployment pointer for " + appDirectory, unreadable);
            return null;
        }
    }

    /** The deployment's unpacked extra downloads, or null when it has none. */
    static File unpacked(File deployment) {
        File extra = new File(deployment, "files/extra");
        File[] entries = Files.isDirectory(extra.toPath(), LinkOption.NOFOLLOW_LINKS)
                ? extra.listFiles() : null;
        return entries != null && entries.length > 0 ? extra : null;
    }

    /** The commit recorded for the private extra directory, or null. */
    static String recorded(File state) {
        Path path = state.toPath();
        try {
            if (!Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS)) return null;
            return new String(Files.readAllBytes(path), StandardCharsets.UTF_8).trim();
        } catch (IOException unreadable) {
            return null;
        }
    }

    private static void write(File state, String commit) throws IOException {
        Files.write(state.toPath(), (commit + "\n").getBytes(StandardCharsets.UTF_8));
    }

    /** Drops the private output and its marker; both are always regenerable. */
    static void forget(File extra, File state) {
        remove(extra);
        state.delete();
    }

    /** Removes a tree without following symlinks. */
    static void remove(File tree) {
        Path path = tree.toPath();
        if (!Files.isSymbolicLink(path) && Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)) {
            File[] entries = tree.listFiles();
            if (entries != null) for (File entry : entries) remove(entry);
        }
        if (!tree.delete() && tree.exists()) Log.w(TAG, "Cannot remove " + tree);
    }

    /**
     * Copies the deployment's unsigned extra tree into the stub's private
     * directory: regular files and directories only, symlinks recreated but
     * never followed, anything else rejected.
     */
    static void copy(File source, File destination) throws IOException {
        if (!destination.isDirectory() && !destination.mkdirs())
            throw new IOException("Cannot create " + destination);
        File[] entries = source.listFiles();
        if (entries == null) throw new IOException("Cannot list " + source);
        for (File entry : entries) {
            Path from = entry.toPath();
            File to = new File(destination, entry.getName());
            if (Files.isSymbolicLink(from)) {
                Path target = Files.readSymbolicLink(from);
                if (target.isAbsolute() || escapes(target))
                    throw new IOException("Unsafe extra-data link " + entry.getName());
                Files.createSymbolicLink(to.toPath(), target);
            } else if (Files.isDirectory(from, LinkOption.NOFOLLOW_LINKS)) {
                copy(entry, to);
            } else if (Files.isRegularFile(from, LinkOption.NOFOLLOW_LINKS)) {
                Files.copy(from, to.toPath());
                // The script may replace or remove its own download.
                if (!to.setWritable(true, true)) throw new IOException("Cannot write " + to);
            } else {
                throw new IOException("Unsupported extra-data entry " + entry.getName());
            }
        }
    }

    /** True when a relative link could resolve outside its own tree. */
    static boolean escapes(Path target) {
        for (Path part : target) if (part.toString().equals("..")) return true;
        return false;
    }

    /**
     * Runs the deployment's {@code apply_extra} through the same v3 loader the
     * app uses, at the stub's UID, with the private directory mapped over
     * {@code /app/extra}. The app's own sandbox permissions do not apply: this
     * is upstream's restricted apply sandbox with no bus, host or devices.
     */
    private static void apply(Context context, File version, File extra, File home, File cache,
            File runtimeDirectory, String appId, String commit, Progress progress) throws Exception {
        ArrayList<String> command = new ArrayList<>();
        command.add(LoaderCommand.loader(context).getAbsolutePath());
        command.add("--parent-pid"); command.add(Integer.toString(android.os.Process.myPid()));
        command.add("--map"); command.add(extra.getAbsolutePath()); command.add("/app/extra");
        command.add(LoaderCommand.FLATPAK_BIN + "/matonos-app-flatpak");
        command.add("--user"); command.add("run");
        command.add("--sandbox"); command.add("--no-documents-portal");
        command.add("--nofilesystem=host"); command.add("--nofilesystem=/tmp");
        command.add("--nodevice=all");
        command.add("--command=/app/bin/apply_extra"); command.add("--cwd=/app/extra");
        command.add(appId);
        ProcessBuilder builder = new ProcessBuilder(command).redirectErrorStream(true);
        Map<String, String> env = builder.environment();
        env.clear();
        env.put("FLATPAK_USER_DIR", version.getAbsolutePath());
        env.put("FLATPAK_SYSTEM_DIR", RUNTIME_ROOT);
        env.put("PATH", LoaderCommand.FLATPAK_BIN + ":/system/bin");
        env.put("LD_LIBRARY_PATH", "/apex/com.matonos.flatpak/lib64:/system/lib64");
        env.put("HOME", home.getAbsolutePath());
        env.put("TMPDIR", cache.getAbsolutePath());
        env.put("FLATPAK_DOWNLOAD_TMPDIR", cache.getAbsolutePath());
        env.put("FLATPAK_SYSTEM_CACHE_DIR", cache.getAbsolutePath());
        env.put("XDG_CACHE_HOME", cache.getAbsolutePath());
        env.put("XDG_CONFIG_HOME", new File(home, ".config").getAbsolutePath());
        env.put("XDG_DATA_HOME", new File(home, ".local/share").getAbsolutePath());
        env.put("XDG_RUNTIME_DIR", runtimeDirectory.getAbsolutePath());
        builder.directory(home);
        Process child = builder.start();
        StringBuilder output = new StringBuilder();
        try (BufferedReader lines = new BufferedReader(
                new InputStreamReader(child.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = lines.readLine()) != null) {
                if (output.length() < MAX_DIAGNOSTIC) output.append(line).append('\n');
                progress.status(line);
            }
        }
        int status = child.waitFor();
        if (status != 0) {
            Log.e(TAG, "apply_extra failed for " + appId + " at " + commit + ": " + output);
            throw new IOException("apply_extra failed (" + status + "): " + output.toString().trim());
        }
    }
}
