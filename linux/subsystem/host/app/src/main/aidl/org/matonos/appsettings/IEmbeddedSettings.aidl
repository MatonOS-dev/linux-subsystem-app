package org.matonos.appsettings;

import org.matonos.appsettings.IEmbeddedSettingsHost;
import android.os.IBinder;

interface IEmbeddedSettings {
    const int VERSION = 1;
    int getVersion();
    void open(in IBinder hostToken, int displayId, int widthPx, int heightPx, IEmbeddedSettingsHost host);
    void resize(int widthPx, int heightPx);
    void close();
}
