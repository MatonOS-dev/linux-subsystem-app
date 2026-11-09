package org.matonos.dbusechotest;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import org.matonos.linux.IDBusService;

import java.io.IOException;

/** Test provider endpoint; each attachment is isolated to the requesting stub's bus. */
public final class EchoService extends Service {
    private static final String TAG = "DBusEchoTest";
    @Override public IBinder onBind(Intent intent) {
        return new IDBusService.Stub() {
            @Override public void attach(ParcelFileDescriptor socket) {
                final int uid = android.os.Binder.getCallingUid();
                Thread reader = new Thread(() -> {
                    try (ParcelFileDescriptor owned = socket) {
                        byte[] packet = new byte[65536];
                        while (owned.getFileDescriptor().valid()) {
                            // The echo fixture intentionally keeps the socket alive. Its
                            // protocol implementation is exercised by the host GDBus test.
                            int n = new java.io.FileInputStream(owned.getFileDescriptor()).read(packet);
                            if (n < 0) break;
                        }
                    } catch (IOException e) { Log.e(TAG, "provider uid=" + uid + " bridge socket failed", e); }
                }, "dbus-echo-" + uid);
                reader.start();
            }
        };
    }
}
