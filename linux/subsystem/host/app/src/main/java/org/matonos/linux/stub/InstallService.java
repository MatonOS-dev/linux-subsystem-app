package org.matonos.linux.stub;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.IBinder;

/** Shows progress for compatibility self-update calls. Network staging uses
 * the subsystem app's own identity; this service owns no install choices. */
public final class InstallService extends Service {
    @Override public void onCreate() {
        super.onCreate();
        String channel = "linux-install";
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(channel,
                "Linux application downloads", NotificationManager.IMPORTANCE_LOW));
        Notification notification = new Notification.Builder(this, channel)
                .setContentTitle(getApplicationInfo().loadLabel(getPackageManager()))
                .setContentText("Downloading Linux application")
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true).build();
        startForeground(2, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        return START_NOT_STICKY;
    }

    @Override public IBinder onBind(Intent intent) { return null; }
}
