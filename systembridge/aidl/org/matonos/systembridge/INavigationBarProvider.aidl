package org.matonos.systembridge;

import android.os.IBinder;
import org.matonos.systembridge.INavigationBarHostCallback;

/** Private IPC contract used by MatonOSClient's typed navigation provider adapter. */
interface INavigationBarProvider {
    void attach(IBinder hostToken, int displayId, int widthPx, int heightPx,
            INavigationBarHostCallback callback);
    void detach();
}
