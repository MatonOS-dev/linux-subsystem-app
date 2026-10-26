package org.matonos.linux.stub;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/** Creates disposable files in the host UID and hands file fds to an isolated service. */
public final class ExtraProbeActivity extends Activity {
    private static final String TAG = "MatonExtraProbe";
    public static final String ACTION = "org.matonos.linux.DEBUG_EXTRA_PROBE";
    private ServiceConnection connection;
    private ParcelFileDescriptor fileFd;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (!ACTION.equals(getIntent().getAction())) {
            Log.e(TAG, "Refusing unknown probe action"); finish(); return;
        }
        try {
            File root = new File(getCacheDir(), "extra-probe");
            delete(root); if (!root.mkdirs()) throw new java.io.IOException("Cannot prepare probe directory");
            File probe = new File(root, "probe.txt");
            try (FileOutputStream out = new FileOutputStream(probe)) {
                out.write("regular-file-stream-ok\n".getBytes(StandardCharsets.UTF_8));
            }
            fileFd = ParcelFileDescriptor.open(probe, ParcelFileDescriptor.MODE_READ_ONLY);
            connection = new ServiceConnection() {
                @Override public void onServiceConnected(ComponentName name, IBinder binder) {
                    try {
                        Log.i(TAG, "Connected to isolated service");
                        IExtraProbe.Stub.asInterface(binder).run(fileFd);
                    } catch (Exception error) { Log.e(TAG, "Could not call isolated probe", error); finish(); }
                }
                @Override public void onServiceDisconnected(ComponentName name) {
                    Log.e(TAG, "Isolated probe service disconnected"); finish();
                }
            };
            if (!bindService(new Intent(this, ExtraProbeService.class), connection, BIND_AUTO_CREATE))
                throw new java.io.IOException("Could not bind isolated probe service");
            Log.i(TAG, "Started; service should report regular-file stream, writable locations, loader and bwrap results");
        } catch (Exception error) { Log.e(TAG, "Probe setup failed", error); finish(); }
    }

    @Override protected void onDestroy() {
        if (connection != null) try { unbindService(connection); } catch (Exception ignored) { }
        close(fileFd); super.onDestroy();
    }
    private static void close(ParcelFileDescriptor fd) { if (fd != null) try { fd.close(); } catch (Exception ignored) { } }
    private static void delete(File file) {
        if (file.isDirectory()) { File[] children=file.listFiles(); if(children!=null)for(File child:children)delete(child); }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }
}
