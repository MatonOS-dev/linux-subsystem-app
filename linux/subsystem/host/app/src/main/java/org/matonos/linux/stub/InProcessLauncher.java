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
    static final String SELF_TEST_ID = "org.matonos.linux.IN_PROCESS_TEST_ID";
    private static boolean loaded;
    private static synchronized void load(Context context) throws Exception {
        if (!loaded) {
            System.load(new File(LoaderCommand.nativeLibraryDir(context), "libmatonos-inprocess.so").getAbsolutePath());
            loaded = true;
        }
    }
    private static native int nativeRunInProcess(String[] argv, String[] env, int[] fds);

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
                    for (String mode : new String[]{"static", "glibc"}) {
                        ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
                        Thread output = new Thread(() -> {
                            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                                    new ParcelFileDescriptor.AutoCloseInputStream(pipe[0])))) {
                                String line;
                                while ((line = reader.readLine()) != null) Log.i("MatonInProcess", line);
                            } catch (Exception failure) { Log.e("MatonInProcess", "Output failed", failure); }
                        }, "inprocess-output");
                        output.start();
                        String executable = new File(dir, "libmatonos-inprocess-" + mode + ".so").getAbsolutePath();
                        String[] argv = mode.equals("static") ? new String[]{executable, "fixture"} :
                                new String[]{new File(dir, "libmatonos-inprocess-ld.so").getAbsolutePath(),
                                    "--inhibit-cache", "--library-path", dir.getAbsolutePath(), executable, "fixture"};
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
