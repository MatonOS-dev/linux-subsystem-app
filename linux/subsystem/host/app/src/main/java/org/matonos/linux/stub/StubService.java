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
    private PerAppRuntime runtime;
    // Same process as the stub's activity; exposed so the activity can drive its
    // windows/Surface/input from the in-process runtime.
    private static volatile PerAppRuntime sRuntime;
    static PerAppRuntime runtime() { return sRuntime; }

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
            startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } catch (Exception error) {
            Log.w("MatonStub", "foreground service not allowed; running as a plain service", error);
        }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
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
