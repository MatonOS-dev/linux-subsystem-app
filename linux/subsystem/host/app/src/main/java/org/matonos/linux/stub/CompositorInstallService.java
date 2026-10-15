package org.matonos.linux.stub;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Binder;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;
import org.matonos.systembridge.ICompositorInstall;
import org.matonos.systembridge.ICompositorInstallCallback;

/**
 * The subsystem's install worker endpoint.
 *
 * <p>Only this app's own store coordinator may drive this service: the caller
 * UID must match the app UID resolved through PackageManager. The service owns at
 * most one transaction, keeps a wake lock and the progress notification for its
 * lifetime, and never sees a caller-supplied path, loader flag or final
 * identity. Everything else lives in {@link InstallTransaction}.
 */
public final class CompositorInstallService extends Service {
    private static final String TAG = "MatonInstall";
    private static final String CHANNEL = "linux-install";
    private static final int NOTIFICATION_ID = 3;
    private static final String SUBSYSTEM_PACKAGE = "org.matonos.linux";
    private static final String WAKE_LOCK = "MatonOS:CompositorInstall";

    private final Object lock = new Object();
    private InstallTransaction active;
    private PowerManager.WakeLock wakeLock;

    private final ICompositorInstall.Stub binder = new ICompositorInstall.Stub() {
        @Override public void startInstall(ICompositorInstallCallback callback, String operationId,
                String appRef, String remote) {
            requireSubsystem();
            InstallTransaction transaction = InstallTransaction.create(
                    CompositorInstallService.this, callback, operationId, appRef, remote);
            synchronized (lock) {
                if (active != null) throw new IllegalStateException("Another install is active");
                active = transaction;
            }
            try {
                beginForeground();
                transaction.start(() -> finish(transaction));
            } catch (RuntimeException error) {
                // Clear the notification and wake lock of a start that failed;
                // the bridge fails the transaction and staging is collected.
                finish(transaction);
                throw error;
            }
        }

        @Override public void cancelInstall(String operationId) {
            requireSubsystem();
            InstallTransaction transaction;
            synchronized (lock) { transaction = active; }
            if (transaction != null) transaction.cancel(operationId);
        }
    };

    @Override public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override public void onDestroy() {
        InstallTransaction transaction;
        synchronized (lock) { transaction = active; active = null; }
        if (transaction != null) transaction.cancel();
        releaseWakeLock();
        super.onDestroy();
    }

    /** PackageManager, not caller supplied metadata, owns the service identity. */
    private void requireSubsystem() {
        int subsystem;
        try {
            subsystem = getPackageManager().getApplicationInfo(SUBSYSTEM_PACKAGE, 0).uid;
        } catch (PackageManager.NameNotFoundException missing) {
            throw new SecurityException("Linux Subsystem app is not installed");
        }
        if (Binder.getCallingUid() != subsystem)
            throw new SecurityException("Only the Linux Subsystem app may run the installer");
    }

    private void beginForeground() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                "Linux application downloads", NotificationManager.IMPORTANCE_LOW));
        Notification notification = new Notification.Builder(this, CHANNEL)
                .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
                .setContentText("Downloading Linux application")
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true).build();
        startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        // Held from the first worker step to the completion callback, so a
        // screen-off or low-power idle never leaves staging half-written.
        PowerManager power = getSystemService(PowerManager.class);
        PowerManager.WakeLock lock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, WAKE_LOCK);
        lock.setReferenceCounted(false);
        lock.acquire();
        synchronized (this) { wakeLock = lock; }
    }

    private void releaseWakeLock() {
        PowerManager.WakeLock lock;
        synchronized (this) { lock = wakeLock; wakeLock = null; }
        if (lock != null && lock.isHeld()) lock.release();
    }

    /** Runs on the worker thread once for the transaction that just finished. */
    private void finish(InstallTransaction transaction) {
        synchronized (lock) { if (active == transaction) active = null; }
        releaseWakeLock();
        try { stopForeground(STOP_FOREGROUND_REMOVE); }
        catch (RuntimeException error) { Log.w(TAG, "Cannot clear the install notification", error); }
        stopSelf();
    }
}
