package org.matonos.linux.stub;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;
import org.matonos.systembridge.ILinuxd;

/** Compatibility action for an already installed, device-signed stub. New
 * installs are owned by the bridge and create this APK after deployment.
 * linuxd authenticates this package and uses FPINSTALL for network staging. */
public class InstallActivity extends Activity {
    private static final String TAG = "MatonStubInstall";
    private static final String LINUXD = "org.matonos.systembridge.ILinuxd/default";
    /** Optional operation ID from the store, so it can follow progress. */
    public static final String EXTRA_OPERATION_ID = "org.matonos.linuxhost.OPERATION_ID";

    protected boolean update() { return false; }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        String operation = getIntent().getStringExtra(EXTRA_OPERATION_ID);
        final String operationId = operation == null ? "" : operation;
        final String pkg = getPackageName();
        final Intent download = new Intent(this, InstallService.class);
        final PowerManager.WakeLock wakeLock = getSystemService(PowerManager.class)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MatonOS:FlatpakInstall");
        wakeLock.acquire();
        try { startForegroundService(download); }
        catch (RuntimeException | Error error) { wakeLock.release(); throw error; }
        try { new Thread(() -> {
            try {
                // ServiceManager is hidden API; this lookup is the stub's only use of it.
                IBinder binder = (IBinder) Class.forName("android.os.ServiceManager")
                        .getMethod("checkService", String.class).invoke(null, LINUXD);
                ILinuxd linuxd = ILinuxd.Stub.asInterface(binder);
                if (linuxd == null) throw new IllegalStateException("linuxd unavailable");
                String result = update() ? linuxd.updateSelf(pkg, operationId) : linuxd.installSelf(pkg, operationId);
                Log.i(TAG, (update() ? "update me: " : "install me: ") + result);
            } catch (Exception error) { Log.e(TAG, "Cannot start the Flatpak install", error); }
            finally {
                try { stopService(download); }
                finally { wakeLock.release(); }
            }
            runOnUiThread(this::finish);
        }).start(); }
        catch (RuntimeException | Error error) {
            try { stopService(download); }
            finally { wakeLock.release(); }
            throw error;
        }
    }
}
