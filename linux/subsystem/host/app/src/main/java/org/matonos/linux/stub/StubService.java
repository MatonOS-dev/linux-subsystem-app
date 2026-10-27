package org.matonos.linux.stub;

import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;
import android.util.Log;

import org.matonos.linux.PerAppRuntime;

import java.io.File;

/**
 * Per-app runtime host. Declared by every generated stub and loaded from the
 * shared library, so it runs in the app's own process and UID.
 *
 * <p>It hosts the app's session bus + portal in-process via
 * {@link PerAppRuntime}; the privileged compositor app is not involved in
 * parsing the app's D-Bus.
 */
public final class StubService extends Service {
    private static final String ACTION_QUIT = "org.matonos.linuxhost.action.QUIT";
    private static final int NOTIFICATION_ID = 1;
    private PerAppRuntime runtime;
    // Same process as the stub's activity; exposed so the activity can drive its
    // windows/Surface/input from the in-process runtime.
    private static volatile PerAppRuntime sRuntime;
    static PerAppRuntime runtime() { return sRuntime; }

    static void updateWindowNotification(android.content.Context context, boolean hasWindows) {
        try {
            android.app.NotificationManager manager = context.getSystemService(android.app.NotificationManager.class);
            String channel = "linux-runtime";
            android.app.Notification.Builder builder = new android.app.Notification.Builder(context, channel)
                    .setContentTitle(context.getApplicationInfo().loadLabel(context.getPackageManager()))
                    .setSmallIcon(android.R.drawable.stat_sys_download)
                    .setOngoing(true);
            if (!hasWindows) {
                android.content.Intent quit = new android.content.Intent(context, StubService.class).setAction(ACTION_QUIT);
                android.app.PendingIntent action = android.app.PendingIntent.getService(context, NOTIFICATION_ID,
                        quit, android.app.PendingIntent.FLAG_UPDATE_CURRENT | android.app.PendingIntent.FLAG_IMMUTABLE);
                builder.setContentText("is still running")
                        .addAction(new android.app.Notification.Action.Builder(
                                android.R.drawable.ic_menu_close_clear_cancel, "Quit", action).build());
            }
            manager.notify(NOTIFICATION_ID, builder.build());
        } catch (Exception error) {
            Log.w("MatonStub", "Cannot update runtime notification", error);
        }
    }

    @Override public void onCreate() {
        super.onCreate();
        // Promote while visible so minimization and activity recreation keep
        // the runtime alive. A promotion denial must not crash the app.
        try {
            String channel = "linux-runtime";
            android.app.NotificationManager manager = getSystemService(android.app.NotificationManager.class);
            manager.createNotificationChannel(new android.app.NotificationChannel(
                    channel, "Linux application", android.app.NotificationManager.IMPORTANCE_LOW));
            android.app.Notification notification = new android.app.Notification.Builder(this, channel)
                    .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
                    .setSmallIcon(android.R.drawable.stat_sys_download)
                    .setOngoing(true)
                    .build();
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } catch (Exception error) {
            Log.w("MatonStub", "foreground service not allowed; running as a plain service", error);
        }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_QUIT.equals(intent.getAction())) {
            V3Launcher.requestQuit();
            return START_NOT_STICKY;
        }
        String ref = intent != null ? intent.getStringExtra(HostContract.EXTRA_FLATPAK_REF) : null;
        if (ref == null) {
            try {
                android.content.pm.ServiceInfo info = getPackageManager().getServiceInfo(
                        new android.content.ComponentName(this, StubService.class),
                        android.content.pm.PackageManager.GET_META_DATA);
                if (info.metaData != null) ref = info.metaData.getString(HostContract.META_FLATPAK_REF);
            } catch (Exception ignored) { }
        }
        if (runtime == null && ref != null) {
            File directory = new File(getDataDir(), "runtime").getAbsoluteFile();
            if (!directory.isDirectory() && !directory.mkdirs()) {
                stopSelf();
                return START_NOT_STICKY;
            }
            try {
                directory = directory.getCanonicalFile();
                android.system.Os.chmod(directory.getAbsolutePath(), 0700);
                runtime = new PerAppRuntime(this, directory, ref);
                sRuntime = runtime;
            } catch (Exception error) {
                Log.e("MatonStub", "per-app runtime failed to start", error);
                stopSelf();
                return START_NOT_STICKY;
            }
        }
        // Only an explicit app launch starts the runtime. In particular,
        // closing the last window ends this process and must not resurrect
        // a headless compositor or Linux session through a sticky restart.
        return START_NOT_STICKY;
    }

    @Override public void onDestroy() {
        if (runtime != null) {
            runtime.close();
            runtime = null;
        }
        sRuntime = null;
        super.onDestroy();
    }

    @Override public IBinder onBind(Intent intent) { return null; }

}
