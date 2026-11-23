package org.matonos.linux;

import android.app.Service;
import android.content.Intent;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.util.Log;
import android.view.Display;
import org.matonos.appsettings.IEmbeddedSettings;
import org.matonos.appsettings.IEmbeddedSettingsHost;

/** Embedded D-Bus handler selection page for the Settings app. */
public final class EmbeddedSettingsService extends Service {
    private static final String TAG = "MatonDBusSettings";
    private final Handler main = new Handler(Looper.getMainLooper());
    private DbusSettingsSession session;
    private final IEmbeddedSettings.Stub binder = new IEmbeddedSettings.Stub() {
        @Override public int getVersion() { return VERSION; }
        @Override public void open(IBinder token, int displayId, int width, int height, IEmbeddedSettingsHost host) {
            main.post(() -> openSession(token, displayId, width, height, host));
        }
        @Override public void resize(int width, int height) {
            main.post(() -> {
                if (session == null) { Log.e(TAG, "resize failed: no open settings surface"); return; }
                try { session.resize(width, height); }
                catch (RuntimeException error) { Log.e(TAG, "resize failed width=" + width + " height=" + height, error); }
            });
        }
        @Override public void close() {
            main.post(() -> { if (session != null) { session.release(); session = null; } });
        }
    };

    @Override public IBinder onBind(Intent intent) { return binder; }
    @Override public void onDestroy() {
        if (session != null) { session.release(); session = null; }
        super.onDestroy();
    }

    private void openSession(IBinder token, int displayId, int width, int height, IEmbeddedSettingsHost host) {
        try {
            if (session != null) session.release();
            Display display = getSystemService(android.hardware.display.DisplayManager.class).getDisplay(displayId);
            if (display == null) throw new IllegalStateException("displayId=" + displayId + " missing");
            session = new DbusSettingsSession(this, display, token, host);
            session.resize(width, height);
        } catch (Exception error) {
            Log.e(TAG, "open failed displayId=" + displayId + " width=" + width + " height=" + height, error);
            session = null;
        }
    }
}
