package org.matonos.linux.stub;

import android.content.Context;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import java.io.BufferedReader;
import java.io.File;
import java.io.InputStreamReader;

/** Stage-one JNI entry: no fork/exec, no process-wide cwd, environment or fd edits. */
final class InProcessLauncher {
    static final String SELF_TEST = "org.matonos.linux.IN_PROCESS_SELF_TEST";
    static final String LIFECYCLE_TEST = "org.matonos.linux.IN_PROCESS_LIFECYCLE_TEST";
    static final String SELF_TEST_ID = "org.matonos.linux.IN_PROCESS_TEST_ID";
    private static boolean loaded;
    private static synchronized void load(Context context) throws Exception {
        if (!loaded) {
            System.load(new File(LoaderCommand.nativeLibraryDir(context), "libmatonos-inprocess.so").getAbsolutePath());
            loaded = true;
        }
    }
    private static native int nativeRunInProcess(String[] argv, String[] env, int[] fds);

    /** Test-only fixed payloads. Production launch switches in stage seven. */
    static void lifecycleTest(android.app.Activity activity, String mode, String testId) {
        if (!java.util.Arrays.asList("clean", "fault", "group-main", "group-worker", "thread-main").contains(mode))
            throw new IllegalArgumentException("Unknown lifecycle fixture");
        android.content.SharedPreferences state = activity.getSharedPreferences("inprocess-lifecycle", 0);
        if (testId == null) throw new IllegalArgumentException("Lifecycle test requires an attempt id");
        if (testId.equals(state.getString("attempt", ""))) {
            // ActivityManager can recreate a crashed activity. A Linux death
            // must not leave that replacement process hosting an idle stub.
            Log.i("MatonInProcess", "Lifecycle removing recreated task id=" + testId);
            activity.finishAndRemoveTask();
            android.os.Process.killProcess(android.os.Process.myPid());
            return;
        }
        if (!state.edit().putString("attempt", testId).remove("exit").commit()) return;
        new Thread(() -> {
            try {
                load(activity);
                File dir = LoaderCommand.nativeLibraryDir(activity);
                String executable = new File(dir, "libmatonos-inprocess-glibc.so").getAbsolutePath();
                String[] argv = {new File(dir, "libmatonos-inprocess-ld.so").getAbsolutePath(),
                        "--inhibit-cache", "--library-path", dir.getAbsolutePath(), executable, "fixture", mode};
                Log.i("MatonInProcess", "Lifecycle start mode=" + mode + " id=" + testId +
                        " ART pid=" + android.os.Process.myPid());
                // Use an owned /dev/null fd; never duplicate over ART's stdio.
                try (ParcelFileDescriptor sink = ParcelFileDescriptor.open(new File("/dev/null"),
                        ParcelFileDescriptor.MODE_WRITE_ONLY)) {
                    int status = nativeRunInProcess(argv, new String[]{"INPROCESS_TEST=present"},
                            new int[]{sink.getFd(), sink.getFd()});
                    if (!state.edit().putInt("exit", status).commit())
                        throw new java.io.IOException("Cannot record Linux exit");
                    Log.i("MatonInProcess", "Lifecycle cleaned mode=" + mode + " status=" + status +
                            " ART pid=" + android.os.Process.myPid());
                }
                activity.runOnUiThread(() -> {
                    try {
                        org.matonos.linux.PerAppRuntime runtime = StubService.runtime();
                        if (runtime != null) runtime.close();
                        activity.finishAndRemoveTask();
                        Log.i("MatonInProcess", "Lifecycle ending Android process mode=" + mode);
                    } finally { android.os.Process.killProcess(android.os.Process.myPid()); }
                });
            } catch (Throwable failure) {
                Log.e("MatonInProcess", "Lifecycle failed mode=" + mode, failure);
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        }, "inprocess-lifecycle").start();
    }

    static void selfTest(Context context, String testId, java.util.function.Consumer<String> result) {
        final String id = testId == null ? "default" : testId;
        android.content.SharedPreferences state = context.getSharedPreferences("inprocess-diagnostic", 0);
        if (id.equals(state.getString("attempt", ""))) {
            result.accept("Diagnostic already attempted; use a new IN_PROCESS_TEST_ID to run again.");
            return;
        }
        if (!state.edit().putString("attempt", id).commit()) {
            result.accept("Cannot persist diagnostic attempt.");
            return;
        }
        new Thread(() -> {
            try {
                load(context);
                File dir = LoaderCommand.nativeLibraryDir(context);
                final int pid = android.os.Process.myPid();
                for (int run = 0; run < 2; run++) {
                    for (String mode : new String[]{"static", "glibc", "exec"}) {
                        ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
                        Thread output = new Thread(() -> {
                            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                                    new ParcelFileDescriptor.AutoCloseInputStream(pipe[0])))) {
                                String line;
                                while ((line = reader.readLine()) != null) Log.i("MatonInProcess", line);
                            } catch (Exception failure) { Log.e("MatonInProcess", "Output failed", failure); }
                        }, "inprocess-output");
                        output.start();
                        String executable = new File(dir, "libmatonos-inprocess-" + (mode.equals("exec") ? "glibc" : mode) + ".so").getAbsolutePath();
                        String[] argv = mode.equals("static") ? new String[]{executable, "fixture"} :
                                new String[]{new File(dir, "libmatonos-inprocess-ld.so").getAbsolutePath(),
                                    "--inhibit-cache", "--library-path", dir.getAbsolutePath(), executable, "fixture"};
                        if (mode.equals("exec")) {
                            argv = new String[]{new File(dir, "libmatonos-inprocess-ld.so").getAbsolutePath(),
                                "--inhibit-cache", "--library-path", dir.getAbsolutePath(), executable, "fixture", "exec0",
                                new File(dir, "libmatonos-inprocess-ld.so").getAbsolutePath(), dir.getAbsolutePath(),
                                executable, new File(dir, "libmatonos-inprocess-static.so").getAbsolutePath()};
                        }
                        int status;
                        try {
                            status = nativeRunInProcess(argv, new String[]{"INPROCESS_TEST=present"},
                                    new int[]{pipe[1].getFd(), pipe[1].getFd()});
                        } finally { pipe[1].close(); }
                        output.join();
                        if (status != 0 || pid != android.os.Process.myPid())
                            throw new IllegalStateException(mode + " failed: " + status);
                        Log.i("MatonInProcess", "ART alive pid=" + pid + " run=" + run + " mode=" + mode + " status=" + status);
                    }
                }
                new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
                    Log.i("MatonInProcess", "Main looper responsive pid=" + pid);
                    result.accept("In-process tests passed; ART alive, pid=" + pid);
                });
            } catch (Throwable failure) {
                Log.e("MatonInProcess", "Self-test failed", failure);
                result.accept("In-process self-test failed: " + failure);
            }
        }, "inprocess-test").start();
    }
}
