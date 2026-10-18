package org.matonos.linux.bootstrap;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.PowerManager;
import android.util.Log;

import java.io.InputStream;
import java.io.OutputStream;

public final class BootstrapReceiver extends BroadcastReceiver {
    private static final String TAG = "MatonBootstrap";
    private static final String ACTION_BOOTSTRAP = "org.matonos.linux.BOOTSTRAP";
    private static final String HOST_PACKAGE = "org.matonos.linux";
    private static PowerManager.WakeLock installWakeLock;

    @Override public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        if (!Intent.ACTION_BOOT_COMPLETED.equals(action) && !ACTION_BOOTSTRAP.equals(action)) return;
        // Only the bootstrap APK has this receiver: if it runs, the real host is not installed.
        final PendingResult pending = goAsync();
        Thread worker = new Thread(() -> install(context, pending), "MatonBootstrapInstall");
        worker.start();
    }

    private static void install(Context context, PendingResult pending) {
        PowerManager.WakeLock lock = null;
        int sessionId = -1;
        try {
            PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
            lock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MatonOS:SubsystemBootstrap");
            synchronized (BootstrapReceiver.class) { installWakeLock = lock; }
            lock.acquire(5 * 60 * 1000L);
            PackageInstaller installer = context.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(HOST_PACKAGE);
            params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED);
            sessionId = installer.createSession(params);
            try (PackageInstaller.Session session = installer.openSession(sessionId)) {
                // The write stream must be closed before commit ("Files still open").
                try (InputStream input = context.getAssets().open("MatonLinuxSubsystem.apk");
                     OutputStream output = session.openWrite("base.apk", 0, -1)) {
                    byte[] buffer = new byte[64 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                    session.fsync(output);
                }
                Intent result = new Intent(context, BootstrapResultReceiver.class)
                        .setAction(BootstrapResultReceiver.ACTION_RESULT);
                android.app.PendingIntent callback = android.app.PendingIntent.getBroadcast(context, 0, result,
                        android.app.PendingIntent.FLAG_UPDATE_CURRENT | android.app.PendingIntent.FLAG_MUTABLE);
                Log.i(TAG, "Committing embedded subsystem APK, session=" + sessionId);
                session.commit(callback.getIntentSender());
                sessionId = -1;
                lock = null; // The result receiver releases it; the timeout covers process replacement.
            }
        } catch (Exception e) {
            Log.e(TAG, "Embedded subsystem install failed", e);
            if (sessionId >= 0) {
                try { context.getPackageManager().getPackageInstaller().abandonSession(sessionId); }
                catch (Exception ignored) { }
            }
        } finally {
            if (lock != null && lock.isHeld()) {
                synchronized (BootstrapReceiver.class) { installWakeLock = null; }
                lock.release();
            }
            pending.finish();
        }
    }

}
